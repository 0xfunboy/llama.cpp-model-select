#include "model-registry.h"

#include "caliber-plan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace model_registry {
namespace {

struct shard_name {
    bool sharded = false;
    std::string prefix;
    int index = 1;
    int total = 1;
};

struct file_entry {
    std::filesystem::path path;
    uintmax_t size = 0;
    int64_t mtime = 0;
    bool partial = false;
    bool auxiliary = false;
    shard_name shard;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return value;
}

bool ends_with_ci(const std::string & value, const std::string & suffix) {
    if (value.size() < suffix.size()) return false;
    return lower(value.substr(value.size() - suffix.size())) == lower(suffix);
}

std::string path_key(const std::filesystem::path & path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) normalized = std::filesystem::absolute(path, ec);
    return lower((ec ? path : normalized).lexically_normal().string());
}

shard_name parse_shard(const std::filesystem::path & path) {
    static const std::regex pattern(R"((.*)-([0-9]{5})-of-([0-9]{5})\.gguf$)", std::regex_constants::icase);
    std::smatch match;
    const std::string filename = path.filename().string();
    if (!std::regex_match(filename, match, pattern)) return {};
    return {true, match[1].str(), std::stoi(match[2].str()), std::stoi(match[3].str())};
}

bool is_auxiliary(const std::filesystem::path & path) {
    const std::string name = lower(path.filename().string());
    return name.find("mmproj") != std::string::npos ||
           name.find("projector") != std::string::npos ||
           name.find("mmvec") != std::string::npos ||
           name.find("tokenizer") != std::string::npos;
}

bool has_gguf_magic(const std::filesystem::path & path) {
    std::array<char, 4> magic{};
    std::ifstream in(path, std::ios::binary);
    in.read(magic.data(), magic.size());
    return in.gcount() == (std::streamsize) magic.size() && std::string(magic.data(), magic.size()) == "GGUF";
}

uint64_t fnv_update(uint64_t hash, const char * data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash ^= (unsigned char) data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t fnv_text(uint64_t hash, const std::string & value) {
    return fnv_update(hash, value.data(), value.size());
}

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

uint64_t sampled_file_hash(uint64_t hash, const file_entry & file) {
    hash = fnv_text(hash, std::to_string(file.size));
    std::ifstream in(file.path, std::ios::binary);
    if (!in) return fnv_text(hash, "unreadable");
    constexpr size_t sample_size = 64 * 1024;
    std::array<char, sample_size> buffer{};
    in.read(buffer.data(), buffer.size());
    hash = fnv_update(hash, buffer.data(), (size_t) in.gcount());
    if (file.size > sample_size) {
        in.clear();
        in.seekg((std::streamoff) (file.size - sample_size));
        in.read(buffer.data(), buffer.size());
        hash = fnv_update(hash, buffer.data(), (size_t) in.gcount());
    }
    return hash;
}

uint64_t full_file_hash(uint64_t hash, const std::vector<file_entry> & files) {
    std::array<char, 1024 * 1024> buffer{};
    for (const auto & file : files) {
        std::ifstream in(file.path, std::ios::binary);
        while (in) {
            in.read(buffer.data(), buffer.size());
            hash = fnv_update(hash, buffer.data(), (size_t) in.gcount());
        }
    }
    return hash;
}

std::string normalized_id(std::string value) {
    std::string out;
    bool dash = false;
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            out.push_back((char) std::tolower(c));
            dash = false;
        } else if (!out.empty() && !dash) {
            out.push_back('-');
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "unknown" : out;
}

std::string provenance_from_path(const std::filesystem::path & path) {
    const std::string value = path.generic_string();
    const auto pos = value.find("models--");
    if (pos == std::string::npos) return "";
    const auto end = value.find("/snapshots/", pos);
    std::string repo = value.substr(pos + 8, end == std::string::npos ? std::string::npos : end - pos - 8);
    const auto separator = repo.find("--");
    if (separator != std::string::npos) repo.replace(separator, 2, "/");
    return repo;
}

std::string scan_signature(const std::vector<file_entry> & files, const std::vector<configured_model> & configured) {
    uint64_t hash = 1469598103934665603ULL;
    for (const auto & file : files) {
        hash = fnv_text(hash, path_key(file.path));
        hash = fnv_text(hash, std::to_string(file.size));
        hash = fnv_text(hash, std::to_string(file.mtime));
        hash = fnv_text(hash, file.partial ? "partial" : "complete");
    }
    for (const auto & model : configured) {
        hash = fnv_text(hash, model.id + path_key(model.path) + model.status);
    }
    return hex64(hash);
}

std::vector<file_entry> discover_files(const std::vector<std::filesystem::path> & roots) {
    std::map<std::string, file_entry> unique;
    for (const auto & root : roots) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::exists(root, ec)) continue;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            auto path = it->path();
            bool partial = false;
            if (ends_with_ci(path.filename().string(), ".gguf.aria2")) {
                path = path.string().substr(0, path.string().size() - 6);
                partial = true;
            } else if (!ends_with_ci(path.filename().string(), ".gguf")) {
                continue;
            }
            const std::string key = path_key(path);
            auto & entry = unique[key];
            entry.path = path;
            entry.partial = entry.partial || partial;
            if (!partial) {
                entry.size = it->file_size(ec);
                entry.mtime = (int64_t) it->last_write_time(ec).time_since_epoch().count();
            }
            entry.auxiliary = is_auxiliary(path);
            entry.shard = parse_shard(path);
        }
    }
    std::vector<file_entry> files;
    for (auto & [_, file] : unique) files.push_back(std::move(file));
    std::sort(files.begin(), files.end(), [](const file_entry & a, const file_entry & b) { return path_key(a.path) < path_key(b.path); });
    return files;
}

} // namespace

