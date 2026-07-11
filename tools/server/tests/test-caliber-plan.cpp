#include "caliber-plan.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

using caliber::json;

namespace {

int failures = 0;

void require(bool condition, const std::string & message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void require_eq(const json & actual, const json & expected, const std::string & message) {
    if (actual != expected) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n"
                  << "  expected: " << expected.dump() << "\n"
                  << "  actual:   " << actual.dump() << "\n";
    }
}

json cfg() {
    return {
        {"hardware", {{"vram_budget_mib", 8000}, {"cpu_cores_physical", 6}, {"cpu_threads_logical", 12}, {"system_ram_available_mib", 32000}}},
        {"planning", {{"overhead_mib", 1200}, {"moecpu_sweep", {28, 30}}, {"offload_sweep", {20, 24}}, {"kv_rescue", {{"enabled", false}}}}},
        {"base_args", "--flash-attn auto --parallel 1 --batch-size 2048 --ubatch-size 512 --no-mmap --prio 2"},
        {"context_candidates", {{{"ctx", 16384}, {"kv", "q8_0"}}, {{"ctx", 32768}, {"kv", "q8_0"}}, {{"ctx", 65536}, {"kv", "q8_0"}}}},
        {"max_context_cap", 262144},
    };
}

std::vector<json> catalog() {
    return {
        {{"path", "C:\\models\\qwen3.5-4b-q4km.gguf"}, {"model", "Qwen3.5-4B"}, {"variant", "Q4_K_M"}, {"series", "qwen"}, {"size_mib", 3100}, {"is_moe", false}, {"mmproj", nullptr}, {"reasoning_mode", "off"}, {"template_note", nullptr}, {"gguf_context_length", 131072}, {"gguf_architecture", "qwen3"}},
        {{"path", "C:\\models\\qwen3.6-35b-a3b-q4km.gguf"}, {"model", "Qwen3.6-35B-A3B"}, {"variant", "Q4_K_M"}, {"series", "qwen"}, {"size_mib", 19000}, {"is_moe", true}, {"mmproj", nullptr}, {"reasoning_mode", "off"}, {"template_note", nullptr}, {"gguf_context_length", 262144}, {"gguf_architecture", "qwen3moe"}},
        {{"path", "C:\\models\\phi-4-reasoning-plus-q4km.gguf"}, {"model", "Phi-4-reasoning-plus"}, {"variant", "Q4_K_M"}, {"series", "phi"}, {"size_mib", 9200}, {"is_moe", false}, {"mmproj", nullptr}, {"reasoning_mode", "default"}, {"template_note", nullptr}, {"gguf_context_length", 131072}, {"gguf_architecture", "phi3"}},
    };
}

std::vector<json> models_catalog() {
    return {
        {{"id", "qwen4b"}, {"hf_file", "qwen3.5-4b-q4km.gguf"}, {"max_context", 131072}},
        {{"id", "qwen35b"}, {"hf_file", "qwen3.6-35b-a3b-q4km.gguf"}, {"max_context", 262144}},
        {{"id", "phi4"}, {"hf_file", "phi-4-reasoning-plus-q4km.gguf"}, {"max_context", 131072}},
    };
}

json presets() {
    return {
        {"middle", {{"label", "middle"}, {"models", {"qwen4b"}}}},
        {"ultra", {{"label", "ultra"}, {"models", {"qwen35b"}}}},
        {"high", {{"label", "high"}, {"models", {"phi4"}}}},
    };
}

void test_offload_estimator() {
    const double mib = 1024.0 * 1024.0;
    json result = caliber::estimate_initial_offload({
        {"size_mib", 105},
        {"gguf_block_count", 4},
        {"gguf_tensor_bytes", 105 * mib},
        {"gguf_global_tensor_bytes", 5 * mib},
        {"gguf_block_tensor_bytes", {{{"block", 0}, {"bytes", 10 * mib}}, {{"block", 1}, {"bytes", 20 * mib}}, {{"block", 2}, {"bytes", 30 * mib}}, {{"block", 3}, {"bytes", 40 * mib}}}},
    }, {{"availableMib", 75}});
    require_eq(result.at("source"), "tensor-directory", "tensor directory source");
    require_eq(result.at("estimatedLayers"), 2, "non-uniform reverse layers");
    require_eq(result.at("availableWeightBytes"), 70 * mib, "available bytes subtract global");

    result = caliber::estimate_initial_offload({{"size_mib", 440}, {"gguf_block_count", 4}, {"gguf_global_tensor_bytes", 40 * mib}}, {{"availableMib", 250}});
    require_eq(result.at("source"), "uniform-file-size", "uniform fallback source");
    require_eq(result.at("estimatedLayers"), 2, "uniform fallback layers");

    result = caliber::estimate_initial_offload({{"size_mib", 16000}}, {{"availableMib", 8000}});
    require_eq(result.at("source"), "unavailable", "unavailable without layer count");
}

void test_offload_planner() {
    auto probe = [](int layer, int vram, bool fit = true, json actual = nullptr) {
        return json{{"requested_layers", layer}, {"offloaded_layers", actual.is_null() ? json(layer) : actual}, {"vram_ready_mib", vram}, {"fit_under_vram_budget", fit}, {"ready", true}};
    };
    json result = caliber::estimate_offload_cliff({{"blockCount", 40}, {"vramBudgetCapMib", 7800}, {"initialEstimate", 17}, {"probes", json::array()}});
    require_eq(result.at("next_probe_layers"), 17, "cliff starts from structural estimate");
    result = caliber::estimate_offload_cliff({{"blockCount", 40}, {"vramBudgetCapMib", 7800}, {"initialEstimate", 16}, {"probes", {probe(10, 4000), probe(20, 7000)}}});
    require_eq(result.at("slope_mib_per_layer"), 300.0, "linear slope");
    require_eq(result.at("predicted_fit_layers"), 22, "linear prediction");
    result = caliber::estimate_offload_cliff({{"blockCount", 40}, {"vramBudgetCapMib", 7800}, {"initialEstimate", 20}, {"probes", {probe(21, 7600, true), probe(22, 7900, false)}}});
    require_eq(result.at("confidence"), "bracketed", "adjacent bracket confidence");
    require(result.at("complete").get<bool>(), "adjacent bracket complete");
    const auto candidates = caliber::build_offload_benchmark_candidates(20, 40);
    require(candidates == std::vector<int>({14, 17, 19, 20, 21, 23}), "offload benchmark candidates");
}

void test_moe() {
    const double gib = 1024.0 * 1024.0 * 1024.0;
    json metadata = {
        {"size_mib", 6000},
        {"gguf_block_count", 6},
        {"gguf_tensor_bytes", 6 * gib},
        {"gguf_global_tensor_bytes", 512 * 1024 * 1024},
        {"gguf_expert_tensor_bytes", 4.5 * gib},
        {"gguf_block_tensor_bytes", json::array()},
    };
    for (int block = 0; block < 6; ++block) metadata["gguf_block_tensor_bytes"].push_back({{"block", block}, {"bytes", 900 * 1024 * 1024}, {"expert_bytes", 768 * 1024 * 1024}});
    const json structural = caliber::estimate_initial_cpu_moe(metadata, 4096, 512);
    require_eq(structural.at("expertBlockCount"), 6, "moe expert count");
    require_eq(structural.at("nCpuMoe"), 4, "moe structural cpu experts");
    auto candidates = caliber::build_moe_benchmark_candidates(13, 40);
    require(candidates == std::vector<int>({10, 12, 13, 14, 15, 16, 20, 30, 37, 39, 40}), "moe benchmark candidates");
}

void test_plan_core() {
    const auto plan = caliber::invoke_plan(catalog(), cfg(), models_catalog(), presets());
    int vanilla = 0;
    int matched = 0;
    bool qwen_max = false;
    bool moe_28 = false;
    bool offload_20 = false;
    for (const auto & item : plan) {
        if (item.at("control_kind") == "vanilla") ++vanilla;
        if (item.at("control_kind") == "vanilla-matched") ++matched;
        if (item.at("label") == "Qwen3.5-4B Q4_K_M @ ctx=131072_kv=q8_0") qwen_max = true;
        if (item.at("label") == "Qwen3.6-35B-A3B Q4_K_M @ ncpumoe_28") moe_28 = true;
        if (item.at("label") == "Phi-4-reasoning-plus Q4_K_M @ ngl_20") offload_20 = true;
    }
    require(vanilla == 3, "raw vanilla controls");
    require(matched == 3, "matched vanilla controls");
    require(qwen_max, "model max context anchor kept");
    require(moe_28, "moe sweep row");
    require(offload_20, "offload sweep row");

    json workload_cfg = cfg();
    workload_cfg["bench"] = {{"n_predict", 128}};
    workload_cfg["planning"]["workload_sweeps"] = {
        {"prefill_micro_tokens", {512}},
        {"prefill_ratios", {0.25, 0.9, 0.99}},
        {"kv_fill_ratios", {0.25, 0.9, 0.99}},
        {"context_reserve_tokens", 512},
    };
    const auto profiles = caliber::workload_profiles_for_context(16384, workload_cfg, "all");
    require(profiles.size() == 5, "workload profile count");
    require_eq(profiles.front().at("prefillTokens"), 512, "workload micro prefill");
}

void test_adaptive_production_plan() {
    const double mib = 1024.0 * 1024.0;
    json adaptive_cfg = cfg();
    adaptive_cfg["planning"].erase("offload_sweep");
    adaptive_cfg["planning"].erase("moecpu_sweep");
    adaptive_cfg["planning"]["overhead_mib"] = 256;
    adaptive_cfg["planning"]["per_gpu_headroom_mib"] = 512;
    adaptive_cfg["hardware"] = {
        {"backend", "CUDA"},
        {"vram_budget_mib", 16384},
        {"gpus", {{{"name", "gpu0"}, {"vram_driver_usable_mib", 4096}}, {{"name", "gpu1"}, {"vram_driver_usable_mib", 4096}}}},
        {"system_ram_available_mib", 32000},
    };
    adaptive_cfg["capabilities"] = {{"supported_flags", {"--ctx-size", "--gpu-layers", "--cache-type-k", "--cache-type-v", "--parallel"}}};
    json meta = {
        {"path", "model.gguf"}, {"model", "Dense-12B"}, {"variant", "Q4_K_M"}, {"size_mib", 9000},
        {"gguf_block_count", 8}, {"gguf_tensor_bytes", 9 * 1024.0 * mib}, {"gguf_global_tensor_bytes", 1024.0 * mib},
        {"gguf_block_tensor_bytes", json::array()}, {"is_moe", false}, {"gguf_context_length", 32768},
    };
    for (int block = 0; block < 8; ++block) meta["gguf_block_tensor_bytes"].push_back({{"block", block}, {"bytes", 1024.0 * mib}});
    const auto plan = caliber::invoke_plan({meta}, adaptive_cfg, {}, {});
    int adaptive_rows = 0;
    for (const auto & item : plan) {
        if (item.value("planning_mode", std::string()) != "adaptive-structural") continue;
        ++adaptive_rows;
        require_eq(item.at("planner_adapter"), "cuda-topology", "topology adapter recorded");
        require(item.at("extra_args").get<std::string>().find("--ctx-size") != std::string::npos, "logical context survives capability mapping");
        require_eq(item.at("capability_source"), "build-probe", "build capability source recorded");
    }
    require(adaptive_rows > 1 && adaptive_rows <= 6, "small adaptive frontier grid");
}

void test_real_gguf_reader() {
    const char * fixture = std::getenv("LLAMA_TEST_GGUF");
    if (fixture == nullptr || fixture[0] == '\0') return;
    const std::string path = fixture;
    if (!std::filesystem::exists(path)) return;
    const json meta = caliber::read_gguf_plan_meta(path);
    require_eq(meta.at("path"), path, "real GGUF path");
    require(meta.at("size_mib").get<double>() > 0, "real GGUF size");
    require(meta.contains("gguf_tensor_count"), "real GGUF tensor count present");
    json local_cfg = cfg();
    local_cfg["hardware"]["vram_budget_mib"] = 49152;
    local_cfg["hardware"]["vram_driver_usable_mib"] = 24576;
    local_cfg["context_candidates"] = {{{"ctx", 8192}, {"kv", "q8_0"}}, {{"ctx", 131072}, {"kv", "q8_0"}}};
    const auto plan = caliber::invoke_plan({meta}, local_cfg, {}, {});
    require(!plan.empty(), "real GGUF plan non-empty");
    require(std::any_of(plan.begin(), plan.end(), [](const json & item) { return item.at("control_kind") == "vanilla"; }), "real GGUF vanilla control");
}

} // namespace

int main() {
    test_offload_estimator();
    test_offload_planner();
    test_moe();
    test_plan_core();
    test_adaptive_production_plan();
    test_real_gguf_reader();
    if (failures) {
        std::cerr << failures << " caliber plan test(s) failed\n";
        return 1;
    }
    std::cout << "caliber plan tests passed\n";
    return 0;
}
