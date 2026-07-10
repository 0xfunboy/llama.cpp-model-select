#include "quality-evidence.h"

#include <iostream>

static void require(bool value, const char * message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using quality_evidence::json;
    const std::vector<json> reports = {{
        {"id", "q1"}, {"kind", "eval"}, {"status", "completed"}, {"created_at", "2026-01-01"},
        {"evaluation_profile", {{"template", "chatml"}, {"temperature", 0.0}}},
        {"results", {
            {{"artifact_id", "artifact:a"}, {"model", "alias-a"}, {"id", "c1"}, {"pack", "coding"}, {"pass", true}},
            {{"artifact_id", "artifact:a"}, {"model", "alias-a"}, {"id", "c2"}, {"pack", "coding"}, {"pass", false}},
            {{"model", "alias-b"}, {"id", "c1"}, {"pack", "coding"}, {"pass", false}},
        }},
    }};
    const json registry = {{"artifacts", {{{"artifact_id", "artifact:b"}, {"configured_ids", {"alias-b"}}}}}};
    const auto profiles = quality_evidence::build_profiles(reports, registry);
    require(profiles.at("artifact:a").at("packs").at("coding").at("score") == 0.5, "profile score");
    require(profiles.contains("artifact:b"), "legacy configured model normalized through registry");
    const std::vector<json> rows = {
        {{"id", "fast-bad"}, {"artifact_id", "artifact:b"}, {"fit_eligible", true}},
        {{"id", "good"}, {"artifact_id", "artifact:a"}, {"fit_eligible", true}},
        {{"id", "unknown"}, {"artifact_id", "artifact:c"}, {"fit_eligible", true}},
    };
    const auto gated = quality_evidence::apply_policy(rows, profiles, {{"required", true}, {"pack", "coding"}, {"min_score", 0.5}, {"min_samples", 2}});
    require(!gated[0].at("quality_gate_passed").get<bool>(), "bad model must fail");
    require(!gated[0].at("fit_eligible").get<bool>(), "failed quality gate must disable FIT");
    require(gated[1].at("quality_gate_passed").get<bool>(), "quality floor must pass");
    require(!gated[2].at("quality_gate_passed").get<bool>(), "missing evidence must fail closed");
    std::cout << "quality evidence tests passed\n";
    return 0;
}
