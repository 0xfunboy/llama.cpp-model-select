#include "model-registry.h"

#include <filesystem>
#include <fstream>
#include <iostream>

using model_registry::json;

namespace {

int failures = 0;

void require(bool condition, const std::string & message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void write_file(const std::filesystem::path & path, const std::string & value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << value;
}

const json * find_health(const json & registry, const std::string & health) {
    for (const auto & artifact : registry.at("artifacts")) {
        if (artifact.value("health", std::string()) == health) return &artifact;
    }
    return nullptr;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "llama-model-registry-test";
    std::filesystem::remove_all(root);
    write_file(root / "corrupt.gguf", "not-a-gguf");
    write_file(root / "partial.gguf", "partial-download");
    write_file(root / "partial.gguf.aria2", "state");
    write_file(root / "sharded" / "model-Q4_K_M-00001-of-00003.gguf", "one");
    write_file(root / "sharded" / "model-Q4_K_M-00003-of-00003.gguf", "three");
    write_file(root / "duplicate-a" / "same.gguf", "identical-content");
    write_file(root / "duplicate-b" / "renamed.gguf", "identical-content");
    write_file(root / "duplicate-a" / "mmproj-model.gguf", "projector");

    model_registry::index registry_index;
    const json first = registry_index.scan({root});
    require(first.value("artifact_count", 0) == 5, "auxiliary GGUF is not a standalone model artifact");
    require(find_health(first, "corrupt") != nullptr, "corrupt file health");
    require(find_health(first, "partial") != nullptr, "aria2 sidecar health");
    const json * missing = find_health(first, "missing-shards");
    require(missing != nullptr, "missing shard health");
    require(missing && missing->at("missing_shards") == json::array({2}), "missing shard index");
    require(first.at("duplicates").size() == 1, "exact duplicate locations detected");

    const json second = registry_index.scan({root});
    require(second.value("cache_hit", false), "unchanged directory uses registry cache");
    std::filesystem::remove_all(root);

    if (failures != 0) {
        std::cerr << failures << " model registry test(s) failed\n";
        return 1;
    }
    std::cout << "model registry tests passed\n";
    return 0;
}
