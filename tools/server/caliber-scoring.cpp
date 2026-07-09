#include "caliber-scoring.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace caliber {

namespace {

bool is_finite(double v) {
    return std::isfinite(v);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool has_key(const json & obj, const char * key) {
    return obj.is_object() && obj.contains(key) && !obj.at(key).is_null();
}

std::string str_value(const json & obj, const char * key, const std::string & fallback = "") {
    if (!has_key(obj, key)) return fallback;
    const auto & value = obj.at(key);
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number()) return value.dump();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    return fallback;
}

double num_value(const json & obj, const char * key, double fallback = 0.0) {
    if (!has_key(obj, key)) return fallback;
    return finite_number(obj.at(key), fallback);
}

bool bool_value(const json & obj, const char * key, bool fallback = false) {
    if (!has_key(obj, key)) return fallback;
    const auto & value = obj.at(key);
    if (value.is_boolean()) return value.get<bool>();
    return fallback;
}

double nullable_num(const json & obj, const char * key) {
    if (!has_key(obj, key)) return std::numeric_limits<double>::quiet_NaN();
    const auto v = finite_number(obj.at(key), std::numeric_limits<double>::quiet_NaN());
    return v;
}

json number_or_null(double value, int digits = -1) {
    if (!std::isfinite(value)) return nullptr;
    if (digits >= 0) {
        const double factor = std::pow(10.0, digits);
        value = std::round(value * factor) / factor;
    }
    return value;
}

double round_to(double value, int digits) {
    const double factor = std::pow(10.0, digits);
    return std::round(value * factor) / factor;
}

int64_t int_value(const json & obj, const char * key, int64_t fallback = 0) {
    const double v = num_value(obj, key, (double) fallback);
    return std::isfinite(v) ? (int64_t) std::trunc(v) : fallback;
}

std::string arg_value(const std::string & args, const std::vector<std::string> & names) {
    for (const auto & name : names) {
        std::string escaped;
        for (char c : name) {
            if (std::string(".^$|()[]*+?{}\\").find(c) != std::string::npos) escaped.push_back('\\');
            escaped.push_back(c);
        }
        std::regex re("(?:^|\\s)" + escaped + "\\s+([^\\s]+)");
        std::smatch m;
        if (std::regex_search(args, m, re)) return m[1].str();
    }
    return "";
}

int64_t arg_int(const std::string & args, const std::vector<std::string> & names, int64_t fallback = 0) {
    const std::string value = arg_value(args, names);
    if (value.empty()) return fallback;
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

bool regex_has(const std::string & s, const std::string & pattern, std::regex::flag_type flags = std::regex::ECMAScript) {
    return std::regex_search(s, std::regex(pattern, flags));
}

std::string key_for_group(const json & row) {
    return str_value(row, "model") + "|" + str_value(row, "variant");
}

double run_vram_mib(const json & row) {
    const double process_peak = nullable_num(row, "vram_process_peak_mib");
    if (std::isfinite(process_peak)) return process_peak;
    const double total_peak = nullable_num(row, "vram_total_peak_mib");
    const double baseline = num_value(row, "vram_baseline_mib", 0.0);
    if (std::isfinite(total_peak)) return std::max(0.0, total_peak - baseline);
    return nullable_num(row, "vram_peak_mib");
}

double total_vram_peak_mib(const json & row, double baseline_mib) {
    const double total_peak = std::isfinite(nullable_num(row, "vram_total_peak_mib"))
        ? nullable_num(row, "vram_total_peak_mib")
        : nullable_num(row, "vram_peak_mib");
    if (std::isfinite(total_peak)) return total_peak;
    const double process_peak = nullable_num(row, "vram_process_peak_mib");
    return std::isfinite(process_peak) ? process_peak + baseline_mib : std::numeric_limits<double>::quiet_NaN();
}

double context_tokens(const json & row) {
    if (std::isfinite(nullable_num(row, "ctx_size"))) return nullable_num(row, "ctx_size");
    if (std::isfinite(nullable_num(row, "requested_context_size"))) return nullable_num(row, "requested_context_size");
    const auto from_args = context_size_from_args(str_value(row, "extra_args"));
    return from_args > 0 ? (double) from_args : std::numeric_limits<double>::quiet_NaN();
}

double memory_pressure_mib(const json & row) {
    const double run = run_vram_mib(row);
    if (!std::isfinite(run)) return std::numeric_limits<double>::quiet_NaN();
    return run + num_value(row, "shared_peak_mib", 0.0);
}

bool observed_partial_offload(const json & row) {
    const std::string layers = str_value(row, "layers_offloaded");
    std::smatch m;
    if (!std::regex_match(layers, m, std::regex(R"((\d+)\/(\d+))"))) return false;
    const int offloaded = std::stoi(m[1].str());
    const int total = std::stoi(m[2].str());
    return total > 0 && offloaded < total;
}

bool is_mixed_expected(const json & row) {
    const std::string sweep = str_value(row, "sweep");
    const std::string planning = str_value(row, "planning_mode");
    const std::string args = str_value(row, "extra_args");
    return sweep == "moe-cpu" ||
           planning == "adaptive-moe" ||
           regex_has(args, R"((?:^|\s)--n-cpu-moe(?:\s|$))") ||
           observed_partial_offload(row);
}

bool is_memory_failure(const json & row) {
    if (bool_value(row, "ok", false)) return false;
    const std::string reason = to_lower(str_value(row, "failure_reason") + " " + str_value(row, "error"));
    return reason.find("oom") != std::string::npos ||
           reason.find("out of memory") != std::string::npos ||
           reason.find("vram_overflow") != std::string::npos ||
           reason.find("memory") != std::string::npos;
}

struct headroom_thresholds_t {
    double light;
    double comfortable;
    double loaded;
};

headroom_thresholds_t headroom_thresholds(double vram_budget_mib) {
    const double budget = std::max(0.0, std::trunc(vram_budget_mib));
    if (budget <= 4096) return {2048, 1024, 512};
    if (budget <= 8192) return {4096, 2048, 1024};
    if (budget <= 16384) return {6144, 4096, 2048};
    if (budget <= 24576) return {12288, 8192, 4096};
    return {24576, 16384, 8192};
}

int fit_rank(const std::string & fit) {
    if (fit == "light-fit") return 1;
    if (fit == "comfortable") return 2;
    if (fit == "loaded") return 3;
    if (fit == "max-usage") return 4;
    if (fit == "stretch") return 5;
    return 0;
}

std::string max_fit(std::string a, const std::string & b) {
    return fit_rank(b) > fit_rank(a) ? b : a;
}

std::string shared_class(double shared, double threshold, double minor_upper, double pressure_upper) {
    if (shared < threshold) return "none";
    if (shared < minor_upper) return "minor";
    if (shared < pressure_upper) return "pressure";
    return "high";
}

std::string resource_fit_from_headroom(double headroom_mib, double vram_budget_mib, const std::string & residency, double shared, double threshold, double minor_upper, double pressure_upper) {
    if (residency == "memory-failed") return "unknown";
    std::string fit = "unknown";
    if (std::isfinite(headroom_mib)) {
        const auto thresholds = headroom_thresholds(vram_budget_mib);
        if (headroom_mib < 0) fit = "stretch";
        else if (headroom_mib < thresholds.loaded) fit = "max-usage";
        else if (headroom_mib < thresholds.comfortable) fit = "loaded";
        else if (headroom_mib < thresholds.light) fit = "comfortable";
        else fit = "light-fit";
    }
    const std::string s = shared_class(shared, threshold, minor_upper, pressure_upper);
    if (s == "minor") fit = max_fit(fit, "loaded");
    else if (s == "pressure" || s == "high") fit = max_fit(fit, "stretch");
    if (residency == "spill-confirmed" || residency == "spill-risk" || residency == "mixed-pressure") fit = max_fit(fit, "stretch");
    return fit;
}

std::string usability_from_eval(const json & row) {
    if (!bool_value(row, "ok", false)) return "unusable";
    const double eval_tps = num_value(row, "eval_tps", 0.0);
    if (eval_tps >= 100) return "ultra-fast";
    if (eval_tps >= 40) return "coding-agent";
    if (eval_tps >= 15) return "fast-chat";
    if (eval_tps >= 7) return "chat";
    if (eval_tps >= 3) return "slow";
    if (eval_tps > 0) return "crawl";
    return "unusable";
}

double response_latency_ms(const json & row) {
    for (const char * key : {"e2e_ttft_ms", "client_ttft_ms", "ttfr_ms", "server_prefill_ms", "prompt_ms"}) {
        const double v = nullable_num(row, key);
        if (std::isfinite(v)) return v;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::pair<std::string, double> usability_from_response_latency(const json & row) {
    if (!bool_value(row, "ok", false)) return {"blocked", std::numeric_limits<double>::quiet_NaN()};
    const double latency = response_latency_ms(row);
    if (!std::isfinite(latency)) return {"unknown", latency};
    if (latency <= 1000) return {"instant", latency};
    if (latency <= 3000) return {"responsive", latency};
    if (latency <= 10000) return {"waiting", latency};
    if (latency <= 30000) return {"background", latency};
    return {"blocked", latency};
}

std::string degradation_band(double degradation_pct) {
    if (!std::isfinite(degradation_pct)) return "unknown";
    if (degradation_pct < 15) return "none";
    if (degradation_pct < 30) return "mild";
    if (degradation_pct < 50) return "severe";
    return "critical";
}

double eval_spread_pct(const json & row) {
    const double direct = nullable_num(row, "eval_spread_pct");
    if (std::isfinite(direct)) return direct;
    std::vector<double> samples;
    if (has_key(row, "runs") && row.at("runs").is_array()) {
        for (const auto & run : row.at("runs")) {
            const double v = nullable_num(run, "eval_tps");
            if (std::isfinite(v)) samples.push_back(v);
        }
    }
    const double med = nullable_num(row, "eval_tps");
    if (samples.size() < 2 || !std::isfinite(med) || med <= 0) return 0.0;
    auto [mn, mx] = std::minmax_element(samples.begin(), samples.end());
    return ((*mx - *mn) / med) * 100.0;
}

bool is_limited_row(const json & row, const std::string & residency) {
    const std::string label = str_value(row, "label");
    const std::string args = str_value(row, "extra_args");
    return residency == "spill-risk" ||
           residency == "shared-allocated" ||
           str_value(row, "conditional_kind") == "kv_rescue" ||
           regex_has(label, "rescue", std::regex::icase) ||
           regex_has(args, R"((?:^|\s)--cache-type-[kv]\s+q4_0(?:\s|$))");
}

std::string confidence_for_row(const json & row, const std::string & residency) {
    if (!bool_value(row, "ok", false)) return is_memory_failure(row) ? "failed" : "partial";
    if (eval_spread_pct(row) > 15) return "noisy";
    if (is_limited_row(row, residency)) return "limited";
    return "reliable";
}

std::string cap_confidence_for_anomalous_degradation(std::string confidence, const std::string & resource_fit, const std::string & degradation) {
    if ((degradation == "severe" || degradation == "critical") &&
        (resource_fit == "comfortable" || resource_fit == "loaded") &&
        confidence == "reliable") {
        return "noisy";
    }
    return confidence;
}

std::string recommendation_for(const json & row, const std::string & resource_fit, const std::string & usability, const std::string & degradation, std::string & reason) {
    if (!bool_value(row, "ok", false)) {
        reason = "The benchmark did not produce a successful measured result.";
        return "failed";
    }
    if (usability == "unusable") {
        reason = "The measured decode speed is not practical for the configured use.";
        return "unusable";
    }
    if (usability == "crawl") {
        reason = "The model responds, but throughput is in the crawl band.";
        return "experimental";
    }
    if (degradation == "critical") {
        reason = "Throughput degraded critically relative to the local plateau.";
        return "stretch-compromise";
    }
    if (degradation == "severe") {
        reason = "The run shows severe throughput degradation relative to the local plateau.";
        return "stretch-compromise";
    }
    if (resource_fit == "stretch") {
        reason = "The run works, but sits at the practical resource edge.";
        return "edge-candidate";
    }
    if (resource_fit == "max-usage") {
        reason = "The run uses most of the machine while remaining measurable.";
        return "max-usage-candidate";
    }
    if (resource_fit == "loaded") {
        reason = "The run meaningfully uses the hardware while keeping a sane margin.";
        return "right-sized-candidate";
    }
    if (resource_fit == "comfortable") {
        reason = "The run is usable with healthy headroom.";
        return "balanced-efficient-candidate";
    }
    reason = "The run is usable and light on resources.";
    return "efficient-candidate";
}

std::string comparable_context_slope_key(const json & row) {
    std::ostringstream out;
    out << key_for_group(row) << "|"
        << str_value(row, "requested_cache_type_k") << "|"
        << str_value(row, "requested_cache_type_v") << "|"
        << str_value(row, "requested_parallel") << "|"
        << str_value(row, "requested_gpu_layers") << "|"
        << str_value(row, "requested_n_cpu_moe") << "|"
        << str_value(row, "sweep") << "|"
        << str_value(row, "planning_mode");
    return out.str();
}

struct context_growth_t {
    double slope_mib_per_1k = std::numeric_limits<double>::quiet_NaN();
    std::string source;
};

context_growth_t measured_context_growth(const std::vector<json> & group, double shared_threshold_mib) {
    std::map<std::string, std::vector<std::pair<double, double>>> by_profile;
    for (const auto & row : group) {
        if (!bool_value(row, "ok", false)) continue;
        if (str_value(row, "workload_kind", "baseline") != "baseline") continue;
        if (str_value(row, "sweep") != "context") continue;
        if (num_value(row, "prefill_target_tokens", 0.0) > 0 || num_value(row, "kv_fill_target_tokens", 0.0) > 0) continue;
        const double tokens = context_tokens(row);
        const double memory = memory_pressure_mib(row);
        if (!std::isfinite(tokens) || !std::isfinite(memory)) continue;
        by_profile[comparable_context_slope_key(row)].push_back({tokens, memory});
    }
    context_growth_t best;
    int best_points = 0;
    for (auto & [profile, raw_points] : by_profile) {
        std::sort(raw_points.begin(), raw_points.end());
        std::vector<std::pair<double, double>> points;
        for (const auto & point : raw_points) {
            if (points.empty() || points.back().first != point.first) points.push_back(point);
        }
        if (points.size() < 3) continue;
        std::vector<double> slopes;
        for (size_t i = 1; i < points.size(); ++i) {
            const double delta_tokens = points[i].first - points[i - 1].first;
            const double delta_memory = points[i].second - points[i - 1].second;
            if (delta_tokens <= 0 || delta_memory <= 0) continue;
            slopes.push_back(delta_memory / (delta_tokens / 1024.0));
        }
        if (slopes.size() < 2) continue;
        double sum = 0.0;
        for (double s : slopes) sum += s;
        const double avg = sum / slopes.size();
        if (!std::isfinite(avg) || avg <= 0) continue;
        auto [mn, mx] = std::minmax_element(slopes.begin(), slopes.end());
        const double spread = avg > 0 ? (*mx - *mn) / avg : 1.0;
        if (spread > 0.10) continue;
        if ((int) points.size() > best_points) {
            best_points = (int) points.size();
            best.slope_mib_per_1k = avg;
            best.source = "measured context baseline profile (" + std::to_string(points.size()) +
                          " points, shared noise floor " + std::to_string((int) shared_threshold_mib) + " MiB)";
        }
    }
    return best;
}

double infer_capability_params_b(const json & result) {
    const std::string haystack = str_value(result, "model") + " " +
                                str_value(result, "series") + " " +
                                str_value(result, "variant") + " " +
                                str_value(result, "label") + " " +
                                str_value(result, "model_path");
    std::regex re(R"((\d+(?:\.\d+)?)\s*B\b)", std::regex::icase);
    std::sregex_iterator it(haystack.begin(), haystack.end(), re);
    std::sregex_iterator end;
    double best = std::numeric_limits<double>::quiet_NaN();
    for (; it != end; ++it) {
        const double value = std::stod((*it)[1].str());
        if (value > 0 && (!std::isfinite(best) || value > best)) best = value;
    }
    return best;
}

std::string capability_class(double params_b) {
    if (!std::isfinite(params_b)) return "unknown";
    if (params_b < 3) return "micro";
    if (params_b < 7) return "small";
    if (params_b < 14) return "medium";
    if (params_b < 35) return "large";
    if (params_b < 70) return "xlarge";
    return "frontier";
}

std::vector<double> nums_from_runs(const json & runs, const char * key) {
    std::vector<double> values;
    if (!runs.is_array()) return values;
    for (const auto & run : runs) {
        const double v = nullable_num(run, key);
        if (std::isfinite(v)) values.push_back(v);
    }
    return values;
}

double max_from_runs(const std::vector<json> & runs, const char * key) {
    double out = 0.0;
    for (const auto & run : runs) {
        const double v = nullable_num(run, key);
        if (std::isfinite(v)) out = std::max(out, v);
    }
    return out;
}

double metric_median(const std::vector<json> & runs, const char * key) {
    std::vector<double> values;
    for (const auto & run : runs) {
        const double v = nullable_num(run, key);
        if (std::isfinite(v)) values.push_back(v);
    }
    return values.empty() ? std::numeric_limits<double>::quiet_NaN() : median(values);
}

} // namespace

double finite_number(const json & value, double fallback) {
    if (value.is_number()) {
        const double out = value.get<double>();
        return std::isfinite(out) ? out : fallback;
    }
    return fallback;
}

int64_t ctx_value(const json & result) {
    const double direct = nullable_num(result, "ctx_size");
    if (std::isfinite(direct)) return (int64_t) direct;
    return context_size_from_args(str_value(result, "extra_args"));
}

double kv_quality_value(const json & result) {
    const std::string args = str_value(result, "extra_args");
    auto quality = [](std::string kv) {
        kv = to_lower(kv);
        std::smatch m;
        if (std::regex_match(kv, m, std::regex(R"(q(\d+)(?:_(\d+))?)"))) {
            return std::stod(m[1].str()) * 10.0 + (m[2].matched ? std::stod(m[2].str()) : 0.0);
        }
        if (kv == "f16" || kv == "bf16") return 160.0;
        if (kv == "f32") return 320.0;
        return 0.0;
    };
    const double k = quality(arg_value(args, {"--cache-type-k"}));
    const double v = quality(arg_value(args, {"--cache-type-v"}));
    if (k > 0 && v > 0) return 0.6 * k + 0.4 * v;
    return std::max(k, v);
}

bool is_safe(const json & result, int confirm_mib) {
    if (str_value(result, "sweep") == "moe-cpu" && str_value(result, "fit_status_source") != "llama.cpp") return true;
    if (str_value(result, "fit_status") == "failed_but_running") return false;
    return num_value(result, "shared_peak_mib", 0.0) <= confirm_mib;
}

std::string normalize_row_role(const json & value) {
    if (!value.is_string()) return "";
    const std::string role = trim(value.get<std::string>());
    if (role == "candidate" || role == "raw_control" || role == "fair_control" || role == "diagnostic") return role;
    return "";
}

std::string infer_row_role(const json & source) {
    const std::string explicit_role = has_key(source, "row_role") ? normalize_row_role(source.at("row_role")) : "";
    const std::string fair_target = trim(str_value(source, "fair_control_target_id"));
    if (explicit_role == "fair_control") return fair_target.empty() ? "diagnostic" : "fair_control";
    if (!explicit_role.empty()) return explicit_role;
    const std::string control_kind = str_value(source, "control_kind");
    if (control_kind == "vanilla") return "raw_control";
    if (control_kind == "vanilla-matched") return fair_target.empty() ? "diagnostic" : "fair_control";
    if (control_kind == "vanilla-adjacent" || !control_kind.empty()) return "diagnostic";
    const std::string workload = str_value(source, "workload_kind", "baseline");
    if (!workload.empty() && workload != "baseline") return "diagnostic";
    return "candidate";
}

std::string row_role(const json & result) {
    return infer_row_role(result);
}

bool is_candidate_row(const json & source) {
    return infer_row_role(source) == "candidate";
}

bool is_winner_eligible(const json & result) {
    return row_role(result) == "candidate";
}

std::string measurement_confidence(const json & result) {
    return to_lower(trim(str_value(result, "measurement_confidence")));
}

std::string decode_usability(const json & result) {
    return to_lower(trim(str_value(result, "decode_usability")));
}

bool is_usability_winner_eligible(const json & result) {
    const std::string usability = decode_usability(result);
    return usability.empty() ||
           usability == "slow" ||
           usability == "chat" ||
           usability == "fast-chat" ||
           usability == "coding-agent" ||
           usability == "ultra-fast";
}

bool is_normal_winner_eligible(const json & result) {
    if (!is_winner_eligible(result)) return false;
    if (!is_usability_winner_eligible(result)) return false;
    const std::string confidence = measurement_confidence(result);
    return confidence.empty() || confidence == "reliable" || confidence == "noisy";
}

bool is_limited_winner_fallback_eligible(const json & result) {
    return is_winner_eligible(result) && is_usability_winner_eligible(result) && measurement_confidence(result) == "limited";
}

winner_policy_anchors compute_anchors(const std::vector<json> & results) {
    winner_policy_anchors anchors;
    std::vector<double> evals;
    std::vector<double> effs;
    for (const auto & r : results) {
        if (!bool_value(r, "ok", false) || !is_normal_winner_eligible(r)) continue;
        evals.push_back(num_value(r, "eval_tps", 0.0));
        const double power = num_value(r, "gpu_power_peak_w", 0.0);
        if (power > 0) effs.push_back(num_value(r, "eval_tps", 0.0) / power);
    }
    if (!evals.empty()) anchors.eval_max = std::max(1.0, *std::max_element(evals.begin(), evals.end()));
    if (!effs.empty()) anchors.eff_max = std::max(1.0, *std::max_element(effs.begin(), effs.end()));
    return anchors;
}

double winner_score(const json & result, winner_profile profile, const winner_policy_options & options) {
    const double eval_tps = num_value(result, "eval_tps", 0.0);
    const double power = num_value(result, "gpu_power_peak_w", 0.0);
    const auto anchors = options.has_anchors ? options.anchors : winner_policy_anchors{};
    switch (profile) {
        case winner_profile::speed:
            return eval_tps;
        case winner_profile::efficiency:
            return power > 0 ? eval_tps / power : -std::numeric_limits<double>::infinity();
        case winner_profile::safety:
            return eval_tps;
        case winner_profile::overall: {
            const double speed_part = eval_tps / std::max(1.0, anchors.eval_max);
            const double safety_part = is_safe(result, options.confirm_mib) ? 1.0 : 0.0;
            const double efficiency_part = power > 0 ? (eval_tps / power) / std::max(1.0, anchors.eff_max) : 0.0;
            return 0.5 * speed_part + 0.3 * safety_part + 0.2 * efficiency_part;
        }
    }
    return 0.0;
}

bool is_better_winner(const json & candidate, const json * current, winner_profile profile, const winner_policy_options & options) {
    if (current == nullptr) return true;
    if (profile != winner_profile::safety) {
        return winner_score(candidate, profile, options) > winner_score(*current, profile, options);
    }

    const bool candidate_safe = is_safe(candidate, options.confirm_mib);
    const bool current_safe = is_safe(*current, options.confirm_mib);
    if (candidate_safe != current_safe) return candidate_safe;

    const double candidate_eval = num_value(candidate, "eval_tps", -1.0);
    const double current_eval = num_value(*current, "eval_tps", -1.0);
    const double best_eval = std::max(candidate_eval, current_eval);
    if (best_eval > 0 && std::abs(candidate_eval - current_eval) / best_eval > options.tie_band_pct) {
        return candidate_eval > current_eval;
    }

    const double candidate_kv = kv_quality_value(candidate);
    const double current_kv = kv_quality_value(*current);
    if (candidate_kv != current_kv) return candidate_kv > current_kv;

    const int64_t candidate_ctx = ctx_value(candidate);
    const int64_t current_ctx = ctx_value(*current);
    if (candidate_ctx != current_ctx) return candidate_ctx > current_ctx;

    const double candidate_shared = num_value(candidate, "shared_peak_mib", 0.0);
    const double current_shared = num_value(*current, "shared_peak_mib", 0.0);
    if (candidate_shared != current_shared) return candidate_shared < current_shared;

    const double candidate_vram = num_value(candidate, "vram_peak_mib", (double) std::numeric_limits<int64_t>::max());
    const double current_vram = num_value(*current, "vram_peak_mib", (double) std::numeric_limits<int64_t>::max());
    if (candidate_vram != current_vram) return candidate_vram < current_vram;

    return candidate_eval > current_eval;
}

std::map<std::string, json> group_winners(const std::vector<json> & results, winner_profile profile, const winner_policy_options & options) {
    std::map<std::string, json> by_model;
    std::map<std::string, json> normal_fallbacks;
    std::map<std::string, json> limited_by_model;
    std::map<std::string, json> limited_fallbacks;

    auto ptr_for = [](std::map<std::string, json> & map, const std::string & key) -> json * {
        auto it = map.find(key);
        return it == map.end() ? nullptr : &it->second;
    };

    for (const auto & result : results) {
        if (!bool_value(result, "ok", false) || !is_winner_eligible(result)) continue;
        std::string model = str_value(result, "model");
        if (model.empty()) model = str_value(result, "id");
        if (model.empty()) continue;

        const bool normal_eligible = is_normal_winner_eligible(result);
        const bool limited_eligible = is_limited_winner_fallback_eligible(result);
        if (!normal_eligible && !limited_eligible) continue;

        const double score = winner_score(result, profile, options);
        if (normal_eligible && score > -std::numeric_limits<double>::infinity() && is_better_winner(result, ptr_for(by_model, model), profile, options)) {
            by_model[model] = result;
            by_model[model]["_score"] = score;
        } else if (limited_eligible && score > -std::numeric_limits<double>::infinity() && is_better_winner(result, ptr_for(limited_by_model, model), profile, options)) {
            limited_by_model[model] = result;
            limited_by_model[model]["_score"] = score;
        }

        const double speed_score = winner_score(result, winner_profile::speed, options);
        if (normal_eligible && is_better_winner(result, ptr_for(normal_fallbacks, model), winner_profile::speed, options)) {
            normal_fallbacks[model] = result;
            normal_fallbacks[model]["_score"] = speed_score;
        } else if (limited_eligible && is_better_winner(result, ptr_for(limited_fallbacks, model), winner_profile::speed, options)) {
            limited_fallbacks[model] = result;
            limited_fallbacks[model]["_score"] = speed_score;
        }
    }

    for (const auto & [model, fallback] : normal_fallbacks) {
        if (!by_model.count(model)) {
            by_model[model] = fallback;
            by_model[model]["_fallback"] = true;
        }
    }
    for (const auto & [model, fallback] : limited_by_model) {
        if (!by_model.count(model)) {
            by_model[model] = fallback;
            by_model[model]["_fallback"] = true;
        }
    }
    for (const auto & [model, fallback] : limited_fallbacks) {
        if (!by_model.count(model)) {
            by_model[model] = fallback;
            by_model[model]["_fallback"] = true;
        }
    }
    return by_model;
}

winner_profile winner_profile_from_string(const std::string & profile) {
    if (profile == "efficiency") return winner_profile::efficiency;
    if (profile == "safety") return winner_profile::safety;
    if (profile == "overall") return winner_profile::overall;
    return winner_profile::speed;
}

std::map<std::string, json> derive_memory_policies(const std::vector<json> & rows, double vram_total_mib, const memory_policy_options & options) {
    const double shared_threshold = options.shared_threshold_mib;
    const double shared_minor = options.shared_minor_upper_mib;
    const double shared_pressure = options.shared_pressure_upper_mib;
    const double degradation_threshold = options.degradation_threshold;
    const double vram_budget_input = options.vram_budget_mib;
    const double vram_driver_usable = options.vram_driver_usable_mib;
    std::string vram_budget_source = options.vram_budget_source;
    if (vram_budget_source.empty()) {
        if (vram_driver_usable >= 0) vram_budget_source = "nvidia_fb_reserved";
        else if (vram_budget_input >= 0) vram_budget_source = "configured";
    }

    std::map<std::string, std::vector<json>> groups;
    for (const auto & row : rows) groups[key_for_group(row)].push_back(row);

    std::map<std::string, json> out;
    for (const auto & [_, group] : groups) {
        const bool is_moe = std::any_of(group.begin(), group.end(), [](const json & row) { return str_value(row, "sweep") == "moe-cpu"; });
        const auto context_growth = measured_context_growth(group, shared_threshold);

        std::vector<std::pair<double, double>> clean_loads;
        for (const auto & row : group) {
            if (!bool_value(row, "ok", false)) continue;
            if (str_value(row, "workload_kind", "baseline") != "baseline") continue;
            if (str_value(row, "sweep") != "context") continue;
            if (num_value(row, "shared_peak_mib", 0.0) > shared_threshold) continue;
            const double tokens = context_tokens(row);
            const double memory = memory_pressure_mib(row);
            if (std::isfinite(tokens) && std::isfinite(memory)) clean_loads.push_back({tokens, memory});
        }
        std::sort(clean_loads.begin(), clean_loads.end());

        double estimated_cliff = std::numeric_limits<double>::quiet_NaN();
        if (clean_loads.size() >= 2 && vram_total_mib > 0) {
            const auto a = clean_loads[clean_loads.size() - 2];
            const auto b = clean_loads[clean_loads.size() - 1];
            const double slope = (b.second - a.second) / (b.first - a.first);
            if (slope > 0) estimated_cliff = std::round(b.first + std::max(0.0, vram_total_mib - b.second) / slope);
        }

        struct fill_t { std::string id; double tokens; double eval; double shared; };
        std::vector<fill_t> fills;
        for (const auto & row : group) {
            if (!bool_value(row, "ok", false) || str_value(row, "workload_kind") != "kv-fill") continue;
            double tokens = std::isfinite(nullable_num(row, "kv_fill_cached_tokens")) ? nullable_num(row, "kv_fill_cached_tokens") : nullable_num(row, "kv_fill_target_tokens");
            double eval = nullable_num(row, "eval_tps");
            if (std::isfinite(tokens) && std::isfinite(eval)) fills.push_back({str_value(row, "id"), tokens, eval, num_value(row, "shared_peak_mib", 0.0)});
        }
        std::sort(fills.begin(), fills.end(), [](const fill_t & a, const fill_t & b) { return a.tokens < b.tokens; });

        struct degr_t { double pct; bool correlated; };
        std::map<std::string, degr_t> degradation_by_id;
        if (std::isfinite(estimated_cliff)) {
            for (const auto & after : fills) {
                if (after.tokens < estimated_cliff) continue;
                std::vector<fill_t> before;
                for (const auto & point : fills) {
                    if (point.tokens < after.tokens && point.tokens < estimated_cliff) before.push_back(point);
                }
                if (before.size() < 2) continue;
                const auto a = before[before.size() - 2];
                const auto b = before[before.size() - 1];
                const double clean_slope = (b.eval - a.eval) / std::max(1.0, b.tokens - a.tokens);
                const double expected = std::max(0.001, b.eval + clean_slope * (after.tokens - b.tokens));
                const double degradation = std::max(0.0, (expected - after.eval) / expected);
                degradation_by_id[after.id] = {degradation, after.shared > shared_threshold && degradation > degradation_threshold};
            }
        }

        for (const auto & row : group) {
            const double shared = num_value(row, "shared_peak_mib", 0.0);
            const double baseline = num_value(row, "vram_baseline_mib", 0.0);
            const double budget_mib = vram_budget_input >= 0 ? vram_budget_input : (vram_driver_usable >= 0 ? vram_driver_usable : (vram_total_mib > 0 ? vram_total_mib : std::numeric_limits<double>::quiet_NaN()));
            const double available_budget = std::isfinite(budget_mib) && budget_mib > 0 ? std::max(0.0, budget_mib - baseline) : std::numeric_limits<double>::quiet_NaN();
            const double run_peak = run_vram_mib(row);
            const double total_peak = total_vram_peak_mib(row, baseline);
            const double pressure_ratio = std::isfinite(available_budget) && available_budget > 0 && std::isfinite(run_peak) ? run_peak / available_budget : std::numeric_limits<double>::quiet_NaN();
            const double headroom = std::isfinite(budget_mib) && std::isfinite(total_peak) ? budget_mib - total_peak : std::numeric_limits<double>::quiet_NaN();
            const double row_ctx = context_tokens(row);
            const double model_max_ctx = nullable_num(row, "gguf_context_length");
            double next_ctx = std::numeric_limits<double>::quiet_NaN();
            for (const auto & candidate : group) {
                const double tokens = context_tokens(candidate);
                if (std::isfinite(tokens) && std::isfinite(row_ctx) && tokens > row_ctx && (!std::isfinite(next_ctx) || tokens < next_ctx)) next_ctx = tokens;
            }
            const double next_cost = std::isfinite(context_growth.slope_mib_per_1k) && std::isfinite(row_ctx) && std::isfinite(next_ctx)
                ? ((next_ctx - row_ctx) / 1024.0) * context_growth.slope_mib_per_1k
                : std::numeric_limits<double>::quiet_NaN();
            const bool at_model_max = std::isfinite(row_ctx) && std::isfinite(model_max_ctx) && row_ctx >= model_max_ctx;
            std::string next_risk;
            if (at_model_max) next_risk = "not-applicable";
            else if (!std::isfinite(next_cost) || !std::isfinite(headroom)) next_risk = "unknown";
            else next_risk = next_cost - headroom > shared_threshold ? "imminent" : "ok";
            std::string next_reason;
            if (at_model_max) next_reason = "The run is already at the model's declared maximum context, so there is no next context step to predict.";
            else if (next_risk == "unknown") next_reason = "No comparable measured next context step is available for this profile.";
            else if (next_risk == "imminent") next_reason = "The measured context-growth slope predicts significant shared-memory pressure at the next context step.";
            else next_reason = "The measured context-growth slope does not predict significant shared-memory pressure at the next context step.";

            std::string state = std::isfinite(headroom) && headroom < headroom_thresholds(budget_mib).loaded ? "saturated" : "dedicated";
            std::string reason = state == "saturated"
                ? "Dedicated VRAM is near capacity; no significant shared allocation was observed."
                : "The measured allocation remained in dedicated VRAM.";

            auto degradation_it = degradation_by_id.find(str_value(row, "id"));
            const bool has_degradation = degradation_it != degradation_by_id.end();
            const bool correlated = has_degradation && degradation_it->second.correlated;
            if (shared > 0 && shared <= shared_threshold) {
                state = "shared_allocated";
                reason = "Shared allocation increased, but stayed below the configured significance threshold.";
            } else if (shared > shared_threshold && is_moe) {
                state = "moe_shared_ambiguous";
                reason = "MoE CPU expert mapping and GPU spill cannot yet be separated reliably.";
            } else if (shared > shared_threshold && correlated) {
                state = "spill_correlated_degradation";
                reason = "KV-fill crossed the estimated memory boundary and throughput degraded beyond the clean trend.";
            } else if (shared > shared_threshold) {
                state = "spill_risk";
                reason = fills.empty()
                    ? "Might spill with high context usage; KV-fill validation was not collected."
                    : "Shared allocation increased, but KV-fill did not confirm correlated degradation.";
            }

            std::string residency;
            std::string residency_reason;
            if (is_memory_failure(row)) {
                residency = "memory-failed";
                residency_reason = "The configuration failed during load or execution with a memory-related error.";
            } else if (is_mixed_expected(row)) {
                residency = shared > shared_threshold ? "mixed-pressure" : "mixed-expected";
                residency_reason = residency == "mixed-pressure"
                    ? "The run intentionally mixes GPU and host/offloaded memory, with significant shared-memory pressure observed."
                    : "The run intentionally mixes GPU and host/offloaded memory; shared/RAM use is expected for this strategy.";
            } else if (shared <= 0 && std::isfinite(headroom) && headroom < headroom_thresholds(budget_mib).loaded) {
                residency = "vram-pressure";
                residency_reason = "The run stayed in dedicated VRAM, but has low absolute VRAM headroom.";
            } else if (shared <= 0) {
                residency = "vram-only";
                residency_reason = "The run stayed within dedicated VRAM with no significant shared allocation.";
            } else if (shared <= shared_threshold) {
                residency = std::isfinite(headroom) && headroom < headroom_thresholds(budget_mib).loaded ? "vram-pressure" : "vram-only";
                residency_reason = "Shared allocation stayed below the configured significance threshold.";
            } else if (correlated) {
                residency = "spill-confirmed";
                residency_reason = "KV-fill crossed the estimated memory boundary and throughput degraded beyond the clean trend.";
            } else if (fills.empty()) {
                residency = "spill-risk";
                residency_reason = "Shared allocation exceeded the significance threshold, but KV-fill validation was not collected.";
            } else {
                residency = "shared-allocated";
                residency_reason = "Shared allocation exceeded the significance threshold, but correlated degradation was not confirmed.";
            }

            const double cliff_pct = has_degradation ? std::round(degradation_it->second.pct * 1000.0) / 10.0 : std::numeric_limits<double>::quiet_NaN();
            const std::string resource_fit = resource_fit_from_headroom(headroom, budget_mib, residency, shared, shared_threshold, shared_minor, shared_pressure);
            const std::string decode = usability_from_eval(row);
            const auto response = usability_from_response_latency(row);
            const std::string degradation = degradation_band(cliff_pct);
            const std::string confidence = cap_confidence_for_anomalous_degradation(confidence_for_row(row, residency), resource_fit, degradation);
            std::string recommendation_reason;
            const std::string recommendation = recommendation_for(row, resource_fit, decode, degradation, recommendation_reason);

            json policy = {
                {"memory_state", state},
                {"memory_state_reason", reason},
                {"estimated_cliff_tokens", number_or_null(estimated_cliff, 0)},
                {"cliff_degradation_pct", number_or_null(cliff_pct, 1)},
                {"context_growth_mib_per_1k", number_or_null(context_growth.slope_mib_per_1k, 1)},
                {"context_growth_confidence", std::isfinite(context_growth.slope_mib_per_1k) ? "measured" : "unavailable"},
                {"context_growth_source", context_growth.source.empty() ? json(nullptr) : json(context_growth.source)},
                {"next_context_step_headroom_mib", number_or_null(headroom, 1)},
                {"next_context_step_cost_mib", number_or_null(next_cost, 1)},
                {"next_context_step_risk", next_risk},
                {"next_context_step_reason", next_reason},
                {"residency", residency},
                {"residency_reason", residency_reason},
                {"vram_available_for_run_mib", number_or_null(available_budget, 0)},
                {"vram_driver_usable_mib", vram_driver_usable >= 0 ? json(vram_driver_usable) : json(nullptr)},
                {"vram_budget_source", vram_budget_source.empty() ? json(nullptr) : json(vram_budget_source)},
                {"vram_run_headroom_mib", number_or_null(headroom, 1)},
                {"vram_pressure_ratio", number_or_null(pressure_ratio, 3)},
                {"resource_fit", resource_fit},
                {"decode_usability", decode},
                {"response_usability", response.first},
                {"response_usability_ms", number_or_null(response.second, 1)},
                {"usability", decode},
                {"degradation", degradation},
                {"measurement_confidence", confidence},
                {"recommendation", recommendation},
                {"recommendation_reason", recommendation_reason},
            };
            out[str_value(row, "id")] = policy;
        }
    }
    return out;
}

double median(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return !std::isfinite(v); }), values.end());
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1) / 2];
}

