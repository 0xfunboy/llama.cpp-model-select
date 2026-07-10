#include "server-caliber-advisor.h"

#include "caliber-plan.h"
#include "caliber-scoring.h"
#include "server-common.h"
#include "server-models.h"
#include "server-persistence.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

#ifndef LLAMA_CALIBER_REPORTS_DIR
#define LLAMA_CALIBER_REPORTS_DIR "tools/ui/static/reports/caliber"
#endif

namespace {

using json = nlohmann::ordered_json;

struct caliber_event {
    uint64_t seq = 0;
    std::string event;
    json data;
};

struct caliber_job {
    std::string id;
    std::string status = "queued";
    std::string error;
    std::string report_id;
    int current = 0;
    int total = 0;
    bool finished = false;
    uint64_t next_seq = 1;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<caliber_event> events;
};

static std::filesystem::path reports_dir() {
    return std::filesystem::path(LLAMA_CALIBER_REPORTS_DIR);
}

static std::string read_text_file(const std::filesystem::path & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to read " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_text_file(const std::filesystem::path & path, const std::string & text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to write " + path.string());
    out << text;
}

static std::string shell_quote(const std::string & value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

static std::filesystem::path current_executable_dir() {
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
    return std::filesystem::current_path() / "build" / "bin";
}

static std::string run_command_capture(const std::string & command, int & exit_code) {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE * pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) throw std::runtime_error("failed to start benchmark command");
    while (fgets(buffer.data(), (int) buffer.size(), pipe) != nullptr) output += buffer.data();
    exit_code = pclose(pipe);
    return output;
}

static json parse_first_json_array(const std::string & output) {
    const auto begin = output.find('[');
    const auto end = output.rfind(']');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        throw std::runtime_error("benchmark did not return JSON output");
    }
    return json::parse(output.substr(begin, end - begin + 1));
}

