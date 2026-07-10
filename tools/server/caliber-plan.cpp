#include "caliber-plan.h"

#include "gguf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace caliber {
namespace {

static constexpr double MIB = 1024.0 * 1024.0;

bool has_key(const json & obj, const char * key) {
    return obj.is_object() && obj.contains(key) && !obj.at(key).is_null();
}

double num_value(const json & obj, const char * key, double fallback = 0.0) {
    if (!has_key(obj, key) || !obj.at(key).is_number()) return fallback;
    const double v = obj.at(key).get<double>();
    return std::isfinite(v) ? v : fallback;
}

int64_t as_int(const json & value, int64_t fallback = 0) {
    if (value.is_number()) {
        const double v = value.get<double>();
        if (std::isfinite(v)) return (int64_t) std::trunc(v);
    }
    return fallback;
}

int64_t int_value(const json & obj, const char * key, int64_t fallback = 0) {
    return has_key(obj, key) ? as_int(obj.at(key), fallback) : fallback;
}

std::string str_value(const json & obj, const char * key, const std::string & fallback = "") {
    if (!has_key(obj, key)) return fallback;
    if (obj.at(key).is_string()) return obj.at(key).get<std::string>();
    if (obj.at(key).is_number() || obj.at(key).is_boolean()) return obj.at(key).dump();
    return fallback;
}

bool bool_value(const json & obj, const char * key, bool fallback = false) {
    return has_key(obj, key) && obj.at(key).is_boolean() ? obj.at(key).get<bool>() : fallback;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string base_name(const std::string & path) {
    return std::filesystem::path(path).filename().string();
}

std::string stem_name(const std::string & path) {
    return std::filesystem::path(path).stem().string();
}

std::string sanitize(std::string s, size_t max_len) {
    for (char & c : s) {
        if (!(std::isalnum((unsigned char) c) || c == '_')) c = '_';
    }
    if (s.size() > max_len) s.resize(max_len);
    return s;
}

bool has_flag(const std::string & args, const std::string & flag) {
    std::istringstream in(args);
    std::string token;
    while (in >> token) if (token == flag) return true;
    return false;
}

std::string remove_flag(const std::string & args, const std::string & flag) {
    std::istringstream in(args);
    std::string token;
    std::vector<std::string> out;
    while (in >> token) if (token != flag) out.push_back(token);
    std::ostringstream joined;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) joined << ' ';
        joined << out[i];
    }
    return joined.str();
}

int clamp_layer(int value, int block_count) {
    return std::max(0, std::min(std::max(0, block_count), value));
}

std::vector<int> ints_from_array(const json & obj, const char * key) {
    std::vector<int> out;
    if (!has_key(obj, key) || !obj.at(key).is_array()) return out;
    for (const auto & value : obj.at(key)) out.push_back((int) as_int(value));
    return out;
}

std::vector<double> doubles_from_array(const json & obj, const char * key) {
    std::vector<double> out;
    if (!has_key(obj, key) || !obj.at(key).is_array()) return out;
    for (const auto & value : obj.at(key)) if (value.is_number()) out.push_back(value.get<double>());
    return out;
}

std::vector<json> array_from(const json & obj, const char * key) {
    std::vector<json> out;
    if (!has_key(obj, key) || !obj.at(key).is_array()) return out;
    for (const auto & value : obj.at(key)) out.push_back(value);
    return out;
}

int positive_int_from(const json & value) {
    return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() > 0 ? (int) std::trunc(value.get<double>()) : 0;
}

int block_count_from(const json & meta) {
    int declared = int_value(meta, "gguf_block_count", 0);
    int highest = 0;
    if (has_key(meta, "gguf_block_tensor_bytes") && meta.at("gguf_block_tensor_bytes").is_array()) {
        for (const auto & entry : meta.at("gguf_block_tensor_bytes")) {
            highest = std::max(highest, positive_int_from(json(num_value(entry, "block", -1) + 1)));
        }
    }
    return std::max(declared, highest);
}

double finite_non_negative(const json & obj, const char * key, double fallback = std::numeric_limits<double>::quiet_NaN()) {
    if (!has_key(obj, key) || !obj.at(key).is_number()) return fallback;
    const double v = obj.at(key).get<double>();
    return std::isfinite(v) && v >= 0 ? v : fallback;
}

double fallback_block_bytes(const json & meta, int block_count, const std::vector<double> & known, double global_bytes) {
    if (!known.empty()) {
        double sum = 0.0;
        for (double value : known) sum += value;
        return sum / known.size();
    }
    const double tensor_bytes = finite_non_negative(meta, "gguf_tensor_bytes");
    if (std::isfinite(tensor_bytes) && tensor_bytes > global_bytes) return (tensor_bytes - global_bytes) / block_count;
    const double file_bytes = std::max(0.0, finite_non_negative(meta, "size_mib", 0.0)) * MIB;
    return std::max(0.0, file_bytes - global_bytes) / block_count;
}

int actual_layer(const json & probe, int block_count) {
    const double offloaded = finite_non_negative(probe, "offloaded_layers");
    const int value = std::isfinite(offloaded) ? (int) std::trunc(offloaded) : int_value(probe, "requested_layers", -1);
    return value >= 0 ? clamp_layer(value, block_count) : -1;
}

struct probe_point {
    json probe;
    int layer;
    double vram;
};

std::vector<probe_point> unique_valid_probes(const std::vector<json> & probes, int block_count) {
    std::map<int, probe_point> by_layer;
    for (const auto & probe : probes) {
        const int layer = actual_layer(probe, block_count);
        const double vram = finite_non_negative(probe, "vram_ready_mib");
        if (!bool_value(probe, "ready") || layer < 0 || !std::isfinite(vram)) continue;
        by_layer[layer] = {probe, layer, vram};
    }
    std::vector<probe_point> out;
    for (const auto & [_, point] : by_layer) out.push_back(point);
    return out;
}

