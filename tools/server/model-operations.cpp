#include "model-operations.h"

#include <algorithm>

model_operation_coordinator::lease::lease(lease && other) noexcept
        : owner_(other.owner_), id_(other.id_) {
    other.owner_ = nullptr;
    other.id_ = 0;
}

model_operation_coordinator::lease & model_operation_coordinator::lease::operator=(lease && other) noexcept {
    if (this == &other) return *this;
    if (owner_) owner_->release(id_);
    owner_ = other.owner_;
    id_ = other.id_;
    other.owner_ = nullptr;
    other.id_ = 0;
    return *this;
}

model_operation_coordinator::lease::~lease() {
    if (owner_) owner_->release(id_);
}

bool model_operation_coordinator::is_next(uint64_t id) const {
    if (active_id_ != 0 || waiters_.empty()) return false;
    const auto best = std::max_element(waiters_.begin(), waiters_.end(), [](const waiter & a, const waiter & b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.sequence > b.sequence;
    });
    return best != waiters_.end() && best->id == id;
}

model_operation_coordinator::lease model_operation_coordinator::acquire(
        const std::string & kind,
        int priority,
        const std::function<bool()> & cancelled,
        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t id = next_id_++;
    waiters_.push_back({id, next_sequence_++, priority, kind});
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!is_next(id)) {
        if ((cancelled && cancelled()) || cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(), [&](const waiter & item) { return item.id == id; }), waiters_.end());
            cv_.notify_all();
            return {};
        }
    }
    active_id_ = id;
    active_kind_ = kind;
    waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(), [&](const waiter & item) { return item.id == id; }), waiters_.end());
    return lease(this, id);
}

void model_operation_coordinator::release(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_id_ != id) return;
    active_id_ = 0;
    active_kind_.clear();
    cv_.notify_all();
}

bool model_operation_coordinator::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_id_ != 0;
}

std::string model_operation_coordinator::active_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_kind_;
}
