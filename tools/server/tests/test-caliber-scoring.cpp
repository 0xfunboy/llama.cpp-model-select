#include "caliber-scoring.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

json load_json_file(const std::string & path) {
    std::ifstream in(path);
    require((bool) in, "open fixture " + path);
    json value;
    in >> value;
    return value;
}

json item(json overrides = json::object()) {
    json out = {
        {"id", "qwen3.5-9b-q4km__ctx16384_q8"},
        {"label", "Qwen3.5-9B Q4_K_M @ ctx=16384_kv=q8_0"},
        {"model", "Qwen3.5-9B"},
        {"variant", "Q4_K_M"},
        {"series", "qwen"},
        {"level", "high"},
        {"sweep", "context"},
        {"workload_kind", "kv-fill"},
        {"prefill_target_tokens", 0},
        {"kv_fill_target_tokens", 49152},
        {"reasoning_mode", "default"},
        {"template_note", nullptr},
        {"gguf_context_length", 131072},
        {"gguf_architecture", "qwen3"},
        {"planning_mode", "adaptive-offload"},
        {"calibration_id", "cal-123"},
        {"predicted_fit_layers", 28},
        {"verified_fit_layers", 27},
        {"first_spill_layers", 28},
        {"probe_count", 3},
        {"fit_offset", 1},
        {"calibration_cache_hit", true},
        {"calibration_cache_age_hours", 2.5},
        {"predicted_n_cpu_moe", 24},
        {"verified_n_cpu_moe", 25},
        {"first_spill_n_cpu_moe", 24},
        {"model_path", "C:\\models\\qwen.gguf"},
        {"mmproj_path", nullptr},
        {"extra_args", "--ctx-size 16384 --gpu-layers 99 --cache-type-k q8_0"},
    };
    for (auto it = overrides.begin(); it != overrides.end(); ++it) out[it.key()] = it.value();
    return out;
}

json cfg() {
    return {
        {"llama_server_exe", "C:\\llama\\llama-server.exe"},
        {"hardware", {{"vram_total_mib", 8192}}},
        {"wddm_detection", {
            {"vram_saturation_threshold", 0.92},
            {"shared_delta_confirm_mib", 500},
        }},
    };
}

json run(int i, int vram_peak, int shared_peak, int prompt_tps, int eval_tps) {
    const int power[] = {130, 180, 150};
    const int temp[] = {60, 72, 65};
    const int util[] = {60, 75, 90};
    const int ram[] = {500, 900, 700};
    const int ws[] = {2048, 3072, 2560};
    const int priv[] = {1536, 2048, 1792};
    const int virt[] = {8192, 12288, 10240};
    const int disk_bytes[] = {10000000, 20000000, 15000000};
    const int proc_disk[] = {12, 48, 24};
    const int disk[] = {200, 500, 300};
    return {
        {"run_index", i},
        {"timestamp", "2026-05-16T10:00:0" + std::to_string(i)},
        {"vram_before_mib", 1200},
        {"vram_peak_mib", vram_peak},
        {"vram_baseline_mib", 1200},
        {"vram_baseline_pct", 0.1465},
        {"vram_total_peak_mib", vram_peak},
        {"vram_process_peak_mib", vram_peak - 1200},
        {"vram_external_peak_mib", 1200},
        {"shared_peak_mib", shared_peak},
        {"load_sec", 6.5},
        {"ready", true},
        {"ok", true},
        {"error", nullptr},
        {"prompt_n", 80},
        {"prompt_tps", prompt_tps},
        {"eval_n", 128},
        {"eval_tps", eval_tps},
        {"cpu_model_mib", 0},
        {"cuda_model_mib", 5200},
        {"kv_cache_mib", 1024},
        {"compute_cuda_mib", 360},
        {"compute_host_mib", 80},
        {"layers_offloaded", "33/33"},
        {"effective_context_size", 4096},
        {"effective_parallel_slots", 4},
        {"effective_n_parallel", 4},
        {"flash_attention_state", "auto-disabled"},
        {"fit_status", "success"},
        {"ttft_sec", 0.2 + i / 10.0},
        {"prompt_ms", 200 + i * 100},
        {"ttfr_ms", 100 + i * 20},
        {"e2e_ttft_ms", 180 + i * 60},
        {"total_request_ms", 3000 + i * 200},
        {"latency_total_request_ms", 360 + i * 60},
        {"gpu_power_peak_w", power[i]},
        {"gpu_temp_peak_c", temp[i]},
        {"gpu_util_avg_pct", util[i]},
        {"ram_baseline_mib", 12000},
        {"ram_used_peak_mib", ram[i]},
        {"process_working_set_peak_mib", ws[i]},
        {"process_private_bytes_peak_mib", priv[i]},
        {"process_virtual_bytes_peak_mib", virt[i]},
        {"process_disk_read_bytes_delta", disk_bytes[i]},
        {"process_disk_read_peak_mb_s", proc_disk[i]},
        {"disk_read_peak_mb_s", disk[i]},
    };
}