json linear_fit(const std::vector<probe_point> & points) {
    if (points.size() < 2) return nullptr;
    double mean_x = 0.0, mean_y = 0.0;
    for (const auto & point : points) {
        mean_x += point.layer;
        mean_y += point.vram;
    }
    mean_x /= points.size();
    mean_y /= points.size();
    double denominator = 0.0, numerator = 0.0;
    for (const auto & point : points) {
        denominator += (point.layer - mean_x) * (point.layer - mean_x);
        numerator += (point.layer - mean_x) * (point.vram - mean_y);
    }
    if (denominator <= 0) return nullptr;
    const double slope = numerator / denominator;
    if (!std::isfinite(slope) || slope <= 0) return nullptr;
    return {{"slope", slope}, {"intercept", mean_y - slope * mean_x}};
}

json first_untested(const std::vector<int> & candidates, const std::set<int> & tested) {
    for (int candidate : candidates) if (!tested.count(candidate)) return candidate;
    return nullptr;
}

json model_base_args(const json & meta, const std::string & base, int64_t ram_available, int64_t vram_driver_usable) {
    const int64_t nommap_required = int_value(meta, "size_mib") + int_value(meta, "mmproj_mib");
    const int64_t plausible = std::max<int64_t>(0, ram_available) + std::max<int64_t>(0, vram_driver_usable);
    if (!has_flag(base, "--no-mmap")) {
        return {{"baseArgs", trim(base)}, {"mode", "mmap-chosen"}, {"reason", "base_args_allow_mmap"}, {"nommapRequiredMib", nommap_required}, {"ramAvailableBeforeLoadMib", ram_available}};
    }
    if (nommap_required > 0 && plausible > 0 && nommap_required > plausible) {
        return {{"baseArgs", remove_flag(base, "--no-mmap")}, {"mode", "mmap-chosen"}, {"reason", "model_size_exceeds_plausible_ram_vram_before_load"}, {"nommapRequiredMib", nommap_required}, {"ramAvailableBeforeLoadMib", ram_available}};
    }
    return {{"baseArgs", trim(base)}, {"mode", "no-mmap"}, {"reason", "model_size_within_plausible_ram_vram_before_load"}, {"nommapRequiredMib", nommap_required}, {"ramAvailableBeforeLoadMib", ram_available}};
}

double effective_vram_mib(const json & hardware, const json & planning) {
    const double per_gpu_reserve = std::max(0.0, num_value(planning, "per_gpu_headroom_mib", 512.0));
    double topology_total = 0.0;
    if (has_key(hardware, "gpus") && hardware.at("gpus").is_array()) {
        for (const auto & gpu : hardware.at("gpus")) {
            const double usable = num_value(gpu, "vram_driver_usable_mib", num_value(gpu, "vram_total_mib", 0.0));
            topology_total += std::max(0.0, usable - per_gpu_reserve);
        }
    }
    double available = topology_total > 0 ? topology_total : num_value(hardware, "vram_driver_usable_mib", num_value(hardware, "vram_budget_mib", 0.0));
    const std::string backend = lower(str_value(hardware, "backend", "generic"));
    if (bool_value(hardware, "unified_memory") || backend == "metal") {
        const double ram = num_value(hardware, "system_ram_available_mib", available);
        if (ram > 0) available = available > 0 ? std::min(available, ram) : ram;
    }
    return std::max(0.0, available);
}

std::string planner_adapter(const json & hardware) {
    const std::string backend = lower(str_value(hardware, "backend", "generic"));
    if (backend.find("cuda") != std::string::npos) return "cuda-topology";
    if (backend.find("metal") != std::string::npos || bool_value(hardware, "unified_memory")) return "metal-unified";
    if (backend.find("hip") != std::string::npos) return "hip-topology";
    if (backend.find("vulkan") != std::string::npos) return "vulkan-topology";
    return num_value(hardware, "vram_budget_mib", 0.0) > 0 ? "generic-gpu" : "cpu-numa";
}