json parse_llama_server_stderr(const std::string & stderr_text) {
    json out = {{"fit_status", "unknown"}};
    const std::vector<std::pair<const char *, std::string>> numeric = {
        {"cpu_model_mib", R"(CPU model buffer size\s*=\s*([\d.]+))"},
        {"cuda_model_mib", R"(CUDA0 model buffer size\s*=\s*([\d.]+))"},
        {"kv_cache_mib", R"(CUDA0 KV buffer size\s*=\s*([\d.]+))"},
        {"compute_cuda_mib", R"(CUDA0 compute buffer size\s*=\s*([\d.]+))"},
        {"compute_host_mib", R"(CUDA_Host compute buffer size\s*=\s*([\d.]+))"},
    };
    for (const auto & [key, pattern] : numeric) {
        std::smatch m;
        if (std::regex_search(stderr_text, m, std::regex(pattern))) out[key] = std::stod(m[1].str());
    }
    std::smatch m;
    if (std::regex_search(stderr_text, m, std::regex(R"(offloaded (\d+)\/(\d+) layers)"))) out["layers_offloaded"] = m[1].str() + "/" + m[2].str();
    if (std::regex_search(stderr_text, m, std::regex(R"(new slot, n_ctx\s*=\s*(\d+))"))) out["effective_context_size"] = std::stoi(m[1].str());
    if (std::regex_search(stderr_text, m, std::regex(R"(initializing slots, n_slots\s*=\s*(\d+))"))) out["effective_parallel_slots"] = std::stoi(m[1].str());
    if (std::regex_search(stderr_text, m, std::regex(R"(n_parallel\s*(?:is set to auto,\s*using\s*)?n_parallel\s*=\s*(\d+))"))) out["effective_n_parallel"] = std::stoi(m[1].str());
    if (regex_has(stderr_text, "Flash Attention was auto, set to disabled", std::regex::icase)) out["flash_attention_state"] = "auto-disabled";
    else if (regex_has(stderr_text, "flash attention.*disabled", std::regex::icase)) out["flash_attention_state"] = "disabled";
    else if (regex_has(stderr_text, "flash attention.*enabled", std::regex::icase)) out["flash_attention_state"] = "enabled";
    if (std::regex_search(stderr_text, m, std::regex(R"(unknown model architecture: '([^']+)')"))) out["unsupported_architecture"] = m[1].str();
    if (stderr_text.find("successfully fit params") != std::string::npos) out["fit_status"] = "success";
    else if (stderr_text.find("failed to fit params") != std::string::npos) out["fit_status"] = "failed_but_running";
    return out;
}

