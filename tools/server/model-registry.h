#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace model_registry {

using json = nlohmann::ordered_json;

struct configured_model {
    std::string id;
    std::string path;
    std::string source;
    std::string status;
};

class index {
  public:
    json scan(
        const std::vector<std::filesystem::path> & roots,
        const std::vector<configured_model> & configured = {},
        bool refresh = false);

  private:
    std::mutex mutex_;
    std::string cache_signature_;
    json cache_ = nullptr;
};

std::string artifact_id_for_path(const json & registry, const std::string & path);

} // namespace model_registry