std::vector<int> closest_candidates(std::vector<int> candidates, int anchor, size_t limit) {
    std::sort(candidates.begin(), candidates.end(), [anchor](int a, int b) {
        const int da = std::abs(a - anchor);
        const int db = std::abs(b - anchor);
        return da == db ? a < b : da < db;
    });
    if (candidates.size() > limit) candidates.resize(limit);
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

void annotate_adaptive_item(json & item, const json & estimate, const std::string & adapter, int anchor, size_t ordinal) {
    item["planning_mode"] = "adaptive-structural";
    item["planner_adapter"] = adapter;
    item["predicted_frontier"] = anchor;
    item["search_stage"] = ordinal < 3 ? "race" : "boundary-confirmation";
    item["structural_estimate"] = estimate;
}

std::string get_sweep_kind(const json & meta, const json & cfg) {
    if (bool_value(meta, "is_moe")) return "moe-cpu";
    const json hardware = has_key(cfg, "hardware") ? cfg.at("hardware") : json::object();
    const json planning = has_key(cfg, "planning") ? cfg.at("planning") : json::object();
    const int64_t budget = (int64_t) effective_vram_mib(hardware, planning);
    const int64_t needed = int_value(meta, "size_mib") + int_value(meta, "mmproj_mib") + int_value(planning, "overhead_mib");
    return needed < budget ? "context" : "offload";
}

std::map<std::string, std::string> catalog_level_map(const std::vector<json> & catalog, const json & presets) {
    std::map<std::string, std::string> by_id;
    for (const auto & entry : catalog) {
        if (has_key(entry, "id") && has_key(entry, "hf_file")) by_id[str_value(entry, "id")] = str_value(entry, "hf_file");
    }
    std::map<std::string, std::string> out;
    for (const std::string level : {"low", "middle", "high", "ultra"}) {
        if (!has_key(presets, level.c_str())) continue;
        const auto & preset = presets.at(level);
        if (!has_key(preset, "models") || !preset.at("models").is_array()) continue;
        for (const auto & id : preset.at("models")) {
            const auto it = by_id.find(id.get<std::string>());
            if (it != by_id.end()) out[lower(it->second)] = level;
        }
    }
    return out;
}

std::map<std::string, int64_t> catalog_context_map(const std::vector<json> & catalog) {
    std::map<std::string, int64_t> out;
    for (const auto & entry : catalog) {
        if (has_key(entry, "hf_file") && int_value(entry, "max_context") > 0) out[lower(str_value(entry, "hf_file"))] = int_value(entry, "max_context");
    }
    return out;
}

bool ctx_allowed(int64_t ctx, int64_t global_cap, int64_t per_model_cap) {
    if (global_cap > 0 && ctx > global_cap) return false;
    if (per_model_cap > 0 && ctx > per_model_cap) return false;
    return true;
}

int64_t choose_sweep_context(const std::vector<json> & candidates, int64_t global_cap, int64_t per_model_cap, int64_t fallback) {
    int64_t best = 0;
    for (const auto & candidate : candidates) {
        const int64_t ctx = int_value(candidate, "ctx");
        if (ctx > 0 && ctx_allowed(ctx, global_cap, per_model_cap)) best = std::max(best, ctx);
    }
    if (best <= 0) best = fallback;
    if (global_cap > 0) best = std::min(best, global_cap);
    if (per_model_cap > 0) best = std::min(best, per_model_cap);
    return std::max<int64_t>(1024, best);
}

std::vector<json> matched_vanilla_baselines(const json & meta, const std::string & sweep, const json & level, int64_t ctx, const std::string & kv_k, const std::string & kv_v, const std::string & target_id) {
    std::vector<json> rows = {
        {{"args", "--ctx-size " + std::to_string(ctx)}, {"label", "llama_cpp_matched_ctx=" + std::to_string(ctx) + "_default"}, {"dims", {"ctx"}}, {"reason", "raw llama.cpp constrained to the anchor candidate context"}},
        {{"args", "--ctx-size " + std::to_string(ctx) + " --parallel 1"}, {"label", "llama_cpp_matched_ctx=" + std::to_string(ctx) + "_parallel1"}, {"dims", {"ctx", "parallel"}}, {"reason", "anchor context plus single-slot parallelism matching the candidate profile"}},
        {{"args", "--ctx-size " + std::to_string(ctx) + " --parallel 1 --cache-type-k " + kv_k + " --cache-type-v " + kv_v}, {"label", kv_k == kv_v ? "llama_cpp_matched_ctx=" + std::to_string(ctx) + "_parallel1_kv=" + kv_k : "llama_cpp_matched_ctx=" + std::to_string(ctx) + "_parallel1_kvk=" + kv_k + "_kvv=" + kv_v}, {"dims", {"ctx", "parallel", "ctk", "ctv"}}, {"reason", "anchor context, single slot, and candidate KV cache profile"}},
    };
    std::vector<json> out;
    for (const auto & row : rows) {
        json item = new_plan_item(meta, sweep, level, str_value(row, "args"), str_value(row, "label"), json::object(), "vanilla-matched");
        item["fair_control_target_id"] = target_id;
        item["fair_control_matched_dims"] = row.at("dims");
        item["fair_control_reason"] = row.at("reason");
        item["row_role"] = infer_row_role({{"control_kind", "vanilla-matched"}, {"workload_kind", "baseline"}, {"fair_control_target_id", target_id}});
        out.push_back(item);
    }
    return out;
}

json gguf_scalar(const gguf_context * ctx, const std::vector<std::string> & keys) {
    for (const auto & key : keys) {
        const int64_t id = gguf_find_key(ctx, key.c_str());
        if (id < 0) continue;
        switch (gguf_get_kv_type(ctx, id)) {
            case GGUF_TYPE_UINT32: return gguf_get_val_u32(ctx, id);
            case GGUF_TYPE_INT32: return gguf_get_val_i32(ctx, id);
            case GGUF_TYPE_UINT64: return gguf_get_val_u64(ctx, id);
            case GGUF_TYPE_INT64: return gguf_get_val_i64(ctx, id);
            case GGUF_TYPE_STRING: return gguf_get_val_str(ctx, id);
            default: break;
        }
    }
    return nullptr;
}

} // namespace

json estimate_initial_offload(const json & meta, const json & options) {
    const int block_count = block_count_from(meta);
    if (block_count == 0) {
        return {{"source", "unavailable"}, {"blockCount", 0}, {"estimatedLayers", 0}, {"availableWeightBytes", 0}, {"estimatedGpuWeightBytes", 0}, {"globalTensorBytes", 0}, {"blockTensorBytes", json::array()}, {"fullModelFits", false}};
    }

    std::map<int, double> by_block;
    if (has_key(meta, "gguf_block_tensor_bytes") && meta.at("gguf_block_tensor_bytes").is_array()) {
        for (const auto & entry : meta.at("gguf_block_tensor_bytes")) {
            const int block = (int) int_value(entry, "block", -1);
            const double bytes = finite_non_negative(entry, "bytes");
            if (block >= 0 && block < block_count && std::isfinite(bytes)) by_block[block] = bytes;
        }
    }
    std::vector<double> known;
    for (const auto & [_, bytes] : by_block) if (bytes > 0) known.push_back(bytes);
    const double global_bytes = finite_non_negative(meta, "gguf_global_tensor_bytes", 0.0);
    const double fallback = fallback_block_bytes(meta, block_count, known, global_bytes);
    std::vector<double> block_bytes(block_count, fallback);
    for (const auto & [block, bytes] : by_block) block_bytes[block] = bytes;
    const bool complete = (int) by_block.size() == block_count && std::all_of(block_bytes.begin(), block_bytes.end(), [](double v) { return v > 0; });
    const std::string source = complete ? "tensor-directory" : (!by_block.empty() ? "tensor-directory-partial" : "uniform-file-size");

    const double available_mib = std::max(0.0, finite_non_negative(options, "availableMib", 0.0));
    const double runtime_reserve = std::max(0.0, finite_non_negative(options, "runtimeReserveMib", 0.0));
    const double mmproj_mib = std::max(0.0, finite_non_negative(options, "mmprojMib", 0.0));
    const double gross_bytes = std::max(0.0, (available_mib - runtime_reserve - mmproj_mib) * MIB);
    const double available_weight_bytes = std::max(0.0, gross_bytes - global_bytes);

    double selected = 0.0;
    int estimated = 0;
    for (int block = block_count - 1; block >= 0; --block) {
        const double next = std::max(0.0, block_bytes[block]);
        if (selected + next > available_weight_bytes) break;
        selected += next;
        ++estimated;
    }
    json out_blocks = json::array();
    for (double value : block_bytes) out_blocks.push_back(value);
    return {
        {"source", source}, {"blockCount", block_count}, {"estimatedLayers", estimated},
        {"availableWeightBytes", available_weight_bytes}, {"estimatedGpuWeightBytes", global_bytes + selected},
        {"globalTensorBytes", global_bytes}, {"blockTensorBytes", out_blocks}, {"fullModelFits", estimated == block_count},
    };
}