json index::scan(const std::vector<std::filesystem::path> & roots, const std::vector<configured_model> & configured, bool refresh) {
    const auto discovered = discover_files(roots);
    const std::string signature = scan_signature(discovered, configured);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!refresh && !cache_.is_null() && signature == cache_signature_) {
        json cached = cache_;
        cached["cache_hit"] = true;
        return cached;
    }

    std::map<std::string, std::vector<file_entry>> groups;
    std::map<std::string, std::vector<file_entry>> auxiliaries;
    for (const auto & file : discovered) {
        const std::string directory = path_key(file.path.parent_path());
        if (file.auxiliary) {
            auxiliaries[directory].push_back(file);
            continue;
        }
        const std::string group = file.shard.sharded
            ? directory + "/" + lower(file.shard.prefix)
            : path_key(file.path);
        groups[group].push_back(file);
    }

    std::map<std::string, configured_model> configured_by_path;
    for (const auto & model : configured) configured_by_path[path_key(model.path)] = model;
    json artifacts = json::array();
    std::map<std::string, std::vector<size_t>> sampled_groups;
    for (auto & [_, files] : groups) {
        std::sort(files.begin(), files.end(), [](const file_entry & a, const file_entry & b) { return a.shard.index < b.shard.index; });
        int expected = 1;
        bool inconsistent = false;
        bool partial = false;
        uintmax_t total_size = 0;
        std::set<int> present;
        for (const auto & file : files) {
            if (file.shard.sharded) {
                if (expected != 1 && expected != file.shard.total) inconsistent = true;
                expected = std::max(expected, file.shard.total);
                present.insert(file.shard.index);
            } else {
                present.insert(1);
            }
            partial = partial || file.partial;
            total_size += file.size;
        }
        json missing = json::array();
        for (int i = 1; i <= expected; ++i) if (!present.count(i)) missing.push_back(i);
        const bool complete = !partial && !inconsistent && missing.empty() && (int) files.size() == expected;

        uint64_t sample = 1469598103934665603ULL;
        for (const auto & file : files) sample = sampled_file_hash(sample, file);
        const std::string sampled = hex64(sample);
        json meta = json::object();
        bool readable = false;
        if (complete && !files.empty()) {
            meta = caliber::read_gguf_plan_meta(files.front().path.string());
            readable = meta.value("gguf_readable", false);
        }
        const std::string model_name = meta.value("model", files.front().shard.sharded ? files.front().shard.prefix : files.front().path.stem().string());
        const std::string variant = meta.value("variant", std::string("unknown"));
        meta["size_mib"] = std::round((double) total_size / 1024.0 / 1024.0);
        meta["shard_count"] = expected;
        const std::string model_id = normalized_id(model_name);
        const std::string artifact_id = "gguf-" + sampled;
        const bool supported = readable && meta.contains("gguf_architecture") && meta["gguf_architecture"].is_string();
        std::string health = partial ? "partial" : (!missing.empty() ? "missing-shards" :
            (inconsistent ? "inconsistent-shards" : (!readable ? (has_gguf_magic(files.front().path) ? "unsupported" : "corrupt") :
            (!supported ? "unsupported" : "ready"))));

        json paths = json::array();
        json shard_rows = json::array();
        bool is_configured = false;
        json configured_ids = json::array();
        for (const auto & file : files) {
            paths.push_back(file.path.string());
            shard_rows.push_back({
                {"index", file.shard.index},
                {"total", file.shard.total},
                {"path", file.path.string()},
                {"size_bytes", file.size},
                {"partial", file.partial},
            });
            const auto configured_it = configured_by_path.find(path_key(file.path));
            if (configured_it != configured_by_path.end()) {
                is_configured = true;
                configured_ids.push_back(configured_it->second.id);
            }
        }
        json mmproj = nullptr;
        const auto auxiliary_it = auxiliaries.find(path_key(files.front().path.parent_path()));
        if (auxiliary_it != auxiliaries.end() && !auxiliary_it->second.empty()) mmproj = auxiliary_it->second.front().path.string();
        const std::string provenance = provenance_from_path(files.front().path);
        const std::string location_id = "location-" + hex64(fnv_text(1469598103934665603ULL, path_key(files.front().path)));
        json artifact = {
            {"artifact_id", artifact_id},
            {"location_id", location_id},
            {"model_id", model_id},
            {"preset_id", artifact_id + ":" + normalized_id(variant)},
            {"name", model_name},
            {"variant", variant},
            {"primary_path", files.front().path.string()},
            {"paths", paths},
            {"shards", shard_rows},
            {"expected_shards", expected},
            {"missing_shards", missing},
            {"size_bytes", total_size},
            {"size_mib", std::round((double) total_size / 1024.0 / 1024.0)},
            {"health", health},
            {"loadable", health == "ready"},
            {"configured", is_configured},
            {"configured_ids", configured_ids},
            {"mmproj_path", mmproj},
            {"hf_repo", provenance.empty() ? json(nullptr) : json(provenance)},
            {"fingerprint", sampled},
            {"fingerprint_kind", "sampled-content-v1"},
            {"metadata", meta},
            {"duplicate_of", nullptr},
            {"redundant_quantization", false},
        };
        sampled_groups[std::to_string(total_size) + ":" + sampled].push_back(artifacts.size());
        artifacts.push_back(std::move(artifact));
    }

    json duplicate_sets = json::array();
    for (const auto & [_, indexes] : sampled_groups) {
        if (indexes.size() < 2) continue;
        std::map<std::string, std::vector<size_t>> exact;
        for (size_t artifact_index : indexes) {
            std::vector<file_entry> files;
            for (const auto & path : artifacts[artifact_index]["paths"]) {
                const auto it = std::find_if(discovered.begin(), discovered.end(), [&](const file_entry & file) {
                    return path_key(file.path) == path_key(path.get<std::string>());
                });
                if (it != discovered.end()) files.push_back(*it);
            }
            exact[hex64(full_file_hash(1469598103934665603ULL, files))].push_back(artifact_index);
        }
        for (const auto & [full_hash, exact_indexes] : exact) {
            if (exact_indexes.size() < 2) continue;
            json duplicate_ids = json::array();
            const std::string canonical = artifacts[exact_indexes.front()]["location_id"].get<std::string>();
            for (size_t i = 0; i < exact_indexes.size(); ++i) {
                auto & artifact = artifacts[exact_indexes[i]];
                artifact["fingerprint"] = full_hash;
                artifact["fingerprint_kind"] = "full-content-fnv1a-v1";
                if (i > 0) artifact["duplicate_of"] = canonical;
                duplicate_ids.push_back(artifact["location_id"]);
            }
            duplicate_sets.push_back({{"fingerprint", full_hash}, {"artifacts", duplicate_ids}});
        }
    }

    std::map<std::string, std::vector<size_t>> by_model;
    for (size_t i = 0; i < artifacts.size(); ++i) by_model[artifacts[i]["model_id"].get<std::string>()].push_back(i);
    json redundant_sets = json::array();
    for (const auto & [model_id, indexes] : by_model) {
        std::set<std::string> variants;
        for (size_t i : indexes) variants.insert(artifacts[i]["variant"].get<std::string>());
        if (indexes.size() < 2 || variants.size() < 2) continue;
        json ids = json::array();
        for (size_t i : indexes) {
            artifacts[i]["redundant_quantization"] = true;
            ids.push_back(artifacts[i]["artifact_id"]);
        }
        redundant_sets.push_back({{"model_id", model_id}, {"artifacts", ids}, {"variants", variants}});
    }

    json root_rows = json::array();
    for (const auto & root : roots) root_rows.push_back(root.string());
    cache_ = {
        {"object", "model-registry"},
        {"schema_version", 1},
        {"cache_hit", false},
        {"signature", signature},
        {"roots", root_rows},
        {"artifacts", artifacts},
        {"artifact_count", artifacts.size()},
        {"duplicates", duplicate_sets},
        {"redundant_quantizations", redundant_sets},
    };
    cache_signature_ = signature;
    return cache_;
}

std::string artifact_id_for_path(const json & registry, const std::string & path) {
    if (!registry.contains("artifacts") || !registry["artifacts"].is_array()) return "";
    const std::string needle = path_key(path);
    for (const auto & artifact : registry["artifacts"]) {
        if (!artifact.contains("paths") || !artifact["paths"].is_array()) continue;
        for (const auto & candidate : artifact["paths"]) {
            if (candidate.is_string() && path_key(candidate.get<std::string>()) == needle) {
                return artifact.value("artifact_id", std::string());
            }
        }
    }
    return "";
}

} // namespace model_registry
