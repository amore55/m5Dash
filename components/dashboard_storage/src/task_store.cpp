#include "dashboard/storage/task_store.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "dashboard/json_compat.hpp"
#include "dashboard/json_util.hpp"
#include "dashboard/storage/fs.hpp"

namespace dashboard::storage {
namespace {

constexpr const char* kTag = "tasks";
constexpr const char* kPath = "/store/tasks.json";

/// Generous ceiling for the serialised document: kMaxTasks (200) x ~300 bytes of JSON.
/// Allocated transiently from PSRAM, not the stack.
constexpr size_t kMaxDocumentBytes = 96u * 1024u;

}  // namespace

// ---------------------------------------------------------------------------------------
// Task helpers
// ---------------------------------------------------------------------------------------

const char* toString(TaskStatus value) { return value == TaskStatus::Done ? "done" : "open"; }

const char* toString(TaskPriority value) {
    switch (value) {
        case TaskPriority::High:
            return "high";
        case TaskPriority::Low:
            return "low";
        case TaskPriority::Normal:
            break;
    }
    return "normal";
}

const char* toString(TaskSource value) {
    return value == TaskSource::Device ? "device" : "telegram";
}

TaskStatus taskStatusFromString(const char* text) {
    return (text != nullptr && std::strcmp(text, "done") == 0) ? TaskStatus::Done
                                                              : TaskStatus::Open;
}

TaskPriority taskPriorityFromString(const char* text) {
    if (text != nullptr) {
        if (std::strcmp(text, "high") == 0) {
            return TaskPriority::High;
        }
        if (std::strcmp(text, "low") == 0) {
            return TaskPriority::Low;
        }
    }
    return TaskPriority::Normal;
}

TaskSource taskSourceFromString(const char* text) {
    return (text != nullptr && std::strcmp(text, "device") == 0) ? TaskSource::Device
                                                                 : TaskSource::Telegram;
}

void Task::shortId(char* out, size_t capacity) const {
    if (out == nullptr || capacity == 0) {
        return;
    }
    // Last four characters of the id. Enough to be unambiguous in a list of at most 200, and
    // short enough that "/done 4821" is a reasonable thing to ask someone to type.
    const size_t len = id.size();
    const char* start = (len > 4) ? id.c_str() + (len - 4) : id.c_str();
    std::snprintf(out, capacity, "%s", start);
}

// ---------------------------------------------------------------------------------------
// TaskStore
// ---------------------------------------------------------------------------------------

const char* TaskStore::path() { return kPath; }

esp_err_t TaskStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = 0;

    if (!Fs::exists(kPath)) {
        ESP_LOGI(kTag, "no task file yet; starting empty");
        return ESP_OK;
    }

