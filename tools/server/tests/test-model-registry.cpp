#include "model-registry.h"

#include <algorithm>
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
    const std::vector<model_registry::configured_model> configured = {
        {"duplicate-base", (root / "duplicate-a" / "same.gguf").string(), "preset", "unloaded"},
        {"duplicate-base", (root / "duplicate-a" / "same.gguf").string(), "preset", "unloaded"},
        {"duplicate-lora", (root / "duplicate-a" / "same.gguf").string(), "preset", "unloaded"},
        {"shard-a", (root / "sharded" / "model-Q4_K_M-00001-of-00003.gguf").string(), "preset", "unloaded"},
        {"shard-b", (root / "sharded" / "model-Q4_K_M-00003-of-00003.gguf").string(), "preset", "unloaded"},
    };
    const json first = registry_index.scan({root}, configured);
    require(first.value("artifact_count", 0) == 5, "auxiliary GGUF is not a standalone model artifact");
    require(find_health(first, "corrupt") != nullptr, "corrupt file health");
    require(find_health(first, "partial") != nullptr, "aria2 sidecar health");
    const json * missing = find_health(first, "missing-shards");
    require(missing != nullptr, "missing shard health");
    require(missing && missing->at("missing_shards") == json::array({2}), "missing shard index");
    require(first.at("duplicates").size() == 1, "exact duplicate locations detected");
    const auto configured_artifact = std::find_if(first.at("artifacts").begin(), first.at("artifacts").end(), [](const json & artifact) {
        return artifact.value("primary_path", std::string()).find("duplicate-a/same.gguf") != std::string::npos;
    });
    require(configured_artifact != first.at("artifacts").end(), "configured artifact found");
    require(configured_artifact != first.at("artifacts").end() &&
            configured_artifact->at("configured_ids") == json::array({"duplicate-base", "duplicate-lora"}),
            "all presets sharing one artifact are retained and deduplicated");
    require(missing && missing->at("configured_ids") == json::array({"shard-a", "shard-b"}),
            "presets pointing to separate shards share one artifact identity");

    require(model_registry::is_media_generation_artifact({
                {"primary_path", (root / "models" / "media" / "video" / "encoder.gguf").string()},
                {"metadata", json::object()},
            }),
            "media directory is excluded from text-generation benchmarking");
    require(model_registry::is_media_generation_artifact({
                {"primary_path", (root / "models" / "encoder.gguf").string()},
                {"metadata", {{"gguf_architecture", "t5encoder"}}},
            }),
            "media GGUF architecture is excluded from text-generation benchmarking");
    require(!model_registry::is_media_generation_artifact({
                {"primary_path", (root / "models" / "qwen.gguf").string()},
                {"name", "Qwen chat"},
                {"metadata", {{"gguf_architecture", "qwen35"}}},
            }),
            "text-generation GGUF remains benchmark eligible");

    const std::vector<model_registry::configured_model> reordered = {
        configured[4], configured[2], configured[0], configured[3], configured[1],
    };
    const json second = registry_index.scan({root}, reordered);
    require(second.value("cache_hit", false), "configured model order does not invalidate registry cache");

    const std::vector<model_registry::configured_model> reduced = {
        configured[0], configured[3], configured[4],
    };
    const json third = registry_index.scan({root}, reduced);
    require(!third.value("cache_hit", false), "configured identity changes invalidate registry cache");
    const auto reduced_artifact = std::find_if(third.at("artifacts").begin(), third.at("artifacts").end(), [](const json & artifact) {
        return artifact.value("primary_path", std::string()).find("duplicate-a/same.gguf") != std::string::npos;
    });
    require(reduced_artifact != third.at("artifacts").end() &&
            reduced_artifact->at("configured_ids") == json::array({"duplicate-base"}),
            "removed preset does not survive cache invalidation");
    std::filesystem::remove_all(root);

    if (failures != 0) {
        std::cerr << failures << " model registry test(s) failed\n";
        return 1;
    }
    std::cout << "model registry tests passed\n";
    return 0;
}
