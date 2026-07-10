#include "model-operations.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    model_operation_coordinator coordinator;
    auto root = coordinator.acquire("active", 1, []() { return false; }, std::chrono::seconds(1));
    if (!root || !coordinator.busy() || coordinator.active_kind() != "active") return 1;

    std::atomic<int> ready{0};
    std::mutex order_mutex;
    std::vector<std::string> order;
    auto contender = [&](const std::string & name, int priority) {
        ++ready;
        auto lease = coordinator.acquire(name, priority, []() { return false; }, std::chrono::seconds(2));
        if (!lease) return;
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(name);
    };
    std::thread low(contender, "low", 10);
    std::thread high(contender, "high", 20);
    while (ready.load() != 2) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    root = {};
    low.join();
    high.join();

    if (order.size() != 2 || order[0] != "high" || order[1] != "low" || coordinator.busy()) {
        std::cerr << "priority or release policy failed\n";
        return 1;
    }
    std::cout << "model operation coordinator tests passed\n";
    return 0;
}