std::string infer_fit_status(const std::string & status, bool ok, double shared_peak_mib, double shared_confirm_mib) {
    if (status == "success" || status == "failed_but_running") return status;
    if (!ok) return status;
    return shared_peak_mib > shared_confirm_mib ? "failed_but_running" : "success";
}

std::string get_failure_reason(const json & result) {
    if (bool_value(result, "ok", false)) return "";
    if (has_key(result, "failure") && result.at("failure").is_object() && has_key(result.at("failure"), "cause")) {
        return str_value(result.at("failure"), "cause");
    }
    const std::string error = str_value(result, "error");
    if (error.rfind("llama.cpp compatibility check failed:", 0) == 0) return "unsupported_argument";
    if (regex_has(error, "GGML_ASSERT|ggml assertion|assertion.*failed", std::regex::icase)) return "engine_assert";
    if (has_key(result, "unsupported_architecture")) return "unsupported_architecture";
    if (str_value(result, "fit_status") == "failed_but_running") return "load_oom";
    if (has_key(result, "ready") && result.at("ready").is_boolean() && !result.at("ready").get<bool>()) return "load_process_exit";
    return "unknown";
}

int64_t context_size_from_args(const std::string & extra_args) {
    const int64_t value = arg_int(extra_args, {"--ctx-size"}, 0);
    return value > 0 ? value : 0;
}

