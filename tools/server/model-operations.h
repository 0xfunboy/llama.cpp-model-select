#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class model_operation_coordinator {
  public:
    class lease {
      public:
        lease() = default;
        lease(const lease &) = delete;
        lease & operator=(const lease &) = delete;
        lease(lease && other) noexcept;
        lease & operator=(lease && other) noexcept;
        ~lease();
        explicit operator bool() const { return owner_ != nullptr; }

      private:
        friend class model_operation_coordinator;
        lease(model_operation_coordinator * owner, uint64_t id) : owner_(owner), id_(id) {}
        model_operation_coordinator * owner_ = nullptr;
        uint64_t id_ = 0;
    };

    lease acquire(
        const std::string & kind,
        int priority,
        const std::function<bool()> & cancelled,
        std::chrono::milliseconds timeout = std::chrono::minutes(30));
    bool busy() const;
    std::string active_kind() const;

  private:
    struct waiter {
        uint64_t id;
        uint64_t sequence;
        int priority;
        std::string kind;
    };
    void release(uint64_t id);
    bool is_next(uint64_t id) const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<waiter> waiters_;
    uint64_t next_id_ = 1;
    uint64_t next_sequence_ = 1;
    uint64_t active_id_ = 0;
    std::string active_kind_;
};
