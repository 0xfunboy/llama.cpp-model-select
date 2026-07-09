#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace caliber {

using json = nlohmann::ordered_json;

static constexpr int DEFAULT_CONFIRM_MIB = 500;
static constexpr double DEFAULT_TIE_BAND_PCT = 0.05;
static constexpr int METRIC_SCHEMA_VERSION = 7;

enum class winner_profile {
    speed,
    efficiency,
    safety,
    overall,
};

struct winner_policy_anchors {
    double eval_max = 1.0;
    double eff_max = 1.0;
};

struct winner_policy_options {
    int confirm_mib = DEFAULT_CONFIRM_MIB;
    winner_policy_anchors anchors;
    bool has_anchors = false;
    double tie_band_pct = DEFAULT_TIE_BAND_PCT;
};

double finite_number(const json & value, double fallback = 0.0);
int64_t ctx_value(const json & result);
double kv_quality_value(const json & result);
bool is_safe(const json & result, int confirm_mib = DEFAULT_CONFIRM_MIB);

std::string normalize_row_role(const json & value);
std::string infer_row_role(const json & source);
std::string row_role(const json & result);
bool is_candidate_row(const json & source);
bool is_winner_eligible(const json & result);

std::string measurement_confidence(const json & result);
std::string decode_usability(const json & result);
bool is_usability_winner_eligible(const json & result);
bool is_normal_winner_eligible(const json & result);
bool is_limited_winner_fallback_eligible(const json & result);

winner_policy_anchors compute_anchors(const std::vector<json> & results);
double winner_score(const json & result, winner_profile profile, const winner_policy_options & options = {});
bool is_better_winner(const json & candidate, const json * current, winner_profile profile, const winner_policy_options & options = {});
std::map<std::string, json> group_winners(const std::vector<json> & results, winner_profile profile, const winner_policy_options & options = {});
winner_profile winner_profile_from_string(const std::string & profile);

struct memory_policy_options {
    double shared_threshold_mib = 500.0;
    double shared_minor_upper_mib = 1024.0;
    double shared_pressure_upper_mib = 2048.0;
    double degradation_threshold = 0.20;
    double vram_budget_mib = -1.0;
    double vram_driver_usable_mib = -1.0;
    std::string vram_budget_source;
};

std::map<std::string, json> derive_memory_policies(
        const std::vector<json> & rows,
        double vram_total_mib,
        const memory_policy_options & options = {});

double median(std::vector<double> values);
json parse_llama_server_stderr(const std::string & stderr_text);
std::string infer_fit_status(const std::string & status, bool ok, double shared_peak_mib, double shared_confirm_mib = 500.0);
std::string get_failure_reason(const json & result);
int64_t context_size_from_args(const std::string & extra_args);
json derive_result_fields(const json & result, double vram_total_mib);
json run_stats(const json & result);
json aggregate_bench_result(const json & item, const json & cfg, const std::vector<json> & runs, const json & session = json::object());
std::vector<json> build_report_rows(const std::vector<json> & results, double vram_total_mib, const memory_policy_options & options = {});

} // namespace caliber