void test_winner_policy() {
    const double standard = caliber::kv_quality_value({{"extra_args", "--cache-type-k q8_0 --cache-type-v q8_0"}});
    const double compromise = caliber::kv_quality_value({{"extra_args", "--cache-type-k q8_0 --cache-type-v q5_1"}});
    const double rescue = caliber::kv_quality_value({{"extra_args", "--cache-type-k q4_0 --cache-type-v q4_0"}});
    require(standard > compromise && compromise > rescue, "KV quality ordering");

    auto winners = caliber::group_winners({
        {{"model", "m"}, {"id", "vanilla"}, {"ok", true}, {"eval_tps", 100}, {"control_kind", "vanilla"}},
        {{"model", "m"}, {"id", "tuned"}, {"ok", true}, {"eval_tps", 80}},
    }, caliber::winner_profile::speed);
    require_eq(winners["m"]["id"], "tuned", "vanilla controls excluded");

    const json cases = load_json_file(CALIBER_WINNER_FIXTURE_PATH);
    for (const auto & c : cases) {
        std::vector<json> candidates;
        for (auto candidate : c.at("candidates")) {
            candidate["model"] = "fixture";
            candidate["ok"] = true;
            candidates.push_back(candidate);
        }
        caliber::winner_policy_options options;
        options.confirm_mib = 500;
        const auto picked = caliber::group_winners(candidates, caliber::winner_profile_from_string(c.at("profile").get<std::string>()), options);
        require_eq(picked.at("fixture").at("id"), c.at("expected"), "winner fixture: " + c.at("name").get<std::string>());
    }

    require(caliber::is_safe({{"shared_peak_mib", 500}}, 500), "is_safe inclusive threshold");
    require(!caliber::is_safe({{"shared_peak_mib", 501}}, 500), "is_safe rejects above threshold");
    require(caliber::is_safe({{"sweep", "moe-cpu"}, {"shared_peak_mib", 10000}, {"fit_status", "failed_but_running"}, {"fit_status_source", "inferred"}}, 500), "inferred MoE shared is safe");
    require(!caliber::is_safe({{"sweep", "moe-cpu"}, {"shared_peak_mib", 10000}, {"fit_status", "failed_but_running"}, {"fit_status_source", "llama.cpp"}}, 500), "llama.cpp MoE fit failure is unsafe");
    require(std::isinf(caliber::winner_score({{"eval_tps", 100}, {"gpu_power_peak_w", 0}}, caliber::winner_profile::efficiency)), "efficiency without power is -inf");

    winners = caliber::group_winners({
        {{"id", "baseline"}, {"model", "m"}, {"ok", true}, {"eval_tps", 50}, {"workload_kind", "baseline"}},
        {{"id", "prefill"}, {"model", "m"}, {"ok", true}, {"eval_tps", 500}, {"workload_kind", "prefill"}},
        {{"id", "kv"}, {"model", "m"}, {"ok", true}, {"eval_tps", 600}, {"workload_kind", "kv-fill"}},
    }, caliber::winner_profile::speed);
    require_eq(winners["m"]["id"], "baseline", "diagnostic workloads excluded");

    winners = caliber::group_winners({
        {{"id", "limited-fast"}, {"model", "m"}, {"ok", true}, {"eval_tps", 100}, {"measurement_confidence", "limited"}},
        {{"id", "reliable-slower"}, {"model", "m"}, {"ok", true}, {"eval_tps", 50}, {"measurement_confidence", "reliable"}},
    }, caliber::winner_profile::speed);
    require_eq(winners["m"]["id"], "reliable-slower", "limited confidence is fallback only");

    winners = caliber::group_winners({
        {{"id", "crawl-fast"}, {"model", "m"}, {"ok", true}, {"eval_tps", 100}, {"decode_usability", "crawl"}},
        {{"id", "chat-slower"}, {"model", "m"}, {"ok", true}, {"eval_tps", 20}, {"decode_usability", "chat"}},
    }, caliber::winner_profile::speed);
    require_eq(winners["m"]["id"], "chat-slower", "crawl band excluded");

    const json synthetic = {
        {"id", "synthetic"},
        {"model", "m"},
        {"ok", true},
        {"eval_tps", 80},
        {"benchmark_backend", "llama-bench"},
        {"memory_measurement_kind", "unavailable"},
        {"measurement_confidence", "provisional"},
        {"gpu_power_peak_w", 0},
    };
    require(!caliber::is_fit_eligible(synthetic), "synthetic benchmark is not FIT eligible");
    require(caliber::group_winners({synthetic}, caliber::winner_profile::speed).count("m") == 1,
            "synthetic throughput remains a provisional speed fallback");
    require(caliber::group_winners({synthetic}, caliber::winner_profile::efficiency).empty(),
            "efficiency requires measured power");
    require(caliber::group_winners({synthetic}, caliber::winner_profile::safety).empty(),
            "safety requires observed memory");
    require(caliber::group_winners({synthetic}, caliber::winner_profile::overall).empty(),
            "overall requires observed memory");
}

