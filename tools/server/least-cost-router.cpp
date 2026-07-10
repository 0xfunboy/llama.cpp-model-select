#include "least-cost-router.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace least_cost_router {
namespace {

const std::set<std::string> VIRTUAL_ALIASES = {
    "local-auto", "local-fast", "local-best", "local-code", "local-long", "local-vision",
};

std::string text(const json & value, const char * key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return value;
}

bool contains_ci(const json & values, const std::string & needle) {
    if (!values.is_array()) return false;
    for (const auto & value : values) if (value.is_string() && lower(value.get<std::string>()).find(lower(needle)) != std::string::npos) return true;
    return false;
}

int estimated_input_tokens(const json & request) {
    size_t characters = 0;
    if (request.contains("messages") && request["messages"].is_array()) {
        for (const auto & message : request["messages"]) {
            if (!message.contains("content")) continue;
            if (message["content"].is_string()) characters += message["content"].get<std::string>().size();
            else characters += message["content"].dump().size();
        }
    } else if (request.contains("prompt")) {
        characters = request["prompt"].is_string() ? request["prompt"].get<std::string>().size() : request["prompt"].dump().size();
    }
    return std::max(1, (int) std::ceil(characters / 3.5));
}

bool request_has_image(const json & request) {
    return request.dump().find("image_url") != std::string::npos || request.dump().find("input_image") != std::string::npos;
}

std::string required_pack(const std::string & alias, const json & request) {
    if (alias == "local-code") return "coding";
    if (alias == "local-long") return "long-context";
    if (request.contains("tools") && request["tools"].is_array() && !request["tools"].empty()) return "tools";
    return "overall";
}

bool row_matches(const json & row, const json & candidate) {
    const std::string artifact = text(candidate, "artifact_id");
    const std::string model = text(candidate, "model");
    return (!artifact.empty() && text(row, "artifact_id") == artifact) || text(row, "model") == model;
}

bool evidence_eligible(const json & row, const std::string & pack) {
    if (!row.value("ok", false) || text(row, "benchmark_backend") != "llama-server-streaming" ||
        !row.value("context_target_met", false) || !row.value("fit_eligible", false)) return false;
    if (!row.value("quality_gate_passed", false)) return false;
    if (pack == "overall") return true;
    const json policy = row.value("quality_policy", json::object());
    return policy.value("pack", std::string()) == pack;
}

std::vector<json> evidence_rows(const std::vector<json> & reports, const json & candidate, const std::string & pack) {
    std::vector<json> rows;
    for (const auto & report : reports) {
        if (text(report, "status") != "completed" || !report.contains("rows") || !report["rows"].is_array()) continue;
        for (const auto & row : report["rows"]) if (row_matches(row, candidate) && evidence_eligible(row, pack)) rows.push_back(row);
    }
    return rows;
}

double number(const json & value, const char * key, double fallback = 0) {
    if (!value.contains(key) || !value[key].is_number()) return fallback;
    const double result = value[key].get<double>();
    return std::isfinite(result) ? result : fallback;
}

double quality(const json & row) {
    const json evidence = row.value("quality_evidence", json::object());
    return number(evidence, "score", 0.0);
}

std::string decision_id(int64_t now_ms, const std::string & alias, const std::string & model) {
    std::ostringstream out;
    out << "route-" << now_ms << '-' << alias << '-' << model;
    return out.str();
}

} // namespace

bool is_virtual_alias(const std::string & model) { return VIRTUAL_ALIASES.count(model) != 0; }

json aliases() {
    return {
        {"local-auto", {{"objective", "balanced"}, {"quality_pack", "overall"}}},
        {"local-fast", {{"objective", "latency"}, {"quality_pack", "overall"}}},
        {"local-best", {{"objective", "quality"}, {"quality_pack", "overall"}}},
        {"local-code", {{"objective", "coding"}, {"quality_pack", "coding"}}},
        {"local-long", {{"objective", "long-context"}, {"quality_pack", "long-context"}}},
        {"local-vision", {{"objective", "vision"}, {"quality_pack", "overall"}}},
    };
}