    auto* buffer = static_cast<char*>(heap_caps_malloc(kMaxDocumentBytes, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) {
        ESP_LOGE(kTag, "could not allocate a read buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    esp_err_t err = Fs::readAll(kPath, buffer, kMaxDocumentBytes, &len);
    if (err != ESP_OK) {
        heap_caps_free(buffer);
        ESP_LOGE(kTag, "could not read %s: %s", kPath, esp_err_to_name(err));
        return err;
    }

    json::Doc doc;
    const bool parsed = doc.parse(buffer, len);
    heap_caps_free(buffer);

    if (!parsed) {
        // Leave the file alone. It may be recoverable by hand, and overwriting someone's task
        // list because of one bad byte would turn a bad session into a lost one.
        ESP_LOGE(kTag, "%s is not valid JSON; starting empty and leaving the file in place",
                 kPath);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON* array = json::array(doc.root(), "tasks");
    const size_t total = json::arraySize(array);
    for (size_t i = 0; i < total && count_ < tasks_.size(); ++i) {
        const cJSON* node = json::at(array, i);
        Task task;
        // A record without an id is unusable — it could never be completed or deleted — so it
        // is dropped rather than given a synthetic one.
        if (!json::string(node, "id", task.id) || task.id.empty()) {
            continue;
        }
        json::string(node, "title", task.title);

        char scratch[24];
        if (json::string(node, "status", scratch, sizeof(scratch))) {
            task.status = taskStatusFromString(scratch);
        }
        if (json::string(node, "priority", scratch, sizeof(scratch))) {
            task.priority = taskPriorityFromString(scratch);
        }
        if (json::string(node, "source", scratch, sizeof(scratch))) {
            task.source = taskSourceFromString(scratch);
        }
        json::string(node, "category", task.category);
        json::integer64(node, "created_at", task.created_at);
        json::integer64(node, "due_at", task.due_at);
        json::integer64(node, "completed_at", task.completed_at);

        tasks_[count_++] = task;
    }

    if (total > tasks_.size()) {
        ESP_LOGW(kTag, "file holds %u tasks but the limit is %u; extras dropped",
                 static_cast<unsigned>(total), static_cast<unsigned>(tasks_.size()));
    }
    ESP_LOGI(kTag, "loaded %u tasks (%u open)", static_cast<unsigned>(count_),
             static_cast<unsigned>(openCountLocked()));
    return ESP_OK;
}

esp_err_t TaskStore::saveLocked() {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    cJSON* array = cJSON_AddArrayToObject(root, "tasks");
    if (array == nullptr) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count_; ++i) {
        const Task& task = tasks_[i];
        cJSON* node = cJSON_CreateObject();
        if (node == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(node, "id", task.id.c_str());
        cJSON_AddStringToObject(node, "title", task.title.c_str());
        cJSON_AddStringToObject(node, "status", toString(task.status));
        cJSON_AddStringToObject(node, "priority", toString(task.priority));
        cJSON_AddStringToObject(node, "category", task.category.c_str());
        cJSON_AddNumberToObject(node, "created_at", static_cast<double>(task.created_at));
        cJSON_AddNumberToObject(node, "due_at", static_cast<double>(task.due_at));
        cJSON_AddNumberToObject(node, "completed_at", static_cast<double>(task.completed_at));
        cJSON_AddStringToObject(node, "source", toString(task.source));
        cJSON_AddItemToArray(array, node);
    }

    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = Fs::writeAtomic(kPath, text, std::strlen(text));
    cJSON_free(text);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "could not save tasks: %s", esp_err_to_name(err));
    }
    return err;
}

int TaskStore::findIndexLocked(const char* id) const {
    if (id == nullptr || id[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (tasks_[i].id.equals(id)) {
            return static_cast<int>(i);
        }
    }
    // Fall back to the short id, so a Telegram command can use the 4-character form shown in
    // /list without the user having to type the full identifier.
    for (size_t i = 0; i < count_; ++i) {
        char shortId[8];
        tasks_[i].shortId(shortId, sizeof(shortId));
        if (std::strcmp(shortId, id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

esp_err_t TaskStore::upsert(const Task& task) {
    if (task.id.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    const int existing = findIndexLocked(task.id.c_str());
    if (existing >= 0) {
        tasks_[existing] = task;
        return saveLocked();
    }

    if (count_ >= tasks_.size()) {
        // Loud, not silent. Dropping the oldest task to make room would quietly lose work the
        // user asked to keep; refusing lets the caller tell them.
        ESP_LOGE(kTag, "task list is full (%u); refusing to add",
                 static_cast<unsigned>(tasks_.size()));
        return ESP_ERR_NO_MEM;
    }
    tasks_[count_++] = task;
    return saveLocked();
}

esp_err_t TaskStore::complete(const char* id, int64_t when) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = findIndexLocked(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (tasks_[index].status == TaskStatus::Done) {
        return ESP_OK;  // idempotent
    }
    tasks_[index].status = TaskStatus::Done;
    tasks_[index].completed_at = when;
    return saveLocked();
}

esp_err_t TaskStore::reopen(const char* id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = findIndexLocked(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    tasks_[index].status = TaskStatus::Open;
    tasks_[index].completed_at = 0;
    return saveLocked();
}

esp_err_t TaskStore::remove(const char* id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = findIndexLocked(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = static_cast<size_t>(index); i + 1 < count_; ++i) {
        tasks_[i] = tasks_[i + 1];
    }
    --count_;
    tasks_[count_] = Task{};
    return saveLocked();
}

esp_err_t TaskStore::clearCompleted(size_t* removed) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t write = 0;
    for (size_t read = 0; read < count_; ++read) {
        if (tasks_[read].status != TaskStatus::Done) {
            tasks_[write++] = tasks_[read];
        }
    }
    const size_t dropped = count_ - write;
    for (size_t i = write; i < count_; ++i) {
        tasks_[i] = Task{};
    }
    count_ = write;
    if (removed != nullptr) {
        *removed = dropped;
    }
    if (dropped == 0) {
        return ESP_OK;
    }
    return saveLocked();
}

size_t TaskStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

size_t TaskStore::openCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return openCountLocked();
}

size_t TaskStore::openCountLocked() const {
    size_t open = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (tasks_[i].open()) {
            ++open;
        }
    }
    return open;
}

bool TaskStore::full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ >= tasks_.size();
}

bool TaskStore::at(size_t index, Task& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= count_) {
        return false;
    }
    out = tasks_[index];
    return true;
}

bool TaskStore::find(const char* id, Task& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = findIndexLocked(id);
    if (index < 0) {
        return false;
    }
    out = tasks_[static_cast<size_t>(index)];
    return true;
}

size_t TaskStore::snapshot(Task* out, size_t capacity, bool open_only) const {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t written = 0;
    for (size_t i = 0; i < count_ && written < capacity; ++i) {
        if (open_only && !tasks_[i].open()) {
            continue;
        }
        out[written++] = tasks_[i];
    }
    return written;
}

}  // namespace dashboard::storage