json estimate_offload_cliff(const json & options) {
    const int block_count = std::max(0, (int) int_value(options, "blockCount"));
    const int initial = clamp_layer((int) int_value(options, "initialEstimate"), block_count);
    const int max_probe_count = std::max(1, (int) int_value(options, "maxProbeCount", 4));
    const double budget = num_value(options, "vramBudgetCapMib", 0.0);
    const std::vector<json> probes = array_from(options, "probes");
    const auto valid = unique_valid_probes(probes, block_count);

    std::set<int> tested;
    std::vector<int> fit_layers;
    std::vector<int> spill_layers;
    for (const auto & probe : probes) {
        const int layer = actual_layer(probe, block_count);
        if (layer < 0) continue;
        tested.insert(layer);
        if (bool_value(probe, "ready") && bool_value(probe, "fit_under_vram_budget")) fit_layers.push_back(layer);
        else spill_layers.push_back(layer);
    }
    const json regression = linear_fit(valid);
    const json verified_fit = fit_layers.empty() ? json(nullptr) : json(*std::max_element(fit_layers.begin(), fit_layers.end()));
    const json first_spill = spill_layers.empty() ? json(nullptr) : json(*std::min_element(spill_layers.begin(), spill_layers.end()));
    const int predicted = regression.is_object()
        ? clamp_layer((int) std::floor((budget - regression.at("intercept").get<double>()) / regression.at("slope").get<double>()), block_count)
        : (verified_fit.is_number_integer() ? verified_fit.get<int>() : initial);
    const bool adjacent = verified_fit.is_number_integer() && first_spill.is_number_integer() && first_spill.get<int>() - verified_fit.get<int>() <= 1;
    const bool verified_full = verified_fit.is_number_integer() && verified_fit.get<int>() == block_count;
    const bool complete = adjacent || verified_full || (int) probes.size() >= max_probe_count;

    json next = nullptr;
    std::string reason = "probe initial structural estimate";
    if (!complete) {
        if (verified_fit.is_number_integer() && first_spill.is_number_integer()) {
            next = first_untested({clamp_layer((verified_fit.get<int>() + first_spill.get<int>()) / 2, block_count), clamp_layer(verified_fit.get<int>() + 1, block_count)}, tested);
            reason = "narrow verified fit/spill bracket";
        } else if (tested.empty()) {
            next = initial;
        } else if (valid.empty()) {
            const int failed = first_spill.is_number_integer() ? first_spill.get<int>() : initial;
            next = first_untested({clamp_layer(failed / 2, block_count), clamp_layer(failed - 1, block_count), 0}, tested);
            reason = "probe failed before stable allocation; search downward";
        } else if (valid.size() == 1) {
            const auto & point = valid.front();
            if (bool_value(point.probe, "fit_under_vram_budget")) {
                next = first_untested({clamp_layer((int) std::ceil((point.layer + block_count) / 2.0), block_count), clamp_layer(point.layer + 1, block_count), block_count}, tested);
                reason = "single fitting probe; search upward";
            } else {
                next = first_untested({clamp_layer(point.layer / 2, block_count), clamp_layer(point.layer - 1, block_count), 0}, tested);
                reason = "single spilling probe; search downward";
            }
        } else {
            const bool beaten = verified_fit.is_number_integer() && first_spill.is_null() && verified_fit.get<int>() >= predicted;
            if (beaten) next = first_untested({block_count, clamp_layer((int) std::ceil((verified_fit.get<int>() + block_count) / 2.0), block_count), clamp_layer(verified_fit.get<int>() + 1, block_count)}, tested);
            else next = first_untested({predicted, clamp_layer(predicted + 1, block_count), clamp_layer(predicted - 1, block_count), verified_fit.is_number_integer() ? block_count : 0}, tested);
            reason = beaten ? "observed fit exceeded prediction; probe upper boundary" : (regression.is_object() ? "validate linear cliff prediction" : "expand probe range");
        }
    }
    const std::string confidence = verified_full ? "verified-full" : (adjacent ? "bracketed" : (regression.is_object() ? "linear" : (valid.size() == 1 ? "single-probe" : "none")));
    return {
        {"predicted_fit_layers", predicted},
        {"verified_fit_layers", verified_fit},
        {"first_spill_layers", first_spill},
        {"slope_mib_per_layer", regression.is_object() ? regression.at("slope") : json(nullptr)},
        {"intercept_mib", regression.is_object() ? regression.at("intercept") : json(nullptr)},
        {"confidence", confidence},
        {"next_probe_layers", next},
        {"complete", complete},
        {"reason", complete && (int) probes.size() >= max_probe_count && !adjacent && !verified_full ? "probe budget exhausted" : reason},
    };
}