json derive_result_fields(const json & result, double vram_total_mib) {
    const double prompt_n = num_value(result, "prompt_n", 0.0);
    const double eval_n = num_value(result, "eval_n", 0.0);
    const double prompt_tps = num_value(result, "prompt_tps", 0.0);
    const double eval_tps = num_value(result, "eval_tps", 0.0);
    const double time_total = prompt_n > 0 && eval_n > 0 && prompt_tps > 0 && eval_tps > 0
        ? round_to((prompt_n / prompt_tps) + (eval_n / eval_tps), 2)
        : std::numeric_limits<double>::quiet_NaN();
    const int64_t headroom = std::max<int64_t>(0, (int64_t) std::trunc(vram_total_mib) - int_value(result, "vram_peak_mib"));
    return {
        {"time_total_sec", number_or_null(time_total, 2)},
        {"headroom_mib", headroom},
        {"ctx_size", context_size_from_args(str_value(result, "extra_args")) > 0 ? json(context_size_from_args(str_value(result, "extra_args"))) : json(nullptr)},
    };
}

json run_stats(const json & result) {
    std::vector<double> samples;
    if (has_key(result, "runs") && result.at("runs").is_array()) samples = nums_from_runs(result.at("runs"), "eval_tps");
    if (samples.empty()) {
        const double top = nullable_num(result, "eval_tps");
        if (std::isfinite(top)) samples.push_back(top);
    }
    const double run_count = std::isfinite(nullable_num(result, "run_count")) ? nullable_num(result, "run_count") : (!has_key(result, "runs") ? samples.size() : result.at("runs").size());
    const double first = std::isfinite(nullable_num(result, "first_eval_tps")) ? nullable_num(result, "first_eval_tps") : (samples.empty() ? std::numeric_limits<double>::quiet_NaN() : samples.front());
    std::vector<double> repeats = samples.size() > 1 ? std::vector<double>(samples.begin() + 1, samples.end()) : std::vector<double>{};
    const double repeat = std::isfinite(nullable_num(result, "repeat_eval_tps")) ? nullable_num(result, "repeat_eval_tps") : median(repeats);
    const double min_eval = std::isfinite(nullable_num(result, "eval_min_tps")) ? nullable_num(result, "eval_min_tps") : (samples.empty() ? std::numeric_limits<double>::quiet_NaN() : *std::min_element(samples.begin(), samples.end()));
    const double max_eval = std::isfinite(nullable_num(result, "eval_max_tps")) ? nullable_num(result, "eval_max_tps") : (samples.empty() ? std::numeric_limits<double>::quiet_NaN() : *std::max_element(samples.begin(), samples.end()));
    double spread = nullable_num(result, "eval_spread_pct");
    const double eval_tps = nullable_num(result, "eval_tps");
    if (!std::isfinite(spread)) {
        spread = samples.size() > 1 && std::isfinite(eval_tps) && eval_tps > 0 && std::isfinite(min_eval) && std::isfinite(max_eval)
            ? round_to(((max_eval - min_eval) / eval_tps) * 100.0, 1)
            : 0.0;
    }
    return {
        {"run_count", (int64_t) std::trunc(run_count)},
        {"first_eval_tps", number_or_null(first, 2)},
        {"repeat_eval_tps", number_or_null(repeat, 2)},
        {"eval_min_tps", number_or_null(min_eval, 2)},
        {"eval_max_tps", number_or_null(max_eval, 2)},
        {"eval_spread_pct", spread},
    };
}

