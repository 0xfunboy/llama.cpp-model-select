#include "server-caliber-advisor.h"

#include "caliber-plan.h"
#include "caliber-scoring.h"
#include "server-common.h"
#include "server-models.h"
#include "server-persistence.h"

#include <sheredom/subprocess.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <atomic>
#include <unistd.h>

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
    std::string current_item;
    int current = 0;
    int total = 0;
    bool finished = false;
    bool cancel_requested = false;
    uint64_t next_seq = 1;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<caliber_event> events;
};

static bool caliber_job_status_live(const std::string & status) {
    return status == "queued" || status == "running" || status == "stopping";
}

static std::filesystem::path reports_dir() {
    const auto target = server_persistence::state_dir() / "reports" / "caliber";
    static std::once_flag migrate_once;
    std::call_once(migrate_once, [&]() {
        std::filesystem::create_directories(target);
        const auto legacy = std::filesystem::current_path() / "tools" / "ui" / "static" / "reports" / "caliber";
        std::error_code ec;
        if (!std::filesystem::exists(legacy, ec)) return;
        for (const auto & entry : std::filesystem::directory_iterator(legacy, ec)) {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
            const auto destination = target / entry.path().filename();
            if (!std::filesystem::exists(destination, ec)) std::filesystem::copy_file(entry.path(), destination, ec);
        }
    });
    return target;
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
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to write " + path.string());
    out << text;
    out.close();
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) throw std::runtime_error("failed to atomically replace " + path.string());
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