std::vector<int> build_offload_benchmark_candidates(int fit_layers, int block_count, const std::vector<int> & offsets) {
    std::set<int> values;
    for (int offset : offsets) values.insert(clamp_layer(fit_layers + offset, block_count));
    return {values.begin(), values.end()};
}

json estimate_initial_cpu_moe(const json & metadata, double available_mib, double runtime_reserve_mib) {
    std::vector<json> blocks;
    if (has_key(metadata, "gguf_block_tensor_bytes") && metadata.at("gguf_block_tensor_bytes").is_array()) {
        for (const auto & entry : metadata.at("gguf_block_tensor_bytes")) {
            const double expert = finite_non_negative(entry, "expert_bytes", 0.0);
            if (expert > 0) blocks.push_back({{"block", int_value(entry, "block")}, {"bytes", num_value(entry, "bytes")}, {"expert_bytes", expert}});
        }
    }
    if (blocks.empty()) return nullptr;
    std::sort(blocks.begin(), blocks.end(), [](const json & a, const json & b) { return int_value(a, "block") < int_value(b, "block"); });
    const double total_bytes = std::max(0.0, finite_non_negative(metadata, "gguf_tensor_bytes", 0.0));
    double expert_bytes = 0.0;
    for (const auto & block : blocks) expert_bytes += num_value(block, "expert_bytes");
    double gpu_bytes = std::max(0.0, total_bytes - expert_bytes) + expert_bytes;
    const double budget_bytes = std::max(0.0, available_mib - std::max(0.0, runtime_reserve_mib)) * MIB;
    int n_cpu_moe = 0;
    while (n_cpu_moe < (int) blocks.size() && gpu_bytes > budget_bytes) {
        gpu_bytes -= num_value(blocks[n_cpu_moe], "expert_bytes");
        ++n_cpu_moe;
    }
    return {{"expertBlockCount", (int) blocks.size()}, {"nCpuMoe", n_cpu_moe}};
}

std::vector<int> build_moe_benchmark_candidates(int load_fit_anchor, int expert_block_count, const std::vector<int> & offsets, const std::vector<double> & ratios, const std::vector<int> & tail_offsets) {
    const int count = std::max(0, expert_block_count);
    const int anchor = clamp_layer(load_fit_anchor, count);
    std::set<int> candidates;
    auto add = [&](int value) { candidates.insert(clamp_layer(value, count)); };
    for (int offset : offsets) add(anchor + offset);
    for (double ratio : ratios) if (std::isfinite(ratio) && ratio >= 0 && ratio <= 1) add((int) std::round(count * ratio));
    for (int offset : tail_offsets) add(count + offset);
    return {candidates.begin(), candidates.end()};
}

json context_candidate_kv(const json & candidate) {
    const std::string fallback = str_value(candidate, "kv", "q8_0");
    const std::string k = str_value(candidate, "kv_k", fallback);
    const std::string v = str_value(candidate, "kv_v", fallback);
    return {{"k", k}, {"v", v}, {"label", k == v ? "kv=" + k : "kvk=" + k + "_kvv=" + v}};
}

std::string plan_workload_identity(const json & workload) {
    const std::string kind = str_value(workload, "kind", "baseline");
    if (kind == "prefill") return "prefill=" + std::to_string(int_value(workload, "prefillTokens"));
    if (kind == "kv-fill") return "kvfill=" + std::to_string(int_value(workload, "kvFillTokens"));
    return "";
}

std::vector<json> workload_profiles_for_context(int64_t context_size, const json & cfg, const std::string & mode) {
    if (mode == "baseline") return {};
    const json planning = has_key(cfg, "planning") ? cfg.at("planning") : json::object();
    const json settings = has_key(planning, "workload_sweeps") ? planning.at("workload_sweeps") : json::object();
    const json bench = has_key(cfg, "bench") ? cfg.at("bench") : json::object();
    const int64_t reserve = int_value(settings, "context_reserve_tokens", 512);
    const int64_t n_predict = int_value(bench, "n_predict", 128);
    const int64_t max_target = std::max<int64_t>(0, context_size - reserve - n_predict);
    std::vector<json> out;
    std::set<std::string> seen;
    auto add = [&](json workload) {
        const int64_t target = str_value(workload, "kind") == "prefill" ? int_value(workload, "prefillTokens") : int_value(workload, "kvFillTokens");
        const std::string key = str_value(workload, "kind") + ":" + std::to_string(target);
        if (!seen.count(key)) {
            seen.insert(key);
            out.push_back(workload);
        }
    };
    if (mode == "prefill" || mode == "all") {
        std::vector<int> micro = ints_from_array(settings, "prefill_micro_tokens");
        if (micro.empty()) micro = has_key(settings, "prefill_ratios") ? std::vector<int>{2048} : ints_from_array(settings, "prefill_tokens");
        if (micro.empty()) micro = {2048};
        for (int tokens : micro) if (tokens > 0 && tokens <= max_target) add({{"kind", "prefill"}, {"prefillTokens", tokens}});
        for (double ratio : doubles_from_array(settings, "prefill_ratios")) {
            const int64_t tokens = (int64_t) std::floor(context_size * ratio);
            if (ratio > 0 && ratio < 1 && tokens > 0 && tokens <= max_target) add({{"kind", "prefill"}, {"prefillTokens", tokens}});
        }
    }
    if (mode == "kv-fill" || mode == "all") {
        std::vector<double> ratios = doubles_from_array(settings, "kv_fill_ratios");
        if (ratios.empty()) ratios = {0.25, 0.5, 0.75, 0.9};
        for (double ratio : ratios) {
            const int64_t tokens = (int64_t) std::floor(context_size * ratio);
            if (ratio > 0 && ratio < 1 && tokens > 0 && tokens <= max_target) add({{"kind", "kv-fill"}, {"kvFillTokens", tokens}});
        }
    }
    return out;
}

