#pragma once

#include <chrono>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ds4_output_guard {
public:
    explicit ds4_output_guard(int64_t max_case_ms)
        : max_case_ms(max_case_ms), started(std::chrono::steady_clock::now()) {}

    bool inspect(const std::string & delta, std::string & reason) {
        recent += delta;
        if (recent.size() > 32768) recent.erase(0, recent.size() - 32768);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        if (elapsed > max_case_ms) {
            reason = "case_time_limit";
            return true;
        }
        if (recent.size() < 2048 || recent.size() - last_checked_size < 256) return false;
        last_checked_size = recent.size();

        std::vector<std::string> words;
        std::string word;
        for (unsigned char c : recent) {
            if (std::isalnum(c)) {
                word += (char) std::tolower(c);
            } else if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
        }
        if (!word.empty()) words.push_back(std::move(word));
        if (words.size() < 128) return false;
        if (words.size() > 320) words.erase(words.begin(), words.end() - 320);

        std::set<std::string> grams;
        for (size_t i = 0; i + 3 < words.size(); i++) {
            grams.insert(words[i] + " " + words[i + 1] + " " + words[i + 2] + " " + words[i + 3]);
        }
        const size_t total = words.size() - 3;
        if (total >= 125 && grams.size() * 100 < total * 42) {
            reason = "repetitive_output";
            return true;
        }
        return false;
    }

private:
    int64_t max_case_ms;
    std::chrono::steady_clock::time_point started;
    size_t last_checked_size = 0;
    std::string recent;
};
