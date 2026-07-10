#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace streaming_profiler {

using json = nlohmann::ordered_json;

json parse_sse_trace(const std::string & body);
double percentile_ms(std::vector<double> values, double percentile);
json encode_timeline(const std::vector<json> & samples);

json profile(
    const json & item,
    const json & cfg,
    const std::filesystem::path & llama_server_executable,
    const std::function<bool()> & cancelled = {});

} // namespace streaming_profiler