static std::string isoish_timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static std::string slugify(std::string value) {
    std::string out;
    for (char c : value) {
        if (std::isalnum((unsigned char) c)) out.push_back((char) std::tolower(c));
        else if (c == '-' || c == '_' || c == '.') out.push_back(c);
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "caliber" : out;
}

static void res_ok(server_http_res_ptr & res, const json & data) {
    res->status = 200;
    res->content_type = "application/json; charset=utf-8";
    res->data = safe_json_to_str(data);
}

static void res_err(server_http_res_ptr & res, const std::string & message, int code = 400) {
    res->status = code;
    res->content_type = "application/json; charset=utf-8";
    res->data = safe_json_to_str({{"error", {{"message", message}, {"code", code}}}});
}

static std::string model_path_from_meta(const server_model_meta & meta) {
    std::string path;
    if (meta.preset.get_option("LLAMA_ARG_MODEL", path)) return path;
    return "";
}

static std::string hf_repo_from_meta(const server_model_meta & meta) {
    std::string repo;
    if (meta.preset.get_option("LLAMA_ARG_HF_REPO", repo)) return repo;
    return "";
}

static std::optional<server_model_meta> find_router_model(server_models_routes & router, const std::string & id) {
    for (const auto & meta : router.models.get_all_meta()) {
        if (meta.name == id) return meta;
    }
    return std::nullopt;
}

static std::vector<std::string> split_cli_args(const std::string & text) {
    std::vector<std::string> out;
    std::string current;
    char quote = 0;
    bool escape = false;
    for (char c : text) {
        if (escape) {
            current.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (quote) {
            if (c == quote) quote = 0;
            else current.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace((unsigned char) c)) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

static std::string arg_value(const std::vector<std::string> & args, const std::vector<std::string> & names) {
    for (size_t i = 0; i < args.size(); ++i) {
        for (const auto & name : names) {
            if (args[i] == name && i + 1 < args.size()) return args[i + 1];
            const std::string prefix = name + "=";
            if (args[i].rfind(prefix, 0) == 0) return args[i].substr(prefix.size());
        }
    }
    return "";
}

static int arg_int_value(const std::vector<std::string> & args, const std::vector<std::string> & names, int fallback) {
    const std::string value = arg_value(args, names);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

static void set_int_arg(json & entry, const std::vector<std::string> & args, const std::string & key, const std::vector<std::string> & names) {
    const std::string value = arg_value(args, names);
    if (value.empty()) return;
    try {
        entry[key] = std::stoi(value);
    } catch (...) {
    }
}

static void set_string_arg(json & entry, const std::vector<std::string> & args, const std::string & key, const std::vector<std::string> & names) {
    const std::string value = arg_value(args, names);
    if (!value.empty()) entry[key] = value;
}

static void apply_runtime_args(json & entry, const std::string & extra_args) {
    const std::vector<std::string> args = split_cli_args(extra_args);
    set_int_arg(entry, args, "ctx_size", {"--ctx-size", "-c"});
    set_int_arg(entry, args, "n_gpu_layers", {"--gpu-layers", "--n-gpu-layers", "-ngl"});
    set_int_arg(entry, args, "parallel", {"--parallel", "-np"});
    set_int_arg(entry, args, "batch_size", {"--batch-size", "-b"});
    set_int_arg(entry, args, "ubatch_size", {"--ubatch-size", "-ub"});
    set_int_arg(entry, args, "main_gpu", {"--main-gpu", "-mg"});
    set_int_arg(entry, args, "n_cpu_moe", {"--n-cpu-moe"});
    set_int_arg(entry, args, "threads", {"--threads", "-t"});
    set_string_arg(entry, args, "cache_type_k", {"--cache-type-k", "-ctk"});
    set_string_arg(entry, args, "cache_type_v", {"--cache-type-v", "-ctv"});
    set_string_arg(entry, args, "flash_attn", {"--flash-attn", "-fa"});
    set_string_arg(entry, args, "split_mode", {"--split-mode", "-sm"});
    set_string_arg(entry, args, "tensor_split", {"--tensor-split", "-ts"});
}

static std::string normalized_tensor_split(std::string value) {
    std::replace(value.begin(), value.end(), ',', '/');
    return value;
}

struct llama_bench_dims {
    int requested_context = 0;
    int allocated_context = 0;
    int prompt_tokens = 512;
    int generate_tokens = 128;
    int depth_tokens = 0;
};

static int requested_context_for_item(const json & item, const json & cfg) {
    const std::vector<std::string> source = split_cli_args(json_value(item, "extra_args", std::string()));
    const int ctx = arg_int_value(source, {"--ctx-size", "-c"}, 0);
    if (ctx <= 0) return 0;
    const int model_cap = json_value(item, "gguf_context_length", 0);
    const int global_cap = json_value(cfg, "max_context_cap", 0);
    int capped = ctx;
    if (model_cap > 0) capped = std::min(capped, model_cap);
    if (global_cap > 0) capped = std::min(capped, global_cap);
    return std::max(0, capped);
}

static llama_bench_dims llama_bench_dims_for_item(const json & item, const json & cfg) {
    llama_bench_dims dims;
    dims.requested_context = requested_context_for_item(item, cfg);
    dims.generate_tokens = json_value(json_value(cfg, "bench", json::object()), "n_predict", 128);
    dims.generate_tokens = std::max(1, dims.generate_tokens);

    const std::string workload = json_value(item, "workload_kind", std::string("baseline"));
    const int ctx = dims.requested_context > 0 ? dims.requested_context : 8192;
    if (workload == "prefill") {
        const int target = json_value(item, "prefill_target_tokens", 0);
        dims.prompt_tokens = target > 0 ? target : std::min(std::max(2048, ctx / 4), std::max(512, ctx - 512));
    } else if (workload == "kv-fill") {
        const int target = json_value(item, "kv_fill_target_tokens", 0);
        dims.prompt_tokens = 512;
        dims.depth_tokens = target > 0 ? target : std::min(std::max(2048, (ctx * 3) / 4), std::max(0, ctx - dims.prompt_tokens - dims.generate_tokens));
    }

    if (dims.requested_context > 0) {
        const int max_prompt = std::max(1, dims.requested_context - dims.generate_tokens);
        dims.prompt_tokens = std::min(std::max(1, dims.prompt_tokens), max_prompt);
        const int max_depth = std::max(0, dims.requested_context - dims.prompt_tokens - dims.generate_tokens);
        dims.depth_tokens = std::min(std::max(0, dims.depth_tokens), max_depth);
    }
    dims.allocated_context = dims.prompt_tokens + dims.generate_tokens + dims.depth_tokens;
    return dims;
}

static std::vector<std::string> llama_bench_args_for_item(const json & item, const json & cfg, llama_bench_dims & dims) {
    const std::vector<std::string> source = split_cli_args(json_value(item, "extra_args", std::string()));
    const std::string model_path = json_value(item, "model_path", json_value(item, "path", std::string()));
    if (model_path.empty()) throw std::runtime_error("plan item has no model path");
    dims = llama_bench_dims_for_item(item, cfg);

    std::vector<std::string> out = {
        "-m", model_path,
        "-o", "json",
        "-r", "1",
        "--no-warmup",
        "-p", std::to_string(dims.prompt_tokens),
        "-n", std::to_string(dims.generate_tokens),
    };
    if (dims.depth_tokens > 0) {
        out.push_back("-d");
        out.push_back(std::to_string(dims.depth_tokens));
    }

    auto push_value = [&](const std::string & dst, const std::vector<std::string> & names, bool tensor_split = false) {
        std::string value = arg_value(source, names);
        if (value.empty()) return;
        if (tensor_split) value = normalized_tensor_split(value);
        out.push_back(dst);
        out.push_back(value);
    };
    push_value("--n-gpu-layers", {"--gpu-layers", "--n-gpu-layers", "-ngl"});
    push_value("--cache-type-k", {"--cache-type-k", "-ctk"});
    push_value("--cache-type-v", {"--cache-type-v", "-ctv"});
    push_value("--n-cpu-moe", {"--n-cpu-moe", "-ncmoe"});
    push_value("--split-mode", {"--split-mode", "-sm"});
    push_value("--main-gpu", {"--main-gpu", "-mg"});
    push_value("--flash-attn", {"--flash-attn", "-fa"});
    push_value("--tensor-split", {"--tensor-split", "-ts"}, true);
    push_value("--batch-size", {"--batch-size", "-b"});
    push_value("--ubatch-size", {"--ubatch-size", "-ub"});
    push_value("--threads", {"--threads", "-t"});
    return out;
}

static json first_bench_row(const json & rows, bool generation) {
    if (!rows.is_array()) return nullptr;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const int prompt = json_value(row, "n_prompt", 0);
        const int gen = json_value(row, "n_gen", 0);
        if (generation && gen > 0) return row;
        if (!generation && prompt > 0 && gen == 0) return row;
    }
    return nullptr;
}

static json run_llama_bench_item(const json & item, const json & cfg) {
    llama_bench_dims dims;
    const auto args = llama_bench_args_for_item(item, cfg, dims);
    std::string command = shell_quote((current_executable_dir() / "llama-bench").string());
    for (const auto & arg : args) command += " " + shell_quote(arg);
    int exit_code = 0;
    const std::string output = run_command_capture(command, exit_code);
    if (exit_code != 0) throw std::runtime_error("llama-bench failed: " + output.substr(0, 2000));
    json parsed = parse_first_json_array(output);
    if (!parsed.is_array() || parsed.empty() || !parsed[0].is_object()) {
        throw std::runtime_error("llama-bench returned no result rows");
    }
    const json prompt_row = first_bench_row(parsed, false);
    const json gen_row = first_bench_row(parsed, true);
    if (!prompt_row.is_object() && !gen_row.is_object()) {
        throw std::runtime_error("llama-bench returned neither prompt nor generation rows");
    }
    json bench = gen_row.is_object() ? gen_row : prompt_row;
    json run = bench;
    run["ok"] = true;
    run["eval_tps"] = gen_row.is_object() ? json_value(gen_row, "avg_ts", 0.0) : 0.0;
    run["prompt_tps"] = prompt_row.is_object() ? json_value(prompt_row, "avg_ts", 0.0) : 0.0;
    run["prompt_n"] = prompt_row.is_object() ? json_value(prompt_row, "n_prompt", dims.prompt_tokens) : dims.prompt_tokens;
    run["eval_n"] = gen_row.is_object() ? json_value(gen_row, "n_gen", dims.generate_tokens) : 0;
    run["n_prompt"] = dims.prompt_tokens;
    run["n_gen"] = dims.generate_tokens;
    run["n_depth"] = dims.depth_tokens;
    run["ctx_size"] = dims.requested_context > 0 ? dims.requested_context : dims.allocated_context;
    run["requested_context_size"] = dims.requested_context > 0 ? json(dims.requested_context) : json(nullptr);
    run["benchmark_allocated_context_size"] = dims.allocated_context;
    run["benchmark_depth_tokens"] = dims.depth_tokens;
    run["benchmark_prompt_tokens"] = dims.prompt_tokens;
    run["benchmark_generate_tokens"] = dims.generate_tokens;
    run["benchmark_rows"] = parsed;
    if (prompt_row.is_object()) run["llama_bench_prompt_row"] = prompt_row;
    if (gen_row.is_object()) run["llama_bench_generation_row"] = gen_row;
    run["benchmark_note"] = "llama-bench synthetic benchmark; eval_tps is the tg row, prompt_tps is the pp row";
    const double model_size_bytes = json_value(bench, "model_size", 0.0);
    if (model_size_bytes > 0) run["vram_peak_mib"] = model_size_bytes / 1024.0 / 1024.0;
    if (!run.contains("shared_peak_mib")) run["shared_peak_mib"] = 0;
    if (!run.contains("gpu_power_peak_w")) run["gpu_power_peak_w"] = 0;
    run["benchmark_backend"] = "llama-bench";
    run["bench_command"] = command;
    json row = caliber::aggregate_bench_result(item, cfg, {run});
    row["benchmark_backend"] = "llama-bench";
    row["bench_command"] = command;
    return row;
}

static std::optional<json::iterator> find_model_entry(json & models, const std::string & id) {
    if (!models.is_array()) return std::nullopt;
    for (auto it = models.begin(); it != models.end(); ++it) {
        if (it->is_object() && json_value(*it, "id", json_value(*it, "name", std::string())) == id) {
            return it;
        }
    }
    return std::nullopt;
}

static json model_meta_json(const server_model_meta & meta) {
    const std::string path = model_path_from_meta(meta);
    json out = {
        {"id", meta.name},
        {"name", meta.name},
        {"source", server_model_source_to_string(meta.source)},
        {"status", server_model_status_to_string(meta.status)},
        {"path", path.empty() ? json(nullptr) : json(path)},
        {"hf_repo", hf_repo_from_meta(meta).empty() ? json(nullptr) : json(hf_repo_from_meta(meta))},
        {"tags", json::array()},
    };
    for (const auto & tag : meta.tags) out["tags"].push_back(tag);
    if (!path.empty() && std::filesystem::exists(path)) {
        try {
            out["plan_meta"] = caliber::read_gguf_plan_meta(path);
        } catch (...) {
            out["plan_meta_error"] = "failed to read GGUF metadata";
        }
    }
    return out;
}

static json default_plan_cfg() {
    return {
        {"hardware", {
            {"vram_budget_mib", 49152},
            {"vram_driver_usable_mib", 24576},
            {"cpu_cores_physical", (int) std::max(1u, std::thread::hardware_concurrency() / 2)},
            {"cpu_threads_logical", (int) std::max(1u, std::thread::hardware_concurrency())},
            {"system_ram_available_mib", 0},
        }},
        {"planning", {
            {"overhead_mib", 1200},
            {"moecpu_sweep", {0, 8, 16, 24, 32}},
            {"offload_sweep", {20, 28, 36, 48, 60, 80}},
            {"kv_rescue", {{"enabled", true}, {"min_context_tokens", 65536}, {"kv_k", "q4_0"}, {"kv_v", "q4_0"}}},
            {"workload_sweeps", {{"context_reserve_tokens", 512}, {"kv_fill_ratios", {0.25, 0.5, 0.75, 0.9}}, {"prefill_micro_tokens", {2048}}, {"prefill_ratios", {0.25, 0.9}}}},
        }},
        {"bench", {{"n_predict", 128}}},
        {"context_candidates", {{{"ctx", 8192}, {"kv", "q8_0"}}, {{"ctx", 32768}, {"kv", "q8_0"}}, {{"ctx", 131072}, {"kv", "q8_0"}}}},
        {"max_context_cap", 262144},
        {"base_args", "--flash-attn auto --parallel 1 --batch-size 512 --ubatch-size 512"},
    };
}

static json merge_cfg(json base, const json & patch) {
    if (patch.is_object()) base.merge_patch(patch);
    return base;
}

static json report_summary(const json & report, const std::filesystem::path & path) {
    return {
        {"id", json_value(report, "id", path.stem().string())},
        {"created_at", json_value(report, "created_at", std::string())},
        {"status", json_value(report, "status", std::string())},
        {"model", json_value(report, "model", std::string())},
        {"plan_items", report.contains("plan") && report["plan"].is_array() ? report["plan"].size() : 0},
        {"rows", report.contains("rows") && report["rows"].is_array() ? report["rows"].size() : 0},
        {"path", path.string()},
    };
}

static bool is_legacy_invalid_llama_bench_row(const json & row) {
    if (json_value(row, "benchmark_backend", std::string()) != "llama-bench") return false;
    if (row.contains("benchmark_note") || row.contains("benchmark_allocated_context_size")) return false;
    const bool missing_ctx = !row.contains("ctx_size") || row["ctx_size"].is_null() || json_value(row, "ctx_size", 0) <= 0;
    const bool no_prompt_metric = json_value(row, "prompt_tps", 0.0) <= 0.0;
    bool first_run_is_prompt_only = false;
    if (row.contains("runs") && row["runs"].is_array() && !row["runs"].empty() && row["runs"][0].is_object()) {
        first_run_is_prompt_only = json_value(row["runs"][0], "n_prompt", 0) > 0 && json_value(row["runs"][0], "n_gen", 0) == 0;
    }
    return missing_ctx && (no_prompt_metric || first_run_is_prompt_only);
}

static json normalize_report_for_api(json report) {
    if (!report.contains("rows") || !report["rows"].is_array()) return report;
    int invalid = 0;
    for (auto & row : report["rows"]) {
        if (!row.is_object() || !is_legacy_invalid_llama_bench_row(row)) continue;
        row["ok"] = false;
        row["eval_tps"] = 0;
        row["measurement_confidence"] = "invalid";
        row["failure_reason"] = "legacy_invalid_llama_bench_prompt_row";
        row["recommendation"] = "rerun-required";
        row["recommendation_reason"] = "This row was produced before llama-bench prompt/generation rows were separated; rerun the benchmark.";
        ++invalid;
    }
    if (invalid > 0) {
        report["legacy_invalid_rows"] = invalid;
        report["status_note"] = "Contains legacy invalid llama-bench rows; rerun required for comparison/FIT.";
    }
    return report;
}

} // namespace

struct server_caliber_advisor_routes::impl {
    server_models_routes & router;
    std::mutex jobs_mutex;
    std::map<std::string, std::shared_ptr<caliber_job>> jobs;

    explicit impl(server_models_routes & router) : router(router) {}

    void publish(const std::shared_ptr<caliber_job> & job, const std::string & event, json data) {
        std::lock_guard<std::mutex> lock(job->mutex);
        data["job_id"] = job->id;
        data["status"] = job->status;
        data["current"] = job->current;
        data["total"] = job->total;
        data["ts"] = isoish_timestamp();
        caliber_event ev;
        ev.seq = job->next_seq++;
        ev.event = event;
        ev.data = std::move(data);
        ev.data["seq"] = ev.seq;
        job->events.push_back(std::move(ev));
        while (job->events.size() > 200) job->events.pop_front();
        job->cv.notify_all();
    }

    std::vector<json> model_plan_metas(const json & body) {
        std::vector<json> metas;
        const std::string requested = json_value(body, "model", json_value(body, "id", std::string()));
        const std::string requested_path = json_value(body, "path", json_value(body, "model_path", std::string()));
        std::set<std::string> requested_models;
        if (body.contains("models") && body["models"].is_array()) {
            for (const auto & id : body["models"]) {
                if (id.is_string() && !id.get<std::string>().empty()) requested_models.insert(id.get<std::string>());
            }
        }
        if (!requested_path.empty()) {
            metas.push_back(caliber::read_gguf_plan_meta(requested_path));
            return metas;
        }
        for (const auto & meta : router.models.get_all_meta()) {
            if (!requested.empty() && requested != meta.name) continue;
            if (!requested_models.empty() && !requested_models.count(meta.name)) continue;
            const std::string path = model_path_from_meta(meta);
            if (path.empty() || !std::filesystem::exists(path)) continue;
            metas.push_back(caliber::read_gguf_plan_meta(path));
        }
        return metas;
    }

    json build_plan_payload(const json & body) {
        const auto metas = model_plan_metas(body);
        const json cfg = merge_cfg(default_plan_cfg(), json_value(body, "cfg", json::object()));
        const json opts = json_value(body, "opts", json::object());
        const auto items = caliber::invoke_plan(metas, cfg, {}, json::object(), opts);
        json plan = json::array();
        for (const auto & item : items) plan.push_back(item);
        return {{"cfg", cfg}, {"models", metas}, {"plan", plan}, {"plan_count", plan.size()}};
    }

    server_http_res_ptr handle_system(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        res_ok(res, {{"module", "caliber-advisor"}, {"reports_dir", reports_dir().string()}, {"default_cfg", default_plan_cfg()}});
        return res;
    }

    server_http_res_ptr handle_models(const server_http_req & req) {
        if (!req.get_param("reload").empty()) router.models.load_models();
        auto res = std::make_unique<server_http_res>();
        json models = json::array();
        for (const auto & meta : router.models.get_all_meta()) models.push_back(model_meta_json(meta));
        res_ok(res, {{"object", "list"}, {"data", models}});
        return res;
    }

    server_http_res_ptr handle_plan(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        const json body = req.body.empty() ? json::object() : json::parse(req.body);
        json payload = build_plan_payload(body);
        payload["object"] = "caliber.plan";
        res_ok(res, payload);
        return res;
    }

    server_http_res_ptr handle_sweep(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        const json body = req.body.empty() ? json::object() : json::parse(req.body);
        auto job = std::make_shared<caliber_job>();
        job->id = slugify("caliber-" + isoish_timestamp() + "-" + std::to_string(jobs.size() + 1));
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            jobs[job->id] = job;
        }
        publish(job, "queued", json::object());
        std::thread([this, job, body]() {
            try {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status = "running";
                }
                publish(job, "started", json::object());
                publish(job, "preflight", {{"message", "Unloading active router models before benchmark"}});
                router.models.unload_all();
                json payload = build_plan_payload(body);
                json plan = payload["plan"];
                const int requested_limit = json_value(body, "limit_rows", 0);
                if (requested_limit > 0 && plan.is_array() && (int) plan.size() > requested_limit) {
                    json limited = json::array();
                    for (int i = 0; i < requested_limit; ++i) limited.push_back(plan[(size_t) i]);
                    plan = limited;
                }
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->total = (int) plan.size();
                    job->current = 0;
                }
                const std::string model_name = payload["models"].is_array() && !payload["models"].empty()
                    ? json_value(payload["models"][0], "model", std::string("local"))
                    : std::string("local");
                const std::string report_id = slugify(model_name + "-" + isoish_timestamp());
                json rows = json::array();
                std::vector<json> winner_rows;
                for (const auto & item : plan) {
                    publish(job, "bench", {{"item", json_value(item, "id", json_value(item, "label", std::string()))}});
                    try {
                        json row = run_llama_bench_item(item, payload["cfg"]);
                        rows.push_back(row);
                        winner_rows.push_back(row);
                        publish(job, "row", {{"ok", true}, {"item", json_value(item, "id", json_value(item, "label", std::string()))}, {"eval_tps", json_value(row, "eval_tps", 0.0)}});
                    } catch (const std::exception & e) {
                        json row = item;
                        const int requested_ctx = requested_context_for_item(item, payload["cfg"]);
                        row["ok"] = false;
                        row["failure_reason"] = e.what();
                        row["eval_tps"] = 0;
                        row["ctx_size"] = requested_ctx > 0 ? requested_ctx : caliber::context_size_from_args(json_value(item, "extra_args", std::string()));
                        row["requested_context_size"] = requested_ctx > 0 ? json(requested_ctx) : json(nullptr);
                        row["shared_peak_mib"] = 0;
                        row["vram_peak_mib"] = 0;
                        rows.push_back(row);
                        winner_rows.push_back(row);
                        publish(job, "row", {{"ok", false}, {"item", json_value(item, "id", json_value(item, "label", std::string()))}, {"error", e.what()}});
                    }
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->current += 1;
                    }
                }
                json winners = json::object();
                for (const std::string profile : {"speed", "efficiency", "safety", "overall"}) {
                    json grouped = json::object();
                    for (const auto & [model, winner] : caliber::group_winners(winner_rows, caliber::winner_profile_from_string(profile))) grouped[model] = winner;
                    winners[profile] = grouped;
                }
                const bool has_success = std::any_of(rows.begin(), rows.end(), [](const json & row) {
                    return json_value(row, "ok", false);
                });
                json report = {
                    {"id", report_id},
                    {"created_at", isoish_timestamp()},
                    {"status", has_success ? "completed" : "failed"},
                    {"model", model_name},
                    {"note", "Measured with llama-bench through Caliber Advisor."},
                    {"cfg", payload["cfg"]},
                    {"models", payload["models"]},
                    {"plan", plan},
                    {"rows", rows},
                    {"winners", winners},
                };
                const auto report_path = reports_dir() / (report_id + ".json");
                write_text_file(report_path, report.dump(2) + "\n");
                server_persistence::record_report(
                    "caliber-advisor",
                    report_id,
                    "campaign",
                    json_value(report, "status", std::string()),
                    json_value(report, "model", std::string()),
                    report_path,
                    report);
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status = "completed";
                    job->finished = true;
                    job->report_id = report_id;
                }
                publish(job, "report", {{"report_id", report_id}, {"plan_count", payload["plan"].size()}});
                publish(job, "done", {{"report_id", report_id}});
            } catch (const std::exception & e) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status = "failed";
                    job->error = e.what();
                    job->finished = true;
                }
                publish(job, "error", {{"error", e.what()}});
            }
        }).detach();
        res_ok(res, {{"success", true}, {"job_id", job->id}, {"status", "queued"}});
        return res;
    }

    server_http_res_ptr handle_sweep_status(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        const std::string id = req.get_param("id");
        std::shared_ptr<caliber_job> job;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            if (!id.empty() && jobs.count(id)) job = jobs[id];
            else if (!jobs.empty()) job = jobs.rbegin()->second;
        }
        if (!job) {
            res_ok(res, {{"status", "idle"}});
            return res;
        }
        std::lock_guard<std::mutex> lock(job->mutex);
        res_ok(res, {{"job_id", job->id}, {"status", job->status}, {"error", job->error}, {"current", job->current}, {"total", job->total}, {"report_id", job->report_id}, {"finished", job->finished}});
        return res;
    }

    server_http_res_ptr handle_sweep_events(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        const std::string id = req.get_param("id");
        std::shared_ptr<caliber_job> job;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            if (!id.empty() && jobs.count(id)) job = jobs[id];
            else if (!jobs.empty()) job = jobs.rbegin()->second;
        }
        if (!job) {
            res->content_type = "text/event-stream";
            res->data = "event: idle\ndata: {\"status\":\"idle\"}\n\n";
            return res;
        }
        uint64_t since = 0;
        try { since = req.get_param("since").empty() ? 0 : (uint64_t) std::stoull(req.get_param("since")); } catch (...) { since = 0; }
        res->content_type = "text/event-stream";
        res->headers["Cache-Control"] = "no-cache";
        res->headers["X-Accel-Buffering"] = "no";
        res->next = [job, since, &req](std::string & output) mutable {
            std::unique_lock<std::mutex> lock(job->mutex);
            job->cv.wait_for(lock, std::chrono::seconds(2), [&]() {
                if (req.should_stop()) return true;
                for (const auto & ev : job->events) if (ev.seq > since) return true;
                return false;
            });
            if (req.should_stop()) return false;
            for (const auto & ev : job->events) {
                if (ev.seq <= since) continue;
                since = ev.seq;
                output = "event: " + ev.event + "\n";
                output += "data: " + safe_json_to_str(ev.data) + "\n\n";
                return true;
            }
            output = ": keepalive\n\n";
            return true;
        };
        return res;
    }

    server_http_res_ptr handle_reports(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        std::filesystem::create_directories(reports_dir());
        json reports = json::array();
        for (const auto & entry : std::filesystem::directory_iterator(reports_dir())) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            try {
                reports.push_back(report_summary(json::parse(read_text_file(entry.path())), entry.path()));
            } catch (...) {}
        }
        res_ok(res, {{"object", "list"}, {"data", reports}});
        return res;
    }

    server_http_res_ptr handle_report(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        std::string id = req.get_param("id");
        if (id.empty()) id = req.get_param("report_id");
        if (id.empty()) {
            res_err(res, "report id is required");
            return res;
        }
        const auto path = reports_dir() / (slugify(id) + ".json");
        if (!std::filesystem::exists(path)) {
            res_err(res, "report not found", 404);
            return res;
        }
        res_ok(res, normalize_report_for_api(json::parse(read_text_file(path))));
        return res;
    }

    server_http_res_ptr handle_delete_report(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        const std::string id = req.get_param("id");
        if (id.empty()) {
            res_err(res, "report id is required");
            return res;
        }
        const auto path = reports_dir() / (slugify(id) + ".json");
        bool removed = false;
        if (std::filesystem::exists(path)) removed = std::filesystem::remove(path);
        if (removed) server_persistence::delete_report("caliber-advisor", slugify(id));
        res_ok(res, {{"success", removed}, {"id", id}});
        return res;
    }

    server_http_res_ptr handle_results(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        std::filesystem::create_directories(reports_dir());
        json all_rows = json::array();
        json reports = json::array();
        for (const auto & entry : std::filesystem::directory_iterator(reports_dir())) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            try {
                json report = normalize_report_for_api(json::parse(read_text_file(entry.path())));
                reports.push_back(report_summary(report, entry.path()));
                if (report.contains("rows") && report["rows"].is_array()) {
                    for (const auto & row : report["rows"]) all_rows.push_back(row);
                }
            } catch (...) {}
        }
        std::vector<json> rows;
        for (const auto & row : all_rows) rows.push_back(row);
        json winners = json::object();
        for (const std::string profile : {"speed", "efficiency", "safety", "overall"}) {
            json grouped = json::object();
            for (const auto & [model, winner] : caliber::group_winners(rows, caliber::winner_profile_from_string(profile))) grouped[model] = winner;
            winners[profile] = grouped;
        }
        res_ok(res, {{"reports", reports}, {"rows", all_rows}, {"winners", winners}});
        return res;
    }

    server_http_res_ptr handle_configure(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        const std::string model = json_value(body, "model", json_value(body, "model_id", std::string()));
        if (model.empty()) {
            res_err(res, "configure requires model/model_id");
            return res;
        }
        const auto meta = find_router_model(router, model);
        if (!meta) {
            res_err(res, "model not found", 404);
            return res;
        }
        const std::string preset_path = router.params.models_preset;
        if (preset_path.empty() || preset_path.size() < 6 || preset_path.substr(preset_path.size() - 5) != ".json") {
            res_err(res, "Caliber configure requires a JSON --models-preset file", 501);
            return res;
        }

        json root = json::object();
        if (std::filesystem::exists(preset_path)) root = json::parse(read_text_file(preset_path));
        if (!root.is_object()) root = json::object();
        if (!root.contains("version")) root["version"] = 1;
        if (!root.contains("models") || !root["models"].is_array()) root["models"] = json::array();

        json entry = json::object();
        auto existing = find_model_entry(root["models"], model);
        if (existing.has_value()) entry = **existing;
        entry["id"] = model;
        const std::string local_path = model_path_from_meta(*meta);
        const std::string hf_repo = hf_repo_from_meta(*meta);
        if (!local_path.empty()) entry["model"] = local_path;
        else if (!hf_repo.empty()) entry["hf_repo"] = hf_repo;
        if (!entry.contains("alias")) entry["alias"] = meta->name;
        if (!entry.contains("load_on_startup")) entry["load_on_startup"] = false;
        const std::string extra_args = json_value(body, "extra_args", std::string());
        if (!extra_args.empty()) {
            apply_runtime_args(entry, extra_args);
            entry["caliber_args"] = extra_args;
        }
        if (body.contains("tags")) entry["tags"] = body["tags"];

        if (existing.has_value()) **existing = entry;
        else root["models"].push_back(entry);

        write_text_file(preset_path, root.dump(2) + "\n");
        router.models.load_models();
        const bool load_now = json_value(body, "load_now", false);
        if (load_now) router.models.load(model);
        const json response = {{"success", true}, {"model", model}, {"models_preset", preset_path}, {"entry", entry}, {"loaded", load_now}};
        server_persistence::record_configuration("caliber-advisor", model, model, response);
        res_ok(res, response);
        return res;
    }
};