void test_memory_policy() {
    std::vector<json> base = {
        {{"id", "a"}, {"model", "Q"}, {"variant", "9B"}, {"sweep", "context"}, {"workload_kind", "baseline"}, {"ok", true}, {"extra_args", "--ctx-size 32768"}, {"vram_peak_mib", 6200}, {"shared_peak_mib", 100}},
        {{"id", "b"}, {"model", "Q"}, {"variant", "9B"}, {"sweep", "context"}, {"workload_kind", "baseline"}, {"ok", true}, {"extra_args", "--ctx-size 65536"}, {"vram_peak_mib", 6900}, {"shared_peak_mib", 150}},
        {{"id", "c"}, {"model", "Q"}, {"variant", "9B"}, {"sweep", "context"}, {"workload_kind", "baseline"}, {"ok", true}, {"extra_args", "--ctx-size 131072"}, {"vram_peak_mib", 7680}, {"shared_peak_mib", 850}},
    };
    auto policies = caliber::derive_memory_policies(base, 8192);
    require_eq(policies.at("c").at("memory_state"), "spill_risk", "shared growth risk");
    require_eq(policies.at("c").at("residency"), "spill-risk", "shared growth residency");
    require_eq(policies.at("b").at("context_growth_confidence"), "measured", "context growth measured");
    require(policies.at("b").at("context_growth_mib_per_1k").get<double>() > 23.0, "context growth lower bound");
    require(policies.at("b").at("context_growth_mib_per_1k").get<double>() < 24.0, "context growth upper bound");

    policies = caliber::derive_memory_policies({{{"id", "m"}, {"model", "M"}, {"variant", "A3B"}, {"sweep", "moe-cpu"}, {"workload_kind", "baseline"}, {"ok", true}, {"vram_peak_mib", 7000}, {"shared_peak_mib", 2000}}}, 8192);
    require_eq(policies.at("m").at("memory_state"), "moe_shared_ambiguous", "MoE shared ambiguous");
    require_eq(policies.at("m").at("residency"), "mixed-pressure", "MoE mixed pressure");

    caliber::memory_policy_options opts;
    opts.vram_budget_mib = 7782;
    policies = caliber::derive_memory_policies({
        {{"id", "s"}, {"model", "D"}, {"variant", "4B"}, {"sweep", "context"}, {"workload_kind", "baseline"}, {"ok", true}, {"vram_peak_mib", 6000}, {"vram_baseline_mib", 1000}, {"shared_peak_mib", 0}},
        {{"id", "p"}, {"model", "E"}, {"variant", "4B"}, {"sweep", "context"}, {"workload_kind", "baseline"}, {"ok", true}, {"vram_peak_mib", 7100}, {"vram_baseline_mib", 1000}, {"shared_peak_mib", 0}},
    }, 8192, opts);
    require_eq(policies.at("s").at("residency"), "vram-only", "canonical budget safe row");
    require_eq(policies.at("p").at("residency"), "vram-pressure", "canonical budget pressure row");
    require_eq(policies.at("p").at("resource_fit"), "max-usage", "canonical budget pressure fit");
    require_eq(policies.at("p").at("vram_available_for_run_mib"), 6782, "budget minus baseline");
}

