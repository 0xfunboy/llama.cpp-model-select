#pragma once

#include <nlohmann/json.hpp>

#include <vector>

namespace quality_evidence {

using json = nlohmann::ordered_json;

json build_profiles(const std::vector<json> & ds4_reports, const json & registry = json::object());
std::vector<json> apply_policy(const std::vector<json> & rows, const json & profiles, const json & policy);

} // namespace quality_evidence