server_caliber_advisor_routes::server_caliber_advisor_routes(server_models_routes & router)
        : pimpl(std::make_shared<impl>(router)) {
    init_routes();
}

server_caliber_advisor_routes::~server_caliber_advisor_routes() = default;

void server_caliber_advisor_routes::init_routes() {
    get_system = [p = pimpl](const server_http_req & req) { return p->handle_system(req); };
    get_models = [p = pimpl](const server_http_req & req) { return p->handle_models(req); };
    post_plan = [p = pimpl](const server_http_req & req) { return p->handle_plan(req); };
    post_sweep = [p = pimpl](const server_http_req & req) { return p->handle_sweep(req); };
    get_sweep_events = [p = pimpl](const server_http_req & req) { return p->handle_sweep_events(req); };
    get_sweep_status = [p = pimpl](const server_http_req & req) { return p->handle_sweep_status(req); };
    get_results = [p = pimpl](const server_http_req & req) { return p->handle_results(req); };
    get_reports = [p = pimpl](const server_http_req & req) { return p->handle_reports(req); };
    get_report = [p = pimpl](const server_http_req & req) { return p->handle_report(req); };
    delete_report = [p = pimpl](const server_http_req & req) { return p->handle_delete_report(req); };
    post_configure = [p = pimpl](const server_http_req & req) { return p->handle_configure(req); };
}