json select(const std::string & alias, const json & request, const std::vector<json> & candidates,
        const std::vector<json> & reports, const json & recent_events, int64_t now_ms) {
    if (!is_virtual_alias(alias)) throw std::invalid_argument("unknown virtual model alias");
    const int input_tokens = estimated_input_tokens(request);
    const int output_tokens = std::max(1, request.value("max_tokens", request.value("n_predict", 256)));
    const int required_context = input_tokens + output_tokens + 256;
    const bool needs_vision = alias == "local-vision" || request_has_image(request);
    const bool needs_tools = request.contains("tools") && request["tools"].is_array() && !request["tools"].empty();
    const std::string pack = required_pack(alias, request);

    struct scored { json candidate; json evidence; double score = -1e9; double quality = 0; };
    std::vector<scored> eligible;
    json rejected = json::array();
    for (const auto & candidate : candidates) {
        const std::string model = text(candidate, "model");
        std::string rejection;
        if (needs_vision && !candidate.value("vision", false)) rejection = "vision capability required";
        else if (needs_tools && !contains_ci(candidate.value("tags", json::array()), "tool") && pack != "tools") rejection = "tool capability required";
        const auto rows = evidence_rows(reports, candidate, pack);
        if (rejection.empty() && rows.empty()) rejection = "no streaming, fit and quality evidence for required pack";
        json best;
        double best_score = -1e9;
        double best_quality = 0;
        if (rejection.empty()) {
            for (const auto & row : rows) {
                const int context = row.value("ctx_size", 0);
                if (context < required_context) continue;
                const double decode = std::max(0.01, number(row, "eval_tps", 0.01));
                const double ttft = std::max(1.0, number(row, "e2e_ttft_ms", number(row, "ttft_sec", 1.0) * 1000));
                const double q = quality(row);
                const double memory_budget = number(row, "vram_budget_mib", number(row, "vram_total_mib", 0.0));
                const double observed_pressure = memory_budget > 0 ? number(row, "vram_peak_mib", 0.0) / memory_budget : 0.0;
                const double memory_pressure = std::clamp(number(row, "memory_pressure", observed_pressure), 0.0, 1.5);
                const double power = number(row, "gpu_power_peak_w", 0.0);
                const double load = std::max(0.0, number(row, "load_sec", 0.0));
                const bool resident = candidate.value("resident", false);
                const double expected_ms = ttft + output_tokens / decode * 1000.0;
                double score = q * 4.0 + std::log1p(decode) * 0.45 - std::log1p(expected_ms) * 0.20 - memory_pressure * 1.5;
                const double expected_energy_wh = power > 0 ? power * expected_ms / 3600000.0 : 0.0;
                if (expected_energy_wh > 0) score -= std::min(0.5, expected_energy_wh * 0.1);
                if (!resident) score -= std::min(1.2, load / 30.0 + 0.20);
                if (resident) score += 0.35;
                if (alias == "local-fast") score += std::log1p(decode) * 0.8 - std::log1p(ttft) * 0.35;
                if (alias == "local-best") score += q * 3.0;
                if ((alias == "local-code" && contains_ci(candidate.value("tags", json::array()), "code")) || alias == "local-long") score += 0.25;
                if (score > best_score) { best_score = score; best = row; best_quality = q; }
            }
            if (!best.is_object()) rejection = "measured context is below the request requirement";
        }
        if (!rejection.empty()) {
            rejected.push_back({{"model", model}, {"reason", rejection}});
            continue;
        }
        eligible.push_back({candidate, best, best_score, best_quality});
    }
    if (eligible.empty()) return {{"ok", false}, {"alias", alias}, {"required_context", required_context}, {"quality_pack", pack},
        {"fallback_alias", alias == "local-auto" ? json(nullptr) : json("local-auto")}, {"rejected", rejected},
        {"reason", "No local preset has the required measured context, modality and quality evidence."}};

    std::sort(eligible.begin(), eligible.end(), [](const scored & a, const scored & b) { return a.score > b.score; });
    scored selected = eligible.front();
    const double quality_tolerance = 0.05;
    for (const auto & candidate : eligible) {
        if (candidate.candidate.value("resident", false) && candidate.quality + quality_tolerance >= selected.quality &&
            candidate.score + 0.15 >= selected.score) { selected = candidate; break; }
    }

    const int64_t ttl_ms = 5 * 60 * 1000;
    if (recent_events.is_array()) {
        for (const auto & event : recent_events) {
            if (text(event, "event_type") != "decision") continue;
            const json previous = event.value("payload", json::object());
            if (text(previous, "alias") != alias || now_ms - previous.value("created_ms", int64_t(0)) > ttl_ms) continue;
            const std::string previous_model = text(previous, "selected_model");
            auto prior = std::find_if(eligible.begin(), eligible.end(), [&](const scored & value) { return text(value.candidate, "model") == previous_model; });
            if (prior != eligible.end() && prior->score + 0.08 >= selected.score) selected = *prior;
            break;
        }
    }

    json alternatives_json = json::array();
    for (const auto & value : eligible) if (text(value.candidate, "model") != text(selected.candidate, "model")) {
        alternatives_json.push_back({{"model", text(value.candidate, "model")}, {"score", value.score}, {"quality", value.quality},
            {"resident", value.candidate.value("resident", false)}});
        if (alternatives_json.size() == 3) break;
    }
    const std::string model = text(selected.candidate, "model");
    const double selected_decode = std::max(0.01, number(selected.evidence, "eval_tps", 0.01));
    const double selected_ttft = std::max(1.0, number(selected.evidence, "e2e_ttft_ms", number(selected.evidence, "ttft_sec", 1.0) * 1000));
    const double expected_decode_ms = output_tokens / selected_decode * 1000.0;
    const double selected_power = number(selected.evidence, "gpu_power_peak_w", 0.0);
    const double expected_energy_wh = selected_power > 0 ? selected_power * (selected_ttft + expected_decode_ms) / 3600000.0 : 0.0;
    return {{"ok", true}, {"id", decision_id(now_ms, alias, model)}, {"created_ms", now_ms}, {"alias", alias},
        {"selected_model", model}, {"artifact_id", selected.candidate.value("artifact_id", json(nullptr))},
        {"required_context", required_context}, {"quality_pack", pack}, {"score", selected.score}, {"quality", selected.quality},
        {"resident", selected.candidate.value("resident", false)}, {"preload_recommended", !selected.candidate.value("resident", false)},
        {"costs", {{"ttft_ms", selected_ttft}, {"expected_decode_ms", expected_decode_ms}, {"expected_energy_wh", expected_energy_wh},
            {"load_switch_penalty_sec", selected.candidate.value("resident", false) ? 0.0 : number(selected.evidence, "load_sec", 0.0)}}},
        {"policy", {{"id", "local-least-cost"}, {"version", 1}, {"ttl_ms", ttl_ms}, {"hysteresis", 0.08}, {"resident_quality_tolerance", quality_tolerance}}},
        {"evidence", selected.evidence}, {"alternatives", alternatives_json}, {"rejected", rejected},
        {"reason", selected.candidate.value("resident", false)
            ? "Selected a qualified resident preset within the quality tolerance to avoid a model switch."
            : "Selected the lowest expected local serving cost among context-, feature- and quality-qualified measured presets."}};
}

} // namespace least_cost_router
