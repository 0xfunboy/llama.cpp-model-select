#include "streaming-profiler.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
    const std::string trace =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}],\"timings\":{\"prompt_n\":10,\"predicted_n\":2}}\n\n"
        "data: [DONE]\n\n";
    const auto parsed = streaming_profiler::parse_sse_trace(trace);
    if (parsed.at("token_count") != 2 || parsed.at("timings").at("prompt_n") != 10) return 1;
    const double p95 = streaming_profiler::percentile_ms({10, 20, 30, 40, 50}, 0.95);
    if (std::abs(p95 - 48.0) > 0.001) return 1;
    const auto timeline = streaming_profiler::encode_timeline({
        {{"t_ms", 100.0}, {"process_rss_mib", 10.0}},
        {{"t_ms", 250.0}, {"process_rss_mib", 12.0}},
    });
    if (timeline.at("encoding") != "delta-columns-v1" || timeline.at("sample_count") != 2 ||
        timeline.at("rows").at(1).at(0) != 150.0) return 1;
    if (const char * model = std::getenv("STREAMING_PROFILE_MODEL"); model && model[0] != '\0') {
        const auto executable = std::filesystem::canonical("/proc/self/exe").parent_path() / "llama-server";
        const streaming_profiler::json item = {
            {"id", "integration"}, {"model", "integration"}, {"variant", "test"}, {"row_role", "candidate"},
            {"workload_kind", "baseline"}, {"model_path", model},
            {"extra_args", "--ctx-size 1024 --gpu-layers 0 --threads 8 --parallel 1"},
        };
        const streaming_profiler::json cfg = {
            {"hardware", {{"vram_total_mib", 0}}},
            {"bench", {{"n_predict", 2}, {"repetitions", 2}}},
        };
        const auto result = streaming_profiler::profile(item, cfg, executable);
        if (!result.value("ok", false) || result.value("benchmark_backend", std::string()) != "llama-server-streaming" ||
            result.value("run_count", 0) != 2 || result.value("evidence_level", std::string()) != "streaming-measured") return 1;
    }
    std::cout << "streaming profiler parser tests passed\n";
    return 0;
}