json aggregate_bench_result(const json & item, const json & cfg, const std::vector<json> & runs, const json & session) {
    if (runs.empty()) throw std::runtime_error("aggregate_bench_result requires at least one run");
    const json & first = runs.front();
    const json hardware = has_key(cfg, "hardware") ? cfg.at("hardware") : json::object();
    const json wddm = has_key(cfg, "wddm_detection") ? cfg.at("wddm_detection") : json::object();
    const double vram_total = num_value(hardware, "vram_total_mib", 0.0);
    const double confirm = num_value(wddm, "shared_delta_confirm_mib", 500.0);
    const double sat_threshold = num_value(wddm, "vram_saturation_threshold", 0.92);

    auto collect = [&](const char * key) {
        std::vector<double> vals;
        for (const auto & run : runs) {
            const double v = nullable_num(run, key);
            if (std::isfinite(v)) vals.push_back(v);
        }
        return vals;
    };

    const double vram_peak_med = std::trunc(median(collect("vram_peak_mib")));
    std::vector<double> vram_total_vals;
    for (const auto & run : runs) {
        const double v = std::isfinite(nullable_num(run, "vram_total_peak_mib")) ? nullable_num(run, "vram_total_peak_mib") : nullable_num(run, "vram_peak_mib");
        if (std::isfinite(v)) vram_total_vals.push_back(v);
    }
    const double vram_total_peak_med = std::trunc(median(vram_total_vals));
    std::vector<double> baseline_vals;
    for (const auto & run : runs) {
        const double v = std::isfinite(nullable_num(run, "vram_baseline_mib")) ? nullable_num(run, "vram_baseline_mib") : nullable_num(run, "vram_before_mib");
        if (std::isfinite(v)) baseline_vals.push_back(v);
    }
    const double shared_peak_med = std::trunc(median(collect("shared_peak_mib")));
    const double prompt_tps_med = round_to(median(collect("prompt_tps")), 2);
    const double eval_tps_med = round_to(median(collect("eval_tps")), 2);

    std::vector<std::string> starts;
    std::vector<std::string> ends;
    double duration_total = 0.0;
    double energy_wh = 0.0;
    double energy_j = 0.0;
    for (const auto & run : runs) {
        const auto s = str_value(run, "run_started_at");
        const auto e = str_value(run, "run_ended_at");
        if (!s.empty()) starts.push_back(s);
        if (!e.empty()) ends.push_back(e);
        duration_total += num_value(run, "run_duration_ms", 0.0);
        energy_wh += num_value(run, "gpu_energy_wh", 0.0);
        energy_j += num_value(run, "gpu_energy_j", 0.0);
    }
    std::sort(starts.begin(), starts.end());
    std::sort(ends.begin(), ends.end());
    const double sat_ratio = vram_total > 0 ? round_to(vram_peak_med / vram_total, 3) : 0.0;
    const std::string extra_args = str_value(item, "extra_args");

    json result = {
        {"metric_schema_version", METRIC_SCHEMA_VERSION},
        {"id", item.value("id", json(nullptr))},
        {"label", item.value("label", json(nullptr))},
        {"model", item.value("model", json(nullptr))},
        {"variant", item.value("variant", json(nullptr))},
        {"series", item.value("series", json(nullptr))},
        {"level", item.value("level", json(nullptr))},
        {"sweep", item.value("sweep", json(nullptr))},
        {"workload_kind", str_value(item, "workload_kind", "baseline")},
        {"control_kind", item.value("control_kind", json(nullptr))},
        {"row_role", infer_row_role(item)},
        {"fair_control_target_id", item.value("fair_control_target_id", json(nullptr))},
        {"prefill_target_tokens", int_value(item, "prefill_target_tokens")},
        {"kv_fill_target_tokens", int_value(item, "kv_fill_target_tokens")},
        {"gguf_context_length", item.value("gguf_context_length", json(nullptr))},
        {"gguf_architecture", item.value("gguf_architecture", json(nullptr))},
        {"planning_mode", item.value("planning_mode", json(nullptr))},
        {"calibration_id", item.value("calibration_id", json(nullptr))},
        {"predicted_fit_layers", item.value("predicted_fit_layers", json(nullptr))},
        {"verified_fit_layers", item.value("verified_fit_layers", json(nullptr))},
        {"first_spill_layers", item.value("first_spill_layers", json(nullptr))},
        {"probe_count", item.value("probe_count", json(nullptr))},
        {"fit_offset", item.value("fit_offset", json(nullptr))},
        {"calibration_cache_hit", item.value("calibration_cache_hit", json(nullptr))},
        {"calibration_cache_age_hours", item.value("calibration_cache_age_hours", json(nullptr))},
        {"predicted_n_cpu_moe", item.value("predicted_n_cpu_moe", json(nullptr))},
        {"verified_n_cpu_moe", item.value("verified_n_cpu_moe", json(nullptr))},
        {"first_spill_n_cpu_moe", item.value("first_spill_n_cpu_moe", json(nullptr))},
        {"dynamic_plan_reason", item.value("dynamic_plan_reason", json(nullptr))},
        {"timestamp", first.value("timestamp", json(nullptr))},
        {"run_started_at", starts.empty() ? json(nullptr) : json(starts.front())},
        {"run_ended_at", ends.empty() ? json(nullptr) : json(ends.back())},
        {"run_duration_ms", round_to(duration_total, 2)},
        {"run_duration_median_ms", number_or_null(metric_median(runs, "run_duration_ms"), 2)},
        {"gpu_energy_wh", round_to(energy_wh, 4)},
        {"gpu_energy_j", round_to(energy_j, 2)},
        {"model_path", item.value("model_path", json(nullptr))},
        {"mmproj_path", item.value("mmproj_path", json(nullptr))},
        {"extra_args", item.value("extra_args", json(nullptr))},
        {"requested_context_size", arg_int(extra_args, {"--ctx-size", "-c"}, 0) > 0 ? json(arg_int(extra_args, {"--ctx-size", "-c"})) : json(nullptr)},
        {"requested_cache_type_k", arg_value(extra_args, {"--cache-type-k", "-ctk"}).empty() ? json(nullptr) : json(arg_value(extra_args, {"--cache-type-k", "-ctk"}))},
        {"requested_cache_type_v", arg_value(extra_args, {"--cache-type-v", "-ctv"}).empty() ? json(nullptr) : json(arg_value(extra_args, {"--cache-type-v", "-ctv"}))},
        {"requested_gpu_layers", arg_int(extra_args, {"--gpu-layers", "-ngl"}, 0) > 0 ? json(arg_int(extra_args, {"--gpu-layers", "-ngl"})) : json(nullptr)},
        {"requested_n_cpu_moe", arg_int(extra_args, {"--n-cpu-moe", "-ncmoe"}, 0) > 0 ? json(arg_int(extra_args, {"--n-cpu-moe", "-ncmoe"})) : json(nullptr)},
        {"requested_parallel", arg_int(extra_args, {"--parallel", "-np"}, 0) > 0 ? json(arg_int(extra_args, {"--parallel", "-np"})) : json(nullptr)},
        {"vram_before_mib", first.value("vram_before_mib", json(nullptr))},
        {"vram_baseline_mib", number_or_null(median(baseline_vals), 0)},
        {"vram_baseline_pct", number_or_null(median(collect("vram_baseline_pct")), 4)},
        {"load_sec", first.value("load_sec", json(nullptr))},
        {"load_ms", std::isfinite(nullable_num(first, "load_ms")) ? json(nullable_num(first, "load_ms")) : (std::isfinite(nullable_num(first, "load_sec")) ? json(round_to(nullable_num(first, "load_sec") * 1000.0, 2)) : json(nullptr))},
        {"ready", first.value("ready", json(nullptr))},
        {"prompt_n", first.value("prompt_n", json(nullptr))},
        {"eval_n", first.value("eval_n", json(nullptr))},
        {"cpu_model_mib", first.value("cpu_model_mib", json(nullptr))},
        {"cuda_model_mib", first.value("cuda_model_mib", json(nullptr))},
        {"kv_cache_mib", first.value("kv_cache_mib", json(nullptr))},
        {"compute_cuda_mib", first.value("compute_cuda_mib", json(nullptr))},
        {"compute_host_mib", first.value("compute_host_mib", json(nullptr))},
        {"layers_offloaded", first.value("layers_offloaded", json(nullptr))},
        {"effective_context_size", first.value("effective_context_size", json(nullptr))},
        {"effective_parallel_slots", first.value("effective_parallel_slots", json(nullptr))},
        {"effective_n_parallel", first.value("effective_n_parallel", json(nullptr))},
        {"flash_attention_state", first.value("flash_attention_state", json(nullptr))},
        {"fit_status", str_value(item, "sweep") == "moe-cpu" && str_value(first, "fit_status_source") != "llama.cpp" ? "success" : infer_fit_status(str_value(first, "fit_status"), true, shared_peak_med, confirm)},
        {"fit_status_source", str_value(first, "fit_status_source", "inferred")},
        {"shared_memory_interpretation", str_value(item, "sweep") == "moe-cpu" ? "cpu_expert_mapping_or_wddm_pressure" : "wddm_pressure"},
        {"vram_peak_mib", vram_peak_med},
        {"vram_total_peak_mib", vram_total_peak_med},
        {"vram_process_peak_mib", number_or_null(median(collect("vram_process_peak_mib")), 0)},
        {"vram_external_peak_mib", number_or_null(median(collect("vram_external_peak_mib")), 0)},
        {"shared_peak_mib", shared_peak_med},
        {"prompt_tps", prompt_tps_med},
        {"eval_tps", eval_tps_med},
        {"ttft_sec", round_to(median(collect("ttft_sec")), 3)},
        {"prompt_ms", number_or_null(metric_median(runs, "prompt_ms"), 2)},
        {"ttfr_ms", number_or_null(metric_median(runs, "ttfr_ms"), 2)},
        {"e2e_ttft_ms", number_or_null(metric_median(runs, "e2e_ttft_ms"), 2)},
        {"total_request_ms", number_or_null(metric_median(runs, "total_request_ms"), 2)},
        {"latency_total_request_ms", number_or_null(metric_median(runs, "latency_total_request_ms"), 2)},
        {"gpu_util_avg_pct", (int64_t) std::trunc(median(collect("gpu_util_avg_pct")))},
        {"cpu_util_avg_pct", (int64_t) std::trunc(median(collect("cpu_util_avg_pct")))},
        {"gpu_power_peak_w", round_to(max_from_runs(runs, "gpu_power_peak_w"), 1)},
        {"gpu_temp_peak_c", (int64_t) std::trunc(max_from_runs(runs, "gpu_temp_peak_c"))},
        {"ram_baseline_mib", first.value("ram_baseline_mib", json(nullptr))},
        {"ram_used_peak_mib", (int64_t) std::trunc(max_from_runs(runs, "ram_used_peak_mib"))},
        {"process_working_set_peak_mib", number_or_null(median(collect("process_working_set_peak_mib")), 0)},
        {"process_private_bytes_peak_mib", number_or_null(median(collect("process_private_bytes_peak_mib")), 0)},
        {"process_virtual_bytes_peak_mib", number_or_null(median(collect("process_virtual_bytes_peak_mib")), 0)},
        {"process_disk_read_bytes_delta", (int64_t) std::trunc(max_from_runs(runs, "process_disk_read_bytes_delta"))},
        {"process_disk_read_peak_mb_s", round_to(max_from_runs(runs, "process_disk_read_peak_mb_s"), 1)},
        {"disk_read_peak_mb_s", round_to(max_from_runs(runs, "disk_read_peak_mb_s"), 1)},
        {"wddm_vram_saturation", sat_ratio},
        {"wddm_flag_high_vram", sat_ratio > sat_threshold},
        {"wddm_flag_shared_pos", shared_peak_med > confirm},
        {"ok", true},
        {"error", nullptr},
        {"bench_session_id", str_value(session, "bench_session_id", "unknown")},
        {"bench_session_started_at", str_value(session, "bench_session_started_at")},
        {"llama_server_version", str_value(session, "llama_server_version", "unknown")},
        {"llama_server_exe", str_value(cfg, "llama_server_exe")},
        {"runs", runs},
    };
    const json stats = run_stats({{"eval_tps", eval_tps_med}, {"runs", runs}});
    for (auto it = stats.begin(); it != stats.end(); ++it) result[it.key()] = it.value();

    memory_policy_options opts;
    opts.vram_budget_mib = num_value(hardware, "vram_budget_mib", -1.0);
    opts.vram_driver_usable_mib = num_value(hardware, "vram_driver_usable_mib", -1.0);
    opts.vram_budget_source = str_value(hardware, "vram_budget_source");
    opts.shared_threshold_mib = num_value(wddm, "shared_delta_confirm_mib", 500.0);
    opts.shared_minor_upper_mib = num_value(wddm, "shared_minor_upper_mib", 1024.0);
    opts.shared_pressure_upper_mib = num_value(wddm, "shared_pressure_upper_mib", 2048.0);
    const auto policies = derive_memory_policies({result}, vram_total, opts);
    const auto pit = policies.find(str_value(result, "id"));
    if (pit != policies.end()) {
        for (auto it = pit->second.begin(); it != pit->second.end(); ++it) result[it.key()] = it.value();
    }
    return result;
}

