#include "quality-evidence.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace quality_evidence {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return value;
}

std::string text(const json & value, const char * key) {
    return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string();
}

std::string infer_pack(const json & row) {
    const std::string explicit_pack = text(row, "pack");
    if (!explicit_pack.empty()) return explicit_pack;
    const std::string source = lower(text(row, "source"));
    const std::string domain = lower(text(row, "domain"));
    if (source.find("compsec") != std::string::npos || domain.find("linux") != std::string::npos ||
        domain.find("php") != std::string::npos || domain.find("tls") != std::string::npos) return "coding";
    if (domain.find("algebra") != std::string::npos || domain.find("geometry") != std::string::npos ||
        domain.find("physics") != std::string::npos || domain.find("chemistry") != std::string::npos ||
        domain.find("number theory") != std::string::npos || source.find("aime") != std::string::npos) return "reasoning";
    return "general";
}

std::string artifact_for(const json & report, const json & row, const json & registry) {
    std::string artifact = text(row, "artifact_id");
    if (!artifact.empty()) return artifact;
    const std::string model = text(row, "model");
    if (report.contains("artifacts") && report["artifacts"].is_object() && report["artifacts"].contains(model) &&
        report["artifacts"][model].is_string()) return report["artifacts"][model].get<std::string>();
    if (registry.contains("artifacts") && registry["artifacts"].is_array()) {
        for (const auto & candidate : registry["artifacts"]) {
            if (!candidate.contains("configured_ids") || !candidate["configured_ids"].is_array()) continue;
            for (const auto & configured : candidate["configured_ids"]) {
                if (configured.is_string() && configured.get<std::string>() == model) return text(candidate, "artifact_id");
            }
        }
    }
    return {};
}

struct observation {
    bool pass = false;
    std::string report_id;
    std::string created_at;
    json configuration = json::object();
};

} // namespace

json build_profiles(const std::vector<json> & input_reports, const json & registry) {
    std::vector<json> reports = input_reports;
    std::sort(reports.begin(), reports.end(), [](const json & a, const json & b) {
        return text(a, "created_at") < text(b, "created_at");
    });
    std::map<std::string, observation> latest_cases;
    for (const auto & report : reports) {
        if (text(report, "kind") != "eval" || text(report, "status") != "completed" ||
            !report.contains("results") || !report["results"].is_array()) continue;
        json configuration = report.value("evaluation_profile", json::object());
        if (configuration.empty()) {
            configuration = {
                {"temperature", report.value("temperature", 0.0)}, {"thinking", report.value("thinking", true)},
                {"thinking_budget_tokens", report.value("thinking_budget_tokens", 0)},
                {"max_tokens", report.value("max_tokens", 0)}, {"template", report.value("template", std::string("router-default"))},
            };
        }
        for (const auto & row : report["results"]) {
            const std::string artifact = artifact_for(report, row, registry);
            if (artifact.empty()) continue;
            const std::string pack = infer_pack(row);
            const std::string case_id = text(row, "id");
            if (case_id.empty()) continue;
            latest_cases[artifact + "\t" + pack + "\t" + case_id] = {
                row.value("pass", false), text(report, "id"), text(report, "created_at"), configuration,
            };
        }
    }

    struct totals { int pass = 0; int total = 0; std::string report_id; std::string created_at; json configuration = json::object(); };
    std::map<std::string, std::map<std::string, totals>> grouped;
    for (const auto & [key, value] : latest_cases) {
        const size_t first = key.find('\t');
        const size_t second = key.find('\t', first + 1);
        const std::string artifact = key.substr(0, first);
        const std::string pack = key.substr(first + 1, second - first - 1);
        auto & total = grouped[artifact][pack];
        total.total++;
        if (value.pass) total.pass++;
        if (value.created_at >= total.created_at) {
            total.created_at = value.created_at;
            total.report_id = value.report_id;
            total.configuration = value.configuration;
        }
    }

    json profiles = json::object();
    for (const auto & [artifact, packs] : grouped) {
        int all_pass = 0;
        int all_total = 0;
        json pack_json = json::object();
        for (const auto & [name, total] : packs) {
            all_pass += total.pass;
            all_total += total.total;
            pack_json[name] = {{"score", total.total ? (double) total.pass / total.total : 0.0}, {"pass", total.pass},
                {"samples", total.total}, {"report_id", total.report_id}, {"created_at", total.created_at},
                {"configuration", total.configuration}};
        }
        profiles[artifact] = {{"artifact_id", artifact}, {"evidence_level", "quality-tested"},
            {"score", all_total ? (double) all_pass / all_total : 0.0}, {"pass", all_pass}, {"samples", all_total}, {"packs", pack_json}};
    }
    return profiles;
}

std::vector<json> apply_policy(const std::vector<json> & input, const json & profiles, const json & policy) {
    std::vector<json> rows = input;
    const bool required = policy.value("required", true);
    const double floor = std::clamp(policy.value("min_score", 0.5), 0.0, 1.0);
    const int min_samples = std::max(1, policy.value("min_samples", 1));
    const std::string pack = policy.value("pack", std::string("overall"));
    for (auto & row : rows) {
        const std::string artifact = text(row, "artifact_id");
        json evidence = json::object();
        if (!artifact.empty() && profiles.contains(artifact)) {
            const json profile = profiles[artifact];
            if (pack == "overall") evidence = profile;
            else if (profile.contains("packs") && profile["packs"].contains(pack)) evidence = profile["packs"][pack];
        }
        const int samples = evidence.value("samples", 0);
        const double score = evidence.value("score", 0.0);
        const bool passed = samples >= min_samples && score >= floor;
        row["quality_policy"] = {{"required", required}, {"pack", pack}, {"min_score", floor}, {"min_samples", min_samples}};
        row["quality_evidence"] = evidence.empty() ? json(nullptr) : evidence;
        row["quality_evidence_level"] = evidence.empty() ? "untested" : "quality-tested";
        row["quality_gate_required"] = required;
        row["quality_gate_passed"] = passed;
        if (required && !passed) row["fit_eligible"] = false;
        if (!passed) {
            row["quality_gate_reason"] = samples < min_samples
                ? "Quality evidence is missing or has too few samples for this artifact and pack."
                : "Measured task quality is below the configured floor.";
        } else {
            row["quality_gate_reason"] = "Measured task quality satisfies the configured floor.";
        }
    }
    return rows;
}

} // namespace quality_evidence