void test_result_core() {
    require(caliber::median({42}) == 42, "single median");
    require(caliber::median({12, 7, 10}) == 10, "odd median");
    require(caliber::median({3, 1, 4, 2}) == 2, "lower-middle even median");
    require(std::isnan(caliber::median({std::nan("")})), "empty finite median is NaN");

    const std::vector<json> runs = {
        run(0, 7000, 30, 410, 46),
        run(1, 7200, 50, 430, 64),
        run(2, 7100, 40, 420, 55),
    };
    const json result = caliber::aggregate_bench_result(item(), cfg(), runs, {
        {"bench_session_id", "s1"},
        {"bench_session_started_at", "2026-05-16T10:00:00"},
        {"llama_server_version", "b9999"},
    });
    require_eq(result.at("vram_peak_mib"), 7100, "aggregate vram_peak_mib");
    require_eq(result.at("eval_tps"), 55.0, "aggregate eval_tps");
    require_eq(result.at("run_count"), 3, "aggregate run_count");
    require_eq(result.at("first_eval_tps"), 46.0, "aggregate first_eval_tps");
    require_eq(result.at("repeat_eval_tps"), 55.0, "aggregate repeat_eval_tps");
    require_eq(result.at("eval_spread_pct"), 32.7, "aggregate eval spread");
    require_eq(result.at("row_role"), "diagnostic", "aggregate row role");
    require_eq(result.at("requested_context_size"), 16384, "aggregate requested ctx");
    require_eq(result.at("requested_cache_type_k"), "q8_0", "aggregate requested cache K");
    require(result.at("requested_cache_type_v").is_null(), "aggregate requested cache V null");
    require_eq(result.at("gpu_power_peak_w"), 180.0, "aggregate power peak");
    require_eq(result.at("disk_read_peak_mb_s"), 500.0, "aggregate disk peak");
    require_eq(result.at("bench_session_id"), "s1", "aggregate session id");

    const json parsed = caliber::parse_llama_server_stderr(
        "CPU model buffer size = 100.50 MiB\n"
        "CUDA0 model buffer size = 5200.25 MiB\n"
        "CUDA0 KV buffer size = 1024.00 MiB\n"
        "CUDA0 compute buffer size = 360.75 MiB\n"
        "CUDA_Host compute buffer size = 80.00 MiB\n"
        "offloaded 33/33 layers to GPU\n"
        "srv  llama_server: n_parallel is set to auto, using n_parallel = 4 and kv_unified = true\n"
        "srv    load_model: initializing slots, n_slots = 4\n"
        "slot   load_model: id  0 | task -1 | new slot, n_ctx = 4096\n"
        "sched_reserve: Flash Attention was auto, set to disabled\n"
        "successfully fit params\n");
    require_eq(parsed.at("cuda_model_mib"), 5200.25, "parse cuda model");
    require_eq(parsed.at("layers_offloaded"), "33/33", "parse offload layers");
    require_eq(parsed.at("effective_context_size"), 4096, "parse effective ctx");
    require_eq(parsed.at("effective_n_parallel"), 4, "parse n_parallel");
    require_eq(parsed.at("fit_status"), "success", "parse fit status");

    const json derived = caliber::derive_result_fields({{"prompt_n", 80}, {"prompt_tps", 100}, {"eval_n", 128}, {"eval_tps", 50}, {"vram_peak_mib", 2000}, {"extra_args", "--ctx-size 16384 --gpu-layers 99"}}, 8192);
    require_eq(derived.at("time_total_sec"), 3.36, "derive time total");
    require_eq(derived.at("headroom_mib"), 6192, "derive headroom");
    require_eq(derived.at("ctx_size"), 16384, "derive ctx");

    const json measured_context = caliber::derive_result_fields({
        {"ctx_size", 640},
        {"requested_context_size", 131072},
        {"extra_args", "--ctx-size 131072"},
    }, 8192);
    require_eq(measured_context.at("ctx_size"), 640, "measured context wins over requested CLI context");

    json synthetic_item = item({
        {"sweep", "context"},
        {"workload_kind", "baseline"},
        {"extra_args", "--ctx-size 131072"},
    });
    json synthetic_run = {
        {"run_index", 0},
        {"ok", true},
        {"eval_tps", 50.0},
        {"prompt_tps", 500.0},
        {"prompt_n", 512},
        {"eval_n", 128},
        {"ctx_size", 640},
        {"measured_context_size", 640},
        {"benchmark_allocated_context_size", 640},
        {"benchmark_backend", "llama-bench"},
        {"memory_measurement_kind", "unavailable"},
    };
    const json synthetic_result = caliber::aggregate_bench_result(synthetic_item, cfg(), {synthetic_run});
    require_eq(synthetic_result.at("ctx_size"), 640, "synthetic aggregate reports allocated context");
    require_eq(synthetic_result.at("requested_context_size"), 131072, "synthetic aggregate preserves requested context");
    require_eq(synthetic_result.at("context_target_met"), false, "synthetic aggregate exposes unmet context target");
    require_eq(synthetic_result.at("measurement_confidence"), "provisional", "single sample is provisional");
    require_eq(synthetic_result.at("memory_state"), "unavailable", "missing telemetry is explicit");
    require_eq(synthetic_result.at("resource_fit"), "unknown", "resource fit is unknown without telemetry");
    require_eq(synthetic_result.at("fit_eligible"), false, "unobserved synthetic result cannot configure FIT");
}

} // namespace

int main() {
    test_winner_policy();
    test_memory_policy();
    test_result_core();
    if (failures != 0) {
        std::cerr << failures << " caliber scoring test(s) failed\n";
        return 1;
    }
    std::cout << "caliber scoring tests passed\n";
    return 0;
}