json new_plan_item(const json & meta, const std::string & sweep, const json & level, const std::string & extra_args, const std::string & label, const json & workload, const json & control_kind, const json & mmap_policy) {
    const std::string workload_kind = str_value(workload, "kind", "baseline");
    const std::string workload_identity = plan_workload_identity(workload);
    const std::string identity_label = workload_identity.empty() ? label : label + "_" + workload_identity;
    json item = {
        {"id", sanitize(str_value(meta, "model") + "_" + str_value(meta, "variant"), 40) + "__" + sanitize(identity_label, 80)},
        {"artifact_id", has_key(meta, "artifact_id") ? meta.at("artifact_id") : json(nullptr)},
        {"model_id", has_key(meta, "model_id") ? meta.at("model_id") : json(nullptr)},
        {"preset_id", has_key(meta, "preset_id") ? meta.at("preset_id") : json(nullptr)},
        {"model_path", str_value(meta, "path")},
        {"mmproj_path", has_key(meta, "mmproj") ? meta.at("mmproj") : json(nullptr)},
        {"model", str_value(meta, "model")},
        {"variant", str_value(meta, "variant")},
        {"series", str_value(meta, "series")},
        {"sweep", sweep},
        {"level", level},
        {"reasoning_mode", has_key(meta, "reasoning_mode") ? meta.at("reasoning_mode") : json(nullptr)},
        {"template_note", has_key(meta, "template_note") ? meta.at("template_note") : json(nullptr)},
        {"gguf_context_length", has_key(meta, "gguf_context_length") ? meta.at("gguf_context_length") : json(nullptr)},
        {"gguf_architecture", has_key(meta, "gguf_architecture") ? meta.at("gguf_architecture") : json(nullptr)},
        {"workload_kind", workload_kind},
        {"control_kind", control_kind},
        {"row_role", infer_row_role({{"control_kind", control_kind}, {"workload_kind", workload_kind}})},
        {"prefill_target_tokens", int_value(workload, "prefillTokens")},
        {"kv_fill_target_tokens", int_value(workload, "kvFillTokens")},
        {"label", str_value(meta, "model") + " " + str_value(meta, "variant") + " @ " + identity_label},
        {"extra_args", extra_args},
        {"mmap_mode", has_key(mmap_policy, "mode") ? mmap_policy.at("mode") : json(has_flag(extra_args, "--no-mmap") ? "no-mmap" : "unknown")},
        {"mmap_policy_reason", has_key(mmap_policy, "reason") ? mmap_policy.at("reason") : json(nullptr)},
        {"nommap_required_mib", has_key(mmap_policy, "nommapRequiredMib") ? mmap_policy.at("nommapRequiredMib") : json(nullptr)},
        {"ram_available_before_load_mib", has_key(mmap_policy, "ramAvailableBeforeLoadMib") ? mmap_policy.at("ramAvailableBeforeLoadMib") : json(nullptr)},
    };
    return item;
}