std::vector<json> build_report_rows(const std::vector<json> & results, double vram_total_mib, const memory_policy_options & options) {
    std::map<std::string, std::pair<double, json>> cold_by_model;
    std::vector<json> ordered = results;
    std::sort(ordered.begin(), ordered.end(), [](const json & a, const json & b) {
        return str_value(a, "timestamp") < str_value(b, "timestamp");
    });
    for (const auto & result : ordered) {
        const std::string model = str_value(result, "model");
        const double disk = num_value(result, "disk_read_peak_mb_s", 0.0);
        const double load_ms = std::isfinite(nullable_num(result, "load_ms")) ? nullable_num(result, "load_ms") :
            (std::isfinite(nullable_num(result, "load_sec")) ? nullable_num(result, "load_sec") * 1000.0 : std::numeric_limits<double>::quiet_NaN());
        auto it = cold_by_model.find(model);
        if (it == cold_by_model.end() || disk > it->second.first) cold_by_model[model] = {disk, number_or_null(load_ms, 2)};
    }
    const auto policies = derive_memory_policies(results, vram_total_mib, options);
    std::vector<json> rows;
    for (const auto & result : results) {
        json row = result;
        const double params_b = infer_capability_params_b(result);
        row["metric_schema_version"] = std::isfinite(nullable_num(row, "metric_schema_version")) ? row["metric_schema_version"] : json(METRIC_SCHEMA_VERSION);
        row["capability_params_b"] = number_or_null(params_b, 3);
        row["capability_class"] = capability_class(params_b);
        row["row_role"] = infer_row_role(result);
        row["failure_reason"] = row.value("failure_reason", json(nullptr));
        row["ready"] = row.value("ready", json(nullptr));
        row["stream_open_ms"] = std::isfinite(nullable_num(row, "stream_open_ms")) ? row["stream_open_ms"] : row.value("ttfr_ms", json(nullptr));
        row["client_ttft_ms"] = std::isfinite(nullable_num(row, "client_ttft_ms")) ? row["client_ttft_ms"] : row.value("e2e_ttft_ms", json(nullptr));
        const auto cold = cold_by_model.find(str_value(row, "model"));
        row["model_cold_load_ms"] = cold == cold_by_model.end() ? json(nullptr) : cold->second.second;
        row["model_cold_disk_read_peak_mb_s"] = cold == cold_by_model.end() ? json(nullptr) : json(cold->second.first);
        const json derived = derive_result_fields(result, vram_total_mib);
        for (auto it = derived.begin(); it != derived.end(); ++it) row[it.key()] = it.value();
        const json stats = run_stats(result);
        for (auto it = stats.begin(); it != stats.end(); ++it) row[it.key()] = it.value();
        auto pit = policies.find(str_value(row, "id"));
        if (pit != policies.end()) {
            for (auto it = pit->second.begin(); it != pit->second.end(); ++it) row[it.key()] = it.value();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace caliber
