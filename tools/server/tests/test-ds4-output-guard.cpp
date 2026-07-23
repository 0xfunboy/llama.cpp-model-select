#include "ds4-output-guard.h"

#include <cstdlib>
#include <iostream>

static void require(bool value, const char * message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    std::string reason;
    ds4_output_guard repetition_guard(60000);
    const std::string repeated =
        "Now calculate the energy using mass and the speed of light before returning the same conclusion. ";
    bool stopped = false;
    for (int i = 0; i < 80 && !stopped; i++) stopped = repetition_guard.inspect(repeated, reason);
    require(stopped, "repetition must stop the stream");
    require(reason == "repetitive_output", "repetition reason");

    ds4_output_guard coherent_guard(60000);
    reason.clear();
    std::string coherent;
    for (int i = 0; i < 400; i++) {
        coherent += "step" + std::to_string(i) + " value" + std::to_string(i * 17) + " ";
    }
    require(!coherent_guard.inspect(coherent, reason), "varied output must continue");

    ds4_output_guard time_guard(-1);
    reason.clear();
    require(time_guard.inspect("x", reason), "expired case must stop the stream");
    require(reason == "case_time_limit", "time limit reason");

    std::cout << "DS4 output guard tests passed\n";
    return 0;
}