std::vector<json> invoke_plan(const std::vector<json> & catalog, const json & cfg, const std::vector<json> & models_catalog, const json & presets, const json & opts) {
    const int64_t absolute_cap = int_value(cfg, "max_context_cap");
    int64_t global_cap = absolute_cap;
    const int64_t preset_max = int_value(opts, "presetMaxCtx");
    if (preset_max > 0 && (global_cap == 0 || preset_max < global_cap)) global_cap = preset_max;
    const json hardware = has_key(cfg, "hardware") ? cfg.at("hardware") : json::object();
    const json planning = has_key(cfg, "planning") ? cfg.at("planning") : json::object();
    const double topology_vram_mib = effective_vram_mib(hardware, planning);
    const std::string adapter = planner_adapter(hardware);
    const std::string threads = int_value(hardware, "cpu_cores_physical") > 0
        ? " --threads " + std::to_string(int_value(hardware, "cpu_cores_physical")) + " --threads-batch " + std::to_string(int_value(hardware, "cpu_threads_logical", int_value(hardware, "cpu_cores_physical")))
        : "";
    const std::string base = str_value(cfg, "base_args") + threads;
    const int64_t ram_available = int_value(hardware, "system_ram_available_mib", 0);
    const auto level_map = catalog_level_map(models_catalog, presets);
    const auto context_map = catalog_context_map(models_catalog);

    std::vector<json> ctx_candidates = array_from(cfg, "context_candidates");
    std::vector<int> ctx_override = ints_from_array(opts, "contextSizes");
    if (ctx_override.empty()) ctx_override = ints_from_array(opts, "presetCtxSizes");
    if (!ctx_override.empty()) {
        std::map<int64_t, json> kv_by_ctx;
        for (const auto & candidate : ctx_candidates) {
            const auto kv = context_candidate_kv(candidate);
            kv_by_ctx[int_value(candidate, "ctx")] = {{"kv_k", kv.at("k")}, {"kv_v", kv.at("v")}};
        }
        ctx_candidates.clear();
        for (int ctx : ctx_override) {
            json candidate = {{"ctx", ctx}};
            const auto it = kv_by_ctx.find(ctx);
            if (it != kv_by_ctx.end()) {
                candidate["kv_k"] = it->second.at("kv_k");
                candidate["kv_v"] = it->second.at("kv_v");
            } else {
                candidate["kv"] = "q8_0";
            }
            ctx_candidates.push_back(candidate);
        }
    }

    std::vector<json> plan;
    for (const auto & meta : catalog) {
        const std::string model_filter = str_value(opts, "model");
        if (!model_filter.empty() && !std::regex_search(str_value(meta, "model"), std::regex(model_filter, std::regex::icase))) continue;
        const std::string sweep = get_sweep_kind(meta, cfg);
        const std::string name = lower(base_name(str_value(meta, "path")));
        const auto lit = level_map.find(name);
        const json level = lit == level_map.end() ? json(nullptr) : json(lit->second);
        if (has_key(opts, "level") && opts.at("level").is_string() && level != opts.at("level")) continue;
        const auto cit = context_map.find(name);
        const int64_t per_model_cap = cit != context_map.end() ? cit->second : int_value(meta, "gguf_context_length");
        const json mmap_policy = model_base_args(meta, base, ram_available, (int64_t) topology_vram_mib);
        const std::string per_model_base = str_value(mmap_policy, "baseArgs");
        const std::string suffix = per_model_base.empty() ? "" : " " + per_model_base;
        plan.push_back(new_plan_item(meta, sweep, level, "", "vanilla_llama_cpp", json::object(), "vanilla"));

        if (sweep == "context") {
            std::vector<json> model_candidates = ctx_candidates;
            if (ctx_override.empty() && per_model_cap > 0) {
                for (auto & candidate : model_candidates) if (int_value(candidate, "ctx") == per_model_cap) candidate["fromModelMax"] = true;
                const bool present = std::any_of(model_candidates.begin(), model_candidates.end(), [&](const json & c) { return int_value(c, "ctx") == per_model_cap; });
                if (!present && ctx_allowed(per_model_cap, absolute_cap, per_model_cap)) {
                    json inherited = model_candidates.empty() ? json{{"kv", "q8_0"}} : model_candidates.back();
                    for (const auto & candidate : model_candidates) {
                        if (int_value(candidate, "ctx") > per_model_cap) {
                            inherited = candidate;
                            break;
                        }
                    }
                    const auto kv = context_candidate_kv(inherited);
                    model_candidates.push_back({{"ctx", per_model_cap}, {"kv_k", kv.at("k")}, {"kv_v", kv.at("v")}, {"fromModelMax", true}});
                    std::sort(model_candidates.begin(), model_candidates.end(), [](const json & a, const json & b) { return int_value(a, "ctx") < int_value(b, "ctx"); });
                }
            }
            std::vector<json> valid;
            for (const auto & candidate : model_candidates) {
                if (ctx_allowed(int_value(candidate, "ctx"), bool_value(candidate, "fromModelMax") ? absolute_cap : global_cap, per_model_cap)) valid.push_back(candidate);
            }
            std::string anchor_primary_id;
            for (const auto & candidate : valid) {
                const int64_t ctx = int_value(candidate, "ctx");
                const auto kv = context_candidate_kv(candidate);
                const std::string label = "ctx=" + std::to_string(ctx) + "_" + str_value(kv, "label");
                const std::string args = "--ctx-size " + std::to_string(ctx) + " --gpu-layers 99 --cache-type-k " + str_value(kv, "k") + " --cache-type-v " + str_value(kv, "v") + suffix;
                json primary = new_plan_item(meta, sweep, level, args, label, json::object(), nullptr, mmap_policy);
                plan.push_back(primary);
                anchor_primary_id = str_value(primary, "id");
                const json rescue = has_key(planning, "kv_rescue") ? planning.at("kv_rescue") : json::object();
                const std::string rescue_k = str_value(rescue, "kv_k", "q4_0");
                const std::string rescue_v = str_value(rescue, "kv_v", "q4_0");
                const int64_t rescue_min = int_value(rescue, "min_context_tokens", 65536);
                if (!has_key(rescue, "enabled") || bool_value(rescue, "enabled", true)) {
                    if (ctx >= rescue_min && (str_value(kv, "k") != rescue_k || str_value(kv, "v") != rescue_v)) {
                        const auto rescue_kv = context_candidate_kv({{"kv_k", rescue_k}, {"kv_v", rescue_v}});
                        const std::string rescue_label = "ctx=" + std::to_string(ctx) + "_" + str_value(rescue_kv, "label") + "_rescue";
                        const std::string rescue_args = "--ctx-size " + std::to_string(ctx) + " --gpu-layers 99 --cache-type-k " + rescue_k + " --cache-type-v " + rescue_v + suffix;
                        json rescue_item = new_plan_item(meta, sweep, level, rescue_args, rescue_label, json::object(), nullptr, mmap_policy);
                        rescue_item["conditional_kind"] = "kv_rescue";
                        rescue_item["conditional_source_id"] = str_value(primary, "id");
                        plan.push_back(rescue_item);
                    }
                }
            }
            if (!valid.empty()) {
                const auto anchor = valid.back();
                const int64_t ctx = int_value(anchor, "ctx");
                const auto kv = context_candidate_kv(anchor);
                auto matched = matched_vanilla_baselines(meta, sweep, level, ctx, str_value(kv, "k"), str_value(kv, "v"), anchor_primary_id);
                plan.insert(plan.end(), matched.begin(), matched.end());
                const std::string args = "--ctx-size " + std::to_string(ctx) + " --gpu-layers 99 --cache-type-k " + str_value(kv, "k") + " --cache-type-v " + str_value(kv, "v") + suffix;
                for (const auto & workload : workload_profiles_for_context(ctx, cfg, str_value(opts, "workloadSweep", "baseline"))) {
                    const std::string label = "ctx=" + std::to_string(ctx) + "_" + str_value(kv, "label");
                    plan.push_back(new_plan_item(meta, sweep, level, args, label, workload, nullptr, mmap_policy));
                }
            }
        } else if (sweep == "moe-cpu") {
            std::vector<int> sweep_values = ints_from_array(planning, "moecpu_sweep");
            json structural = json::object();
            int anchor = 0;
            if (sweep_values.empty()) {
                structural = estimate_initial_cpu_moe(meta, topology_vram_mib, num_value(planning, "overhead_mib", 1200.0));
                const int blocks = int_value(structural, "expertBlockCount", int_value(meta, "gguf_block_count"));
                anchor = int_value(structural, "nCpuMoe", blocks / 2);
                sweep_values = closest_candidates(build_moe_benchmark_candidates(anchor, blocks), anchor, 6);
            }
            const int64_t sweep_ctx = choose_sweep_context(ctx_candidates, global_cap, per_model_cap, 16384);
            for (size_t i = 0; i < sweep_values.size(); ++i) {
                const int n = sweep_values[i];
                const std::string args = "--ctx-size " + std::to_string(sweep_ctx) + " --gpu-layers 99 --n-cpu-moe " + std::to_string(n) + " --cache-type-k q8_0 --cache-type-v q8_0" + suffix;
                json item = new_plan_item(meta, sweep, level, args, "ncpumoe_" + std::to_string(n), json::object(), nullptr, mmap_policy);
                if (structural.is_object() && !structural.empty()) annotate_adaptive_item(item, structural, adapter, anchor, i);
                plan.push_back(std::move(item));
            }
        } else {
            std::vector<int> sweep_values = ints_from_array(planning, "offload_sweep");
            json structural = json::object();
            int anchor = 0;
            if (sweep_values.empty()) {
                const json estimate_options = {
                    {"availableMib", topology_vram_mib},
                    {"runtimeReserveMib", num_value(planning, "overhead_mib", 1200.0)},
                    {"mmprojMib", num_value(meta, "mmproj_mib", 0.0)},
                };
                structural = estimate_initial_offload(meta, estimate_options);
                anchor = int_value(structural, "estimatedLayers", 0);
                const int blocks = int_value(structural, "blockCount", int_value(meta, "gguf_block_count"));
                sweep_values = closest_candidates(build_offload_benchmark_candidates(anchor, blocks, {-4, -2, 0, 1, 2, 4}), anchor, 6);
            }
            const int64_t sweep_ctx = choose_sweep_context(ctx_candidates, global_cap, per_model_cap, 16384);
            for (size_t i = 0; i < sweep_values.size(); ++i) {
                const int n = sweep_values[i];
                const std::string args = "--ctx-size " + std::to_string(sweep_ctx) + " --gpu-layers " + std::to_string(n) + " --cache-type-k q8_0 --cache-type-v q8_0" + suffix;
                json item = new_plan_item(meta, sweep, level, args, "ngl_" + std::to_string(n), json::object(), nullptr, mmap_policy);
                annotate_adaptive_item(item, structural, adapter, anchor, i);
                plan.push_back(std::move(item));
            }
        }
    }
    for (auto & item : plan) {
        item["capability_source"] = has_key(cfg, "capabilities") ? "build-probe" : "unprobed";
    }
    return plan;
}

