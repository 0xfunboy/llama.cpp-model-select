#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace least_cost_router {

using json = nlohmann::ordered_json;

bool is_virtual_alias(const std::string & model);
json aliases();
json select(
    const std::string & alias,
    const json & request,
    const std::vector<json> & candidates,
    const std::vector<json> & caliber_reports,
    const json & recent_events,
    int64_t now_ms);

} // namespace least_cost_router
