#pragma once

#include "caliber-scoring.h"

#include <string>
#include <vector>

namespace caliber {

json estimate_initial_offload(const json & meta, const json & options);
json estimate_offload_cliff(const json & options);
std::vector<int> build_offload_benchmark_candidates(int fit_layers, int block_count, const std::vector<int> & offsets = {-6, -3, -1, 0, 1, 3});

json estimate_initial_cpu_moe(const json & metadata, double available_mib, double runtime_reserve_mib = 512.0);
std::vector<int> build_moe_benchmark_candidates(
        int load_fit_anchor,
        int expert_block_count,
        const std::vector<int> & offsets = {-3, -1, 0, 1, 2, 3},
        const std::vector<double> & ratios = {0.5, 0.75},
        const std::vector<int> & tail_offsets = {-3, -1, 0});

json context_candidate_kv(const json & candidate);
std::string plan_workload_identity(const json & workload = json::object());
std::vector<json> workload_profiles_for_context(int64_t context_size, const json & cfg, const std::string & mode = "baseline");
json new_plan_item(
        const json & meta,
        const std::string & sweep,
        const json & level,
        const std::string & extra_args,
        const std::string & label,
        const json & workload = json::object(),
        const json & control_kind = nullptr,
        const json & mmap_policy = json::object());

std::vector<json> invoke_plan(
        const std::vector<json> & catalog,
        const json & cfg,
        const std::vector<json> & models_catalog = {},
        const json & presets = json::object(),
        const json & opts = json::object());

json read_gguf_plan_meta(const std::string & path);

} // namespace caliber