static std::string run_command_capture(const std::filesystem::path & executable, const std::vector<std::string> & args, int & exit_code) {
    std::vector<std::string> command = {executable.string()};
    command.insert(command.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto & value : command) argv.push_back(value.data());
    argv.push_back(nullptr);
    subprocess_s process{};
    const int options = subprocess_option_no_window | subprocess_option_combined_stdout_stderr | subprocess_option_inherit_environment;
    if (subprocess_create(argv.data(), options, &process) != 0) throw std::runtime_error("failed to start benchmark process");

    std::atomic<bool> done{false};
    std::thread timeout([&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(2);
        while (!done.load()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                subprocess_terminate(&process);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::array<char, 4096> buffer{};
    std::string output;
    FILE * pipe = subprocess_stdout(&process);
    while (fgets(buffer.data(), (int) buffer.size(), pipe) != nullptr) output += buffer.data();
    done.store(true);
    timeout.join();
    subprocess_join(&process, &exit_code);
    subprocess_destroy(&process);
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

static json detected_build_capabilities() {
    static std::once_flag once;
    static json capabilities = json::object();
    std::call_once(once, []() {
        int exit_code = 0;
        const std::string help = run_command_capture(current_executable_dir() / "llama-bench", {"--help"}, exit_code);
        std::set<std::string> flags;
        const std::regex flag_pattern(R"((--[a-zA-Z0-9][a-zA-Z0-9-]*))");
        for (std::sregex_iterator it(help.begin(), help.end(), flag_pattern), end; it != end; ++it) flags.insert((*it)[1].str());
        capabilities = {
            {"source", "llama-bench-help"},
            {"exit_code", exit_code},
            {"supported_flags", flags},
        };
    });
    return capabilities;
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
    int filled_context = 0;
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
    dims.filled_context = dims.prompt_tokens + dims.depth_tokens;
    dims.allocated_context = dims.filled_context + dims.generate_tokens;
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
        "-r", std::to_string(std::max(1, json_value(json_value(cfg, "bench", json::object()), "repetitions", 3))),
        "-p", std::to_string(dims.prompt_tokens),
        "-n", std::to_string(dims.generate_tokens),
    };
    if (!json_value(json_value(cfg, "bench", json::object()), "warmup", true)) {
        out.push_back("--no-warmup");
    }
    if (dims.depth_tokens > 0) {
        out.push_back("-d");
        out.push_back(std::to_string(dims.depth_tokens));
    }

    auto flag_supported = [&](const std::string & flag) {
        const json capabilities = json_value(cfg, "capabilities", json::object());
        if (!capabilities.contains("supported_flags") || !capabilities["supported_flags"].is_array() || capabilities["supported_flags"].empty()) return true;
        return std::any_of(capabilities["supported_flags"].begin(), capabilities["supported_flags"].end(), [&](const json & value) {
            return value.is_string() && value.get<std::string>() == flag;
        });
    };
    auto push_value = [&](const std::string & dst, const std::vector<std::string> & names, bool tensor_split = false) {
        std::string value = arg_value(source, names);
        if (value.empty() || !flag_supported(dst)) return;
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

static std::vector<double> bench_samples(const json & row) {
    std::vector<double> out;
    if (row.is_object() && row.contains("samples_ts") && row["samples_ts"].is_array()) {
        for (const auto & value : row["samples_ts"]) {
            if (value.is_number()) out.push_back(value.get<double>());
        }
    }
    if (out.empty() && row.is_object()) {
        const double avg = json_value(row, "avg_ts", 0.0);
        if (avg > 0) out.push_back(avg);
    }
    return out;
}

static json sanitized_bench_args(const std::vector<std::string> & args) {
    json out = json::array();
    bool redact_next = false;
    for (const auto & arg : args) {
        if (redact_next) {
            out.push_back("<model>");
            redact_next = false;
            continue;
        }
        out.push_back(arg);
        if (arg == "-m" || arg == "--model") redact_next = true;
    }
    return out;
}

static json run_llama_bench_item(const json & item, const json & cfg) {
    llama_bench_dims dims;
    const auto args = llama_bench_args_for_item(item, cfg, dims);
    int exit_code = 0;
    const std::string output = run_command_capture(current_executable_dir() / "llama-bench", args, exit_code);
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
    const auto prompt_samples = bench_samples(prompt_row);
    const auto eval_samples = bench_samples(gen_row);
    const size_t sample_count = std::max<size_t>(1, std::max(prompt_samples.size(), eval_samples.size()));
    std::vector<json> runs;
    runs.reserve(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        json run = json::object();
        run["run_index"] = i;
        run["ok"] = true;
        run["eval_tps"] = i < eval_samples.size() ? eval_samples[i] : (eval_samples.empty() ? 0.0 : eval_samples.back());
        run["prompt_tps"] = i < prompt_samples.size() ? prompt_samples[i] : (prompt_samples.empty() ? 0.0 : prompt_samples.back());
        run["prompt_n"] = prompt_row.is_object() ? json_value(prompt_row, "n_prompt", dims.prompt_tokens) : dims.prompt_tokens;
        run["eval_n"] = gen_row.is_object() ? json_value(gen_row, "n_gen", dims.generate_tokens) : 0;
        run["n_prompt"] = dims.prompt_tokens;
        run["n_gen"] = dims.generate_tokens;
        run["n_depth"] = dims.depth_tokens;
        run["ctx_size"] = dims.allocated_context;
        run["measured_context_size"] = dims.allocated_context;
        run["requested_context_size"] = dims.requested_context > 0 ? json(dims.requested_context) : json(nullptr);
        run["context_target_met"] = dims.requested_context <= 0 || dims.allocated_context >= dims.requested_context;
        run["context_measurement_kind"] = "synthetic-test-shape";
        run["benchmark_allocated_context_size"] = dims.allocated_context;
        run["benchmark_filled_context_size"] = dims.filled_context;
        run["benchmark_depth_tokens"] = dims.depth_tokens;
        run["benchmark_prompt_tokens"] = dims.prompt_tokens;
        run["benchmark_generate_tokens"] = dims.generate_tokens;
        run["benchmark_note"] = "llama-bench synthetic benchmark; excludes tokenization and sampling; context is the executed test shape";
        run["memory_measurement_kind"] = "unavailable";
        run["benchmark_backend"] = "llama-bench";
        runs.push_back(std::move(run));
    }
    json row = caliber::aggregate_bench_result(item, cfg, runs);
    row["benchmark_backend"] = "llama-bench";
    row["benchmark_rows"] = parsed;
    if (prompt_row.is_object()) row["llama_bench_prompt_row"] = prompt_row;
    if (gen_row.is_object()) row["llama_bench_generation_row"] = gen_row;
    row["bench_executable"] = "llama-bench";
    row["bench_args"] = sanitized_bench_args(args);
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

static json default_plan_cfg() {
    return {
        {"hardware", {
            {"backend", "auto"},
            {"vram_budget_mib", 0},
            {"vram_driver_usable_mib", 0},
            {"gpus", json::array()},
            {"cpu_cores_physical", (int) std::max(1u, std::thread::hardware_concurrency() / 2)},
            {"cpu_threads_logical", (int) std::max(1u, std::thread::hardware_concurrency())},
            {"system_ram_available_mib", 0},
        }},
        {"planning", {
            {"overhead_mib", 1200},
            {"per_gpu_headroom_mib", 512},
            {"search", "adaptive"},
            {"kv_rescue", {{"enabled", true}, {"min_context_tokens", 65536}, {"kv_k", "q4_0"}, {"kv_v", "q4_0"}}},
            {"workload_sweeps", {{"context_reserve_tokens", 512}, {"kv_fill_ratios", {0.25, 0.5, 0.75, 0.9}}, {"prefill_micro_tokens", {2048}}, {"prefill_ratios", {0.25, 0.9}}}},
        }},
        {"bench", {{"n_predict", 128}, {"repetitions", 3}, {"warmup", true}}},
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
    const std::string report_id = json_value(report, "id", std::string());
    for (auto & row : report["rows"]) {
        if (!row.is_object()) continue;
        if (!report_id.empty()) row["report_id"] = report_id;
        if (json_value(row, "benchmark_backend", std::string()) == "llama-bench") {
            const int allocated = json_value(row, "benchmark_allocated_context_size", 0);
            const int requested = json_value(row, "requested_context_size", 0);
            const int reported = json_value(row, "ctx_size", 0);
            if (allocated > 0) {
                if (reported > 0 && reported != allocated) row["legacy_reported_context_size"] = reported;
                row["ctx_size"] = allocated;
                row["measured_context_size"] = allocated;
                row["context_target_met"] = requested <= 0 || allocated >= requested;
                row["context_measurement_kind"] = "synthetic-test-shape";
            }
            row["memory_measurement_kind"] = "unavailable";
            row["evidence_level"] = "synthetic-measured";
            row["fit_eligible"] = false;
            if (json_value(row, "run_count", 0) < 2 && json_value(row, "measurement_confidence", std::string()) == "reliable") {
                row["measurement_confidence"] = "provisional";
            }
        }
        if (is_legacy_invalid_llama_bench_row(row)) {
            row["ok"] = false;
            row["eval_tps"] = 0;
            row["measurement_confidence"] = "invalid";
            row["failure_reason"] = "legacy_invalid_llama_bench_prompt_row";
            row["recommendation"] = "rerun-required";
            row["recommendation_reason"] = "This row was produced before llama-bench prompt/generation rows were separated; rerun the benchmark.";
            ++invalid;
        }
    }
    if (invalid > 0) {
        report["legacy_invalid_rows"] = invalid;
        report["status_note"] = "Contains legacy invalid llama-bench rows; rerun required for comparison/FIT.";
    }
    std::vector<json> rows;
    for (const auto & row : report["rows"]) {
        if (row.is_object()) rows.push_back(row);
    }
    report["recommendations"] = caliber::build_recommendations(rows);
    report["recommendation_policy"] = {
        {"id", "caliber-recommendation"},
        {"version", caliber::RECOMMENDATION_POLICY_VERSION},
    };
    return report;
}

} // namespace

struct server_caliber_advisor_routes::impl {
    server_models_routes & router;
    std::mutex jobs_mutex;
    std::map<std::string, std::shared_ptr<caliber_job>> jobs;

    explicit impl(server_models_routes & router) : router(router) {}

    json snapshot_locked(const std::shared_ptr<caliber_job> & job) {
        return {
            {"job_id", job->id},
            {"status", job->status},
            {"error", job->error.empty() ? nullptr : json(job->error)},
            {"current", job->current},
            {"total", job->total},
            {"current_item", job->current_item.empty() ? nullptr : json(job->current_item)},
            {"report_id", job->report_id.empty() ? nullptr : json(job->report_id)},
            {"finished", job->finished},
            {"cancel_requested", job->cancel_requested},
        };
    }

    json snapshot(const std::shared_ptr<caliber_job> & job) {
        std::lock_guard<std::mutex> lock(job->mutex);
        return snapshot_locked(job);
    }

    std::shared_ptr<caliber_job> find_job(const std::string & id) {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        if (!id.empty()) {
            auto it = jobs.find(id);
            return it == jobs.end() ? nullptr : it->second;
        }
        std::shared_ptr<caliber_job> latest;
        for (const auto & [_, job] : jobs) {
            std::lock_guard<std::mutex> job_lock(job->mutex);
            if (caliber_job_status_live(job->status) && !job->finished) {
                return job;
            }
            latest = job;
        }
        return latest;
    }

    void publish(const std::shared_ptr<caliber_job> & job, const std::string & event, json data) {
        std::lock_guard<std::mutex> lock(job->mutex);
        data["job_id"] = job->id;
        data["status"] = job->status;
        data["current"] = job->current;
        data["total"] = job->total;
        data["current_item"] = job->current_item.empty() ? nullptr : json(job->current_item);
        data["report_id"] = job->report_id.empty() ? nullptr : json(job->report_id);
        data["finished"] = job->finished;
        data["cancel_requested"] = job->cancel_requested;
        data["ts"] = isoish_timestamp();
        caliber_event ev;
        ev.seq = job->next_seq++;
        ev.event = event;
        ev.data = std::move(data);
        ev.data["seq"] = ev.seq;
        job->events.push_back(std::move(ev));
        while (job->events.size() > 200) job->events.pop_front();
        server_persistence::record_job("caliber-advisor", job->id, job->status, job->events.back().data);
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
        const json registry = router.scan_model_registry(false);
        if (!registry.contains("artifacts") || !registry["artifacts"].is_array()) return metas;
        for (const auto & artifact : registry["artifacts"]) {
            if (!artifact.value("loadable", false)) continue;
            const std::string artifact_id = json_value(artifact, "artifact_id", std::string());
            bool configured_match = false;
            if (artifact.contains("configured_ids") && artifact["configured_ids"].is_array()) {
                for (const auto & id : artifact["configured_ids"]) {
                    if (id.is_string() && (id.get<std::string>() == requested || requested_models.count(id.get<std::string>()))) configured_match = true;
                }
            }
            if (!requested.empty() && requested != artifact_id && !configured_match) continue;
            if (!requested_models.empty() && !requested_models.count(artifact_id) && !configured_match) continue;
            json meta = json_value(artifact, "metadata", json::object());
            meta["path"] = json_value(artifact, "primary_path", std::string());
            meta["artifact_id"] = artifact_id;
            meta["model_id"] = json_value(artifact, "model_id", std::string());
            meta["preset_id"] = json_value(artifact, "preset_id", std::string());
            meta["mmproj"] = artifact.value("mmproj_path", json(nullptr));
            metas.push_back(std::move(meta));
        }
        return metas;
    }

    json build_plan_payload(const json & body) {
        const auto metas = model_plan_metas(body);
        json cfg = merge_cfg(default_plan_cfg(), json_value(body, "cfg", json::object()));
        if (!cfg.contains("capabilities")) cfg["capabilities"] = detected_build_capabilities();
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
        const json registry = router.scan_model_registry(!req.get_param("reload").empty());
        if (registry.contains("artifacts") && registry["artifacts"].is_array()) {
            for (const auto & artifact : registry["artifacts"]) {
                const bool loadable = artifact.value("loadable", false);
                models.push_back({
                    {"id", json_value(artifact, "artifact_id", std::string())},
                    {"artifact_id", json_value(artifact, "artifact_id", std::string())},
                    {"model_id", json_value(artifact, "model_id", std::string())},
                    {"preset_id", json_value(artifact, "preset_id", std::string())},
                    {"name", json_value(artifact, "name", std::string())},
                    {"source", "registry"},
                    {"status", json_value(artifact, "health", std::string())},
                    {"loadable", loadable},
                    {"configured", artifact.value("configured", false)},
                    {"path", loadable ? artifact.value("primary_path", json(nullptr)) : json(nullptr)},
                    {"plan_meta", artifact.value("metadata", json::object())},
                    {"missing_shards", artifact.value("missing_shards", json::array())},
                    {"duplicate_of", artifact.value("duplicate_of", json(nullptr))},
                    {"redundant_quantization", artifact.value("redundant_quantization", false)},
                });
            }
        }
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
        if (!require_admin_api_key(req, router.params, res)) return res;
        const json body = req.body.empty() ? json::object() : json::parse(req.body);
        auto job = std::make_shared<caliber_job>();
        job->id = slugify("caliber-" + isoish_timestamp() + "-" + std::to_string(jobs.size() + 1));
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            jobs[job->id] = job;
        }
        publish(job, "queued", json::object());
        std::thread([this, job, body]() {
            std::vector<std::string> restore_models;
            const bool restore_after = json_value(body, "restore_active_model", true);
            try {
                auto cancel_requested = [&job]() {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    return job->cancel_requested;
                };
                if (cancel_requested()) {
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->status = "cancelled";
                        job->finished = true;
                    }
                    publish(job, "cancelled", {{"message", "Campaign cancelled before start"}});
                    return;
                }
                auto operation = router.operations.acquire("caliber", 40, cancel_requested, std::chrono::hours(24));
                if (!operation) throw std::runtime_error("campaign cancelled while waiting for inference resources");
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status = "running";
                }
                publish(job, "started", json::object());
                publish(job, "preflight", {{"message", "Unloading active router models before benchmark"}});
                if (restore_after) {
                    for (const auto & meta : router.models.get_all_meta()) {
                        if (meta.is_running()) restore_models.push_back(meta.name);
                    }
                }
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
                bool cancelled = false;
                for (const auto & item : plan) {
                    const std::string item_id = json_value(item, "id", json_value(item, "label", std::string()));
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        if (job->cancel_requested) {
                            cancelled = true;
                            break;
                        }
                        job->current_item = item_id;
                    }
                    publish(job, "bench", {{"item", item_id}});
                    try {
                        json row = run_llama_bench_item(item, payload["cfg"]);
                        rows.push_back(row);
                        winner_rows.push_back(row);
                        publish(job, "row", {{"ok", true}, {"item", item_id}, {"eval_tps", json_value(row, "eval_tps", 0.0)}});
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
                        row["memory_measurement_kind"] = "unavailable";
                        row["evidence_level"] = "failed";
                        row["fit_eligible"] = false;
                        rows.push_back(row);
                        winner_rows.push_back(row);
                        publish(job, "row", {{"ok", false}, {"item", item_id}, {"error", e.what()}});
                    }
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->current += 1;
                        job->current_item.clear();
                        if (job->cancel_requested) {
                            cancelled = true;
                        }
                    }
                    if (cancelled) {
                        break;
                    }
                }
                const json hardware = json_value(payload["cfg"], "hardware", json::object());
                const double vram_total_mib = json_value(
                    hardware,
                    "vram_total_mib",
                    json_value(hardware, "vram_budget_mib", 0.0));
                caliber::memory_policy_options memory_options;
                memory_options.vram_budget_mib = json_value(hardware, "vram_budget_mib", -1.0);
                memory_options.vram_driver_usable_mib = json_value(hardware, "vram_driver_usable_mib", -1.0);
                winner_rows = caliber::build_report_rows(winner_rows, vram_total_mib, memory_options);
                rows = json::array();
                for (const auto & row : winner_rows) rows.push_back(row);

                const json recommendations = caliber::build_recommendations(winner_rows);
                const bool has_success = std::any_of(rows.begin(), rows.end(), [](const json & row) {
                    return json_value(row, "ok", false);
                });
                const std::string report_status = cancelled ? "cancelled" : (has_success ? "completed" : "failed");
                json report = {
                    {"id", report_id},
                    {"created_at", isoish_timestamp()},
                    {"status", report_status},
                    {"model", model_name},
                    {"note", "Measured with llama-bench through Caliber Advisor."},
                    {"metric_schema_version", caliber::METRIC_SCHEMA_VERSION},
                    {"evidence_level", "synthetic-measured"},
                    {"automatic_fit_allowed", false},
                    {"cfg", payload["cfg"]},
                    {"models", payload["models"]},
                    {"plan", plan},
                    {"rows", rows},
                    {"recommendations", recommendations},
                    {"recommendation_policy", {{"id", "caliber-recommendation"}, {"version", caliber::RECOMMENDATION_POLICY_VERSION}}},
                };
                const auto report_path = reports_dir() / (report_id + ".json");
                server_persistence::record_report(
                    "caliber-advisor",
                    report_id,
                    "campaign",
                    json_value(report, "status", std::string()),
                    json_value(report, "model", std::string()),
                    report_path,
                    report);
                write_text_file(report_path, report.dump(2) + "\n");
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status = cancelled ? "cancelled" : "completed";
                    job->finished = true;
                    job->report_id = report_id;
                    job->current_item.clear();
                }
                publish(job, "report", {{"report_id", report_id}, {"plan_count", payload["plan"].size()}});
                publish(job, cancelled ? "cancelled" : "done", {{"report_id", report_id}});
                for (const auto & name : restore_models) {
                    try { router.models.load(name); } catch (const std::exception & e) { publish(job, "restore-warning", {{"model", name}, {"error", e.what()}}); }
                }
                restore_models.clear();
            } catch (const std::exception & e) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status = "failed";
                    job->error = e.what();
                    job->finished = true;
                    job->current_item.clear();
                }
                publish(job, "error", {{"error", e.what()}});
            }
            for (const auto & name : restore_models) {
                try { router.models.load(name); } catch (...) {}
            }
        }).detach();
        res_ok(res, {{"success", true}, {"job_id", job->id}, {"status", "queued"}});
        return res;
    }

    server_http_res_ptr handle_sweep_status(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        auto job = find_job(req.get_param("id"));
        if (!job) {
            res_ok(res, {{"status", "idle"}});
            return res;
        }
        res_ok(res, snapshot(job));
        return res;
    }

    server_http_res_ptr handle_sweep_stop(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        if (!require_admin_api_key(req, router.params, res)) return res;
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        const std::string id = json_value(body, "id", req.get_param("id"));
        auto job = find_job(id);
        if (!job) {
            res_ok(res, {{"status", "idle"}});
            return res;
        }
        bool requested = false;
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            if (!job->finished) {
                job->cancel_requested = true;
                if (caliber_job_status_live(job->status)) {
                    job->status = "stopping";
                }
                requested = true;
            }
        }
        if (requested) {
            publish(job, "stop", {{"message", "Stop requested; the current benchmark config may finish before the campaign exits"}});
        }
        res_ok(res, snapshot(job));
        return res;
    }

    server_http_res_ptr handle_sweep_events(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        auto job = find_job(req.get_param("id"));
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

    server_http_res_ptr handle_reports(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        std::vector<json> report_items;
        for (const auto & report : server_persistence::load_reports("caliber-advisor")) {
            if (!report.is_object()) continue;
            report_items.push_back(report_summary(report, {}));
        }
        std::sort(report_items.begin(), report_items.end(), [](const json & a, const json & b) {
            return json_value(a, "created_at", std::string()) > json_value(b, "created_at", std::string());
        });
        const int offset = std::max(0, std::atoi(req.get_param("offset", "0").c_str()));
        const int limit = std::clamp(std::atoi(req.get_param("limit", "50").c_str()), 1, 200);
        json reports = json::array();
        const size_t begin = std::min(report_items.size(), (size_t) offset);
        const size_t end = std::min(report_items.size(), begin + (size_t) limit);
        for (size_t i = begin; i < end; ++i) reports.push_back(report_items[i]);
        res_ok(res, {
            {"object", "list"},
            {"data", reports},
            {"offset", offset},
            {"limit", limit},
            {"total", report_items.size()},
            {"has_more", end < report_items.size()},
        });
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
        const json report = server_persistence::load_report("caliber-advisor", slugify(id));
        if (!report.is_object()) {
            res_err(res, "report not found", 404);
            return res;
        }
        res_ok(res, normalize_report_for_api(report));
        return res;
    }

    server_http_res_ptr handle_delete_report(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        if (!require_admin_api_key(req, router.params, res)) return res;
        const std::string id = req.get_param("id");
        if (id.empty()) {
            res_err(res, "report id is required");
            return res;
        }
        const auto path = reports_dir() / (slugify(id) + ".json");
        const bool removed = server_persistence::load_report("caliber-advisor", slugify(id)).is_object();
        if (removed) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            server_persistence::delete_report("caliber-advisor", slugify(id));
        }
        res_ok(res, {{"success", removed}, {"id", id}});
        return res;
    }

    server_http_res_ptr handle_results(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        json all_rows = json::array();
        json reports = json::array();
        std::vector<json> latest_rows;
        std::string latest_created_at;
        std::string latest_report_id;
        for (const auto & stored_report : server_persistence::load_reports("caliber-advisor")) {
            try {
                json report = normalize_report_for_api(stored_report);
                reports.push_back(report_summary(report, {}));
                if (report.contains("rows") && report["rows"].is_array()) {
                    for (const auto & row : report["rows"]) all_rows.push_back(row);
                    const std::string created_at = json_value(report, "created_at", std::string());
                    if (created_at >= latest_created_at) {
                        latest_created_at = created_at;
                        latest_report_id = json_value(report, "id", std::string());
                        latest_rows.clear();
                        for (const auto & row : report["rows"]) {
                            if (row.is_object()) latest_rows.push_back(row);
                        }
                    }
                }
            } catch (...) {}
        }
        std::vector<json> rows;
        for (const auto & row : all_rows) rows.push_back(row);
        const json recommendations = caliber::build_recommendations(rows);
        const json latest_recommendations = caliber::build_recommendations(latest_rows);
        res_ok(res, {
            {"scope", "compatible-history"},
            {"reports", reports},
            {"rows", all_rows},
            {"recommendations", recommendations},
            {"recommendation_policy", {{"id", "caliber-recommendation"}, {"version", caliber::RECOMMENDATION_POLICY_VERSION}}},
            {"scopes", {
                {"compatible_history", {
                    {"scope", "compatible-history"},
                    {"recommendations", recommendations},
                }},
                {"latest_campaign", {
                    {"scope", "latest-campaign"},
                    {"report_id", latest_report_id.empty() ? json(nullptr) : json(latest_report_id)},
                    {"created_at", latest_created_at.empty() ? json(nullptr) : json(latest_created_at)},
                    {"recommendations", latest_recommendations},
                }},
            }},
        });
        return res;
    }

    server_http_res_ptr handle_configure(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        if (!require_admin_api_key(req, router.params, res)) return res;
        auto operation = router.operations.acquire("caliber-configure", 70, req.should_stop, std::chrono::seconds(30));
        if (!operation) {
            res_err(res, "inference resources are busy", 503);
            return res;
        }
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        const std::string model = json_value(body, "model", json_value(body, "model_id", std::string()));
        const std::string report_id = json_value(body, "report_id", std::string());
        const std::string row_id = json_value(body, "row_id", std::string());
        if (model.empty()) {
            res_err(res, "configure requires model/model_id");
            return res;
        }
        const json report = server_persistence::load_report("caliber-advisor", report_id);
        json measured_row = nullptr;
        if (report.is_object() && report.contains("rows") && report["rows"].is_array()) {
            for (const auto & row : report["rows"]) {
                if (row.is_object() && json_value(row, "id", std::string()) == row_id &&
                    json_value(row, "model", std::string()) == model) {
                    measured_row = row;
                    break;
                }
            }
        }
        if (!measured_row.is_object() || !json_value(measured_row, "fit_eligible", false) ||
            !json_value(measured_row, "context_target_met", false)) {
            res_err(res, "configure requires a FIT-eligible row from a stored decision-grade report", 403);
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

        std::lock_guard<std::mutex> preset_lock(router.preset_mutex);
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
        const std::string extra_args = json_value(measured_row, "extra_args", std::string());
        if (!extra_args.empty()) {
            apply_runtime_args(entry, extra_args);
            entry["caliber_evidence"] = {{"report_id", report_id}, {"row_id", row_id}};
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
    post_sweep_stop = [p = pimpl](const server_http_req & req) { return p->handle_sweep_stop(req); };
    get_sweep_events = [p = pimpl](const server_http_req & req) { return p->handle_sweep_events(req); };
    get_sweep_status = [p = pimpl](const server_http_req & req) { return p->handle_sweep_status(req); };
    get_results = [p = pimpl](const server_http_req & req) { return p->handle_results(req); };
    get_reports = [p = pimpl](const server_http_req & req) { return p->handle_reports(req); };
    get_report = [p = pimpl](const server_http_req & req) { return p->handle_report(req); };
    delete_report = [p = pimpl](const server_http_req & req) { return p->handle_delete_report(req); };
    post_configure = [p = pimpl](const server_http_req & req) { return p->handle_configure(req); };
}
