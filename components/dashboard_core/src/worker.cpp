#include "dashboard/worker.hpp"

#include <cinttypes>
#include <new>

#include "esp_log.h"

namespace dashboard {
namespace {
constexpr const char* kTag = "worker";

/// The queue carries pointers to heap-allocated jobs rather than the std::function objects
/// themselves: a FreeRTOS queue byte-copies its items, which would be undefined behaviour for
/// a type with a non-trivial copy constructor.
using JobPtr = Worker::Job*;
}  // namespace

Worker::~Worker() { stop(); }

esp_err_t Worker::start(const char* name, uint32_t stack_bytes, int priority,
                        size_t queue_depth) {
    if (task_ != nullptr) {
        return ESP_OK;  // idempotent
    }
    if (name == nullptr || queue_depth == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    queue_ = xQueueCreate(queue_depth, sizeof(JobPtr));
    if (queue_ == nullptr) {
        ESP_LOGE(kTag, "%s: queue allocation failed", name);
        return ESP_ERR_NO_MEM;
    }

    exited_ = xSemaphoreCreateBinary();
    if (exited_ == nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    stopping_ = false;
    busy_ = false;

    // Stack is specified in bytes here and converted, because xTaskCreate takes words and
    // getting that wrong by a factor of 4 is a classic source of mysterious stack overflows.
    const uint32_t stack_words = stack_bytes / sizeof(StackType_t);
    if (xTaskCreate(&Worker::taskEntry, name, stack_words, this, priority, &task_) != pdPASS) {
        ESP_LOGE(kTag, "%s: task creation failed (stack %" PRIu32 " bytes)", name, stack_bytes);
        vSemaphoreDelete(exited_);
        exited_ = nullptr;
        vQueueDelete(queue_);
        queue_ = nullptr;
        task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(kTag, "%s started (stack %" PRIu32 " B, prio %d, depth %u)", name, stack_bytes,
             priority, static_cast<unsigned>(queue_depth));
    return ESP_OK;
}

void Worker::stop() {
    if (task_ == nullptr) {
        return;
    }
    stopping_ = true;

    // A null job is the sentinel that breaks the receive loop. Push it with a timeout: if the
    // queue is full the task is mid-job and will observe stopping_ on its next iteration
    // anyway, so failing to enqueue the sentinel is not fatal.
    JobPtr sentinel = nullptr;
    xQueueSend(queue_, &sentinel, pdMS_TO_TICKS(100));

    // Wait for the task to confirm it has left run(). Bounded so a wedged job cannot hang
    // shutdown forever; if it does time out we deliberately leak the task rather than delete
    // a task that may be inside mbedtls.
    if (xSemaphoreTake(exited_, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(kTag, "worker did not exit within 5 s; leaving it running");
        return;
    }

    task_ = nullptr;

    // Drain and free anything left queued, so stopping does not leak the pending jobs.
    JobPtr leftover = nullptr;
    while (xQueueReceive(queue_, &leftover, 0) == pdTRUE) {
        delete leftover;
    }
    vQueueDelete(queue_);
    queue_ = nullptr;
    vSemaphoreDelete(exited_);
    exited_ = nullptr;
}

bool Worker::post(Job job) {
    if (task_ == nullptr || queue_ == nullptr || stopping_ || !job) {
        return false;
    }
    // std::nothrow because the project builds with exceptions disabled: a throwing new would
    // abort rather than return, and we want a clean false.
    JobPtr held = new (std::nothrow) Job(std::move(job));
    if (held == nullptr) {
        return false;
    }
    if (xQueueSend(queue_, &held, 0) != pdTRUE) {
        // Queue full: an equivalent job is already scheduled. Dropping this one is correct.
        delete held;
        return false;
    }
    return true;
}

size_t Worker::pending() const {
    if (queue_ == nullptr) {
        return 0;
    }
    return static_cast<size_t>(uxQueueMessagesWaiting(queue_));
}

void Worker::taskEntry(void* arg) { static_cast<Worker*>(arg)->run(); }

void Worker::run() {
    JobPtr job = nullptr;
    while (!stopping_) {
        if (xQueueReceive(queue_, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job == nullptr) {
            break;  // stop sentinel
        }
        busy_ = true;
        (*job)();
        busy_ = false;
        delete job;
        job = nullptr;
    }

    // Signal stop() before deleting ourselves. Everything after xSemaphoreGive must be
    // self-contained, because stop() may already be tearing down the queue.
    SemaphoreHandle_t exited = exited_;
    if (exited != nullptr) {
        xSemaphoreGive(exited);
    }
    vTaskDelete(nullptr);
}

}  // namespace dashboard
