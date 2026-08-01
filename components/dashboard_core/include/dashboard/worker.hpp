// A single-threaded job queue: one FreeRTOS task, one bounded queue.
//
// Every plugin owns one of these. It is the mechanism that keeps HTTPS calls, JSON parsing and
// filesystem writes off the LVGL thread, which is the single most important structural
// property of this application.
//
// Design notes that matter on an embedded target:
//
//   * The queue is SMALL (default 2). Refresh requests are idempotent, so dropping one when a
//     fetch is already queued is the correct behaviour, not a failure — it coalesces a user
//     hammering pull-to-refresh into one network call.
//   * Jobs are heap-allocated std::function objects, but the number in flight is capped by the
//     queue depth, so total allocation is bounded. post() returns false rather than growing.
//   * The task blocks on xQueueReceive with an infinite timeout, so it consumes no CPU when
//     idle and never trips the task watchdog (it is not a watchdog subscriber).
//   * stop() is synchronous: it waits for the task to actually exit before returning, so a
//     destructor cannot race a running job.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace dashboard {

class Worker {
  public:
    using Job = std::function<void()>;

    /// Priority below the LVGL task, so UI rendering always wins over a background fetch.
    static constexpr int kDefaultPriority = 4;
    static constexpr uint32_t kDefaultStackBytes = 8192;
    static constexpr size_t kDefaultQueueDepth = 2;

    Worker() = default;
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// `name` is used verbatim as the FreeRTOS task name, so keep it short (<16 chars) — it is
    /// what shows up in a task dump or a watchdog report.
    /// `stack_bytes` must accommodate mbedtls: a TLS handshake needs several KB.
    esp_err_t start(const char* name, uint32_t stack_bytes = kDefaultStackBytes,
                    int priority = kDefaultPriority, size_t queue_depth = kDefaultQueueDepth);

    /// Blocks until the task has finished its current job and exited. Idempotent.
    void stop();

    /// Enqueue a job. Returns false if the worker is not running or the queue is full.
    /// A false return is normal under load and callers should treat it as "already scheduled".
    bool post(Job job);

    bool running() const { return task_ != nullptr; }

    /// Jobs waiting, excluding one currently executing.
    size_t pending() const;

    /// True while a job is executing. Used by the OTA service to refuse to start an update
    /// while a flash-touching job is in progress.
    bool busy() const { return busy_; }

  private:
    static void taskEntry(void* arg);
    void run();

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t exited_ = nullptr;
    volatile bool stopping_ = false;
    volatile bool busy_ = false;
};

}  // namespace dashboard