json read_gguf_plan_meta(const std::string & path) {
    const auto file_size = std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;
    const std::string stem = stem_name(path);
    std::string model = stem;
    std::string variant = "unknown";
    std::smatch m;
    if (std::regex_search(stem, m, std::regex(R"((.*?)[\.-]((?:UD-)?(?:IQ[1-4][A-Za-z0-9_]*|Q[2-8][A-Za-z0-9_]*|F(?:16|32)|BF16|FP(?:8|16|32)|MXFP4)(?:[\.-].*)?)$)", std::regex::icase))) {
        model = m[1].str();
        variant = m[2].str();
    }
    json meta = {
        {"path", path},
        {"model", model.empty() ? stem : model},
        {"variant", variant},
        {"series", lower(model).find("qwen") != std::string::npos ? "qwen" : (lower(model).find("deepseek") != std::string::npos ? "deepseek" : "local")},
        {"size_mib", std::round((double) file_size / MIB)},
        {"is_moe", lower(stem).find("moe") != std::string::npos || std::regex_search(stem, std::regex(R"(A\d+B)", std::regex::icase))},
        {"mmproj", nullptr},
    };
    gguf_init_params params = { true, nullptr };
    gguf_context * ctx = gguf_init_from_file(path.c_str(), params);
    meta["gguf_readable"] = ctx != nullptr;
    if (!ctx) return meta;
    const json general_name = gguf_scalar(ctx, {"general.name", "general.basename"});
    if (general_name.is_string() && !general_name.get<std::string>().empty()) meta["model"] = general_name;
    const json arch = gguf_scalar(ctx, {"general.architecture"});
    if (arch.is_string()) {
        const std::string a = arch.get<std::string>();
        meta["gguf_architecture"] = a;
        if (lower(a).find("moe") != std::string::npos) meta["is_moe"] = true;
        const json block_count = gguf_scalar(ctx, {a + ".block_count"});
        const json context_length = gguf_scalar(ctx, {a + ".context_length"});
        if (!block_count.is_null()) meta["gguf_block_count"] = block_count;
        if (!context_length.is_null()) meta["gguf_context_length"] = context_length;
    }
    meta["gguf_tensor_count"] = gguf_get_n_tensors(ctx);
    meta["gguf_tensor_data_offset"] = gguf_get_data_offset(ctx);

    std::map<int, json> blocks;
    uint64_t total = 0;
    uint64_t global = 0;
    uint64_t experts = 0;
    std::regex block_re(R"(blk\.(\d+)\.)");
    for (int64_t i = 0; i < gguf_get_n_tensors(ctx); ++i) {
        const std::string name = gguf_get_tensor_name(ctx, i);
        const uint64_t size = gguf_get_tensor_size(ctx, i);
        total += size;
        std::smatch bm;
        const bool is_expert = lower(name).find("expert") != std::string::npos || lower(name).find("exps") != std::string::npos;
        if (std::regex_search(name, bm, block_re)) {
            const int block = std::stoi(bm[1].str());
            if (!blocks.count(block)) blocks[block] = {{"block", block}, {"bytes", 0}, {"expert_bytes", 0}};
            blocks[block]["bytes"] = blocks[block].at("bytes").get<uint64_t>() + size;
            if (is_expert) {
                blocks[block]["expert_bytes"] = blocks[block].at("expert_bytes").get<uint64_t>() + size;
                experts += size;
            }
        } else {
            global += size;
        }
    }
    json block_arr = json::array();
    for (const auto & [_, entry] : blocks) block_arr.push_back(entry);
    meta["gguf_tensor_bytes"] = total;
    meta["gguf_global_tensor_bytes"] = global;
    meta["gguf_expert_tensor_bytes"] = experts;
    meta["gguf_block_tensor_bytes"] = block_arr;
    if (experts > 0) meta["is_moe"] = true;
    gguf_free(ctx);
    return meta;
}

} // namespace caliber
