#include "server-fit-advisor.h"

#include "common.h"
#include "download.h"
#include "server-common.h"
#include "server-models.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
#include <sys/sysinfo.h>
#endif

namespace {

static constexpr const char * FIT_CATALOG_URL =
    "https://raw.githubusercontent.com/AlexsJones/llmfit/main/llmfit-core/data/hf_models.json";

static void fit_res_ok(std::unique_ptr<server_http_res> & res, const json & response_data) {
    res->status = 200;
    res->data = safe_json_to_str(response_data);
}

static void fit_res_err(std::unique_ptr<server_http_res> & res, const json & error_data) {
    res->status = json_value(error_data, "code", 500);
    res->data = safe_json_to_str({{"error", error_data}});
}

static std::string trim_copy(const std::string & in) {
    size_t start = 0;
    while (start < in.size() && std::isspace((unsigned char) in[start])) {
        start++;
    }
    size_t end = in.size();
    while (end > start && std::isspace((unsigned char) in[end - 1])) {
        end--;
    }
    return in.substr(start, end - start);
}

static std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
    });
    return text;
}

static bool contains_ci(const std::string & haystack, const std::string & needle) {
    if (needle.empty()) {
        return true;
    }
    return lower_copy(haystack).find(lower_copy(needle)) != std::string::npos;
}

static std::string isoish_timestamp() {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static std::filesystem::path cache_dir() {
    return std::filesystem::path(fs_get_cache_directory()) / "fit-advisor";
}

static std::filesystem::path catalog_cache_path() {
    return cache_dir() / "hf_models.json";
}

static std::filesystem::path catalog_status_path() {
    return cache_dir() / "catalog-status.json";
}

static std::string read_text_file(const std::filesystem::path & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(string_format("cannot open '%s'", path.string().c_str()));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_text_file(const std::filesystem::path & path, const std::string & text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error(string_format("cannot write '%s'", path.string().c_str()));
    }
    out << text;
}

static std::vector<std::string> shell_lines(const std::string & command) {
    std::vector<std::string> lines;
#if defined(_WIN32)
    FILE * pipe = _popen(command.c_str(), "r");
#else
    FILE * pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        return lines;
    }
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line = trim_copy(buffer);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return lines;
}

static std::string detect_cpu_name() {
#if defined(__linux__)
    try {
        std::ifstream in("/proc/cpuinfo");
        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find(':');
            if (pos == std::string::npos) {
                continue;
            }
            std::string key = trim_copy(line.substr(0, pos));
            if (key == "model name" || key == "Hardware") {
                return trim_copy(line.substr(pos + 1));
            }
        }
    } catch (...) {
    }
#endif
    return "unknown";
}

static double mib_to_gib(double mib) {
    return mib / 1024.0;
}

static double bytes_to_gib(double bytes) {
    return bytes / 1073741824.0;
}

static json detect_system_json() {
    double total_ram_gb = 0.0;
    double available_ram_gb = 0.0;
#if defined(__linux__)
    struct sysinfo si {};
    if (sysinfo(&si) == 0) {
        total_ram_gb = bytes_to_gib((double) si.totalram * (double) si.mem_unit);
        available_ram_gb = bytes_to_gib((double) si.freeram * (double) si.mem_unit);
        available_ram_gb += bytes_to_gib((double) si.bufferram * (double) si.mem_unit);
    }
#endif
    if (total_ram_gb <= 0.0) {
        total_ram_gb = 0.0;
    }

    json gpus = json::array();
    std::vector<double> vram_gb;
    std::string primary_gpu;
    for (const auto & line : shell_lines("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null")) {
        auto pos = line.rfind(',');
        if (pos == std::string::npos) {
            continue;
        }
        std::string name = trim_copy(line.substr(0, pos));
        std::string mem = trim_copy(line.substr(pos + 1));
        double gb = 0.0;
        try {
            gb = mib_to_gib(std::stod(mem));
        } catch (...) {
            gb = 0.0;
        }
        if (name.empty()) {
            name = "NVIDIA GPU";
        }
        if (primary_gpu.empty()) {
            primary_gpu = name;
        }
        vram_gb.push_back(gb);
        gpus.push_back({
            {"name", name},
            {"vram_gb", gb},
            {"backend", "CUDA"},
            {"unified_memory", false},
        });
    }

    double total_gpu_vram_gb = 0.0;
    double max_gpu_vram_gb = 0.0;
    for (double gb : vram_gb) {
        total_gpu_vram_gb += gb;
        max_gpu_vram_gb = std::max(max_gpu_vram_gb, gb);
    }

    return {
        {"cpu_name", detect_cpu_name()},
        {"cpu_cores", (int) std::max(1u, std::thread::hardware_concurrency())},
        {"total_ram_gb", total_ram_gb},
        {"available_ram_gb", available_ram_gb > 0.0 ? available_ram_gb : total_ram_gb},
        {"has_gpu", !vram_gb.empty()},
        {"gpu_name", primary_gpu},
        {"gpu_count", (int) vram_gb.size()},
        {"gpu_vram_gb", max_gpu_vram_gb},
        {"total_gpu_vram_gb", total_gpu_vram_gb},
        {"backend", vram_gb.empty() ? "CPU" : "CUDA"},
        {"unified_memory", false},
        {"gpus", gpus},
    };
}

static double quant_bpp(const std::string & quant) {
    std::string q = quant;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    if (q == "F32") return 4.0;
    if (q == "F16" || q == "BF16") return 2.0;
    if (q == "Q8_0" || q.find("Q8") != std::string::npos) return 1.05;
    if (q == "Q6_K" || q.find("Q6") != std::string::npos) return 0.80;
    if (q == "Q5_K_M" || q.find("Q5") != std::string::npos) return 0.68;
    if (q == "Q4_K_M" || q == "Q4_0" || q.find("Q4") != std::string::npos) return 0.58;
    if (q == "Q3_K_M" || q.find("Q3") != std::string::npos) return 0.48;
    if (q == "Q2_K" || q.find("Q2") != std::string::npos) return 0.37;
    if (q.find("4BIT") != std::string::npos) return 0.50;
    if (q.find("8BIT") != std::string::npos) return 1.0;
    return 0.58;
}

static double quant_speed_multiplier(const std::string & quant) {
    std::string q = quant;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    if (q == "F16" || q == "BF16") return 0.6;
    if (q == "Q8_0" || q.find("Q8") != std::string::npos) return 0.8;
    if (q == "Q6_K" || q.find("Q6") != std::string::npos) return 0.95;
    if (q == "Q5_K_M" || q.find("Q5") != std::string::npos) return 1.0;
    if (q == "Q4_K_M" || q == "Q4_0" || q.find("Q4") != std::string::npos) return 1.15;
    if (q == "Q3_K_M" || q.find("Q3") != std::string::npos) return 1.25;
    if (q == "Q2_K" || q.find("Q2") != std::string::npos) return 1.35;
    return 1.0;
}

static double quant_quality_penalty(const std::string & quant) {
    std::string q = quant;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    if (q == "F16" || q == "BF16" || q == "Q8_0" || q.find("Q8") != std::string::npos) return 0.0;
    if (q == "Q6_K" || q.find("Q6") != std::string::npos) return -1.0;
    if (q == "Q5_K_M" || q.find("Q5") != std::string::npos) return -2.0;
    if (q == "Q4_K_M" || q == "Q4_0" || q.find("Q4") != std::string::npos) return -5.0;
    if (q == "Q3_K_M" || q.find("Q3") != std::string::npos) return -8.0;
    if (q == "Q2_K" || q.find("Q2") != std::string::npos) return -12.0;
    return -5.0;
}

static double kv_bytes_per_element(const std::string & kv_quant) {
    std::string q = lower_copy(kv_quant);
    if (q == "fp8" || q == "q8_0" || q == "q8" || q == "int8") return 1.0;
    if (q == "q4_0" || q == "q4" || q == "int4") return 0.5;
    return 2.0;
}

static double params_b_from_json(const json & model) {
    if (model.contains("parameters_raw") && model["parameters_raw"].is_number()) {
        return model["parameters_raw"].get<double>() / 1000000000.0;
    }
    std::string s = json_value(model, "parameter_count", std::string("7B"));
    s = trim_copy(s);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    try {
        if (!s.empty() && s.back() == 'T') return std::stod(s.substr(0, s.size() - 1)) * 1000.0;
        if (!s.empty() && s.back() == 'B') return std::stod(s.substr(0, s.size() - 1));
        if (!s.empty() && s.back() == 'M') return std::stod(s.substr(0, s.size() - 1)) / 1000.0;
        if (!s.empty() && s.back() == 'K') return std::stod(s.substr(0, s.size() - 1)) / 1000000.0;
        return std::stod(s);
    } catch (...) {
        return 7.0;
    }
}

static double opt_u32(const json & object, const char * key, double fallback = 0.0) {
    if (object.contains(key) && object[key].is_number()) {
        return object[key].get<double>();
    }
    return fallback;
}

static double estimate_kv_cache_gb(const json & model, double params_b, int ctx, const std::string & kv_quant) {
    const double bpe = kv_bytes_per_element(kv_quant);
    const double layers = opt_u32(model, "num_hidden_layers");
    const double head_dim = opt_u32(model, "head_dim");
    if (layers > 0.0 && head_dim > 0.0) {
        double kv_heads = opt_u32(model, "num_key_value_heads");
        if (kv_heads <= 0.0) {
            kv_heads = opt_u32(model, "num_attention_heads", 8.0);
        }
        const double bytes = 2.0 * layers * kv_heads * head_dim * (double) ctx * bpe;
        return bytes / 1073741824.0;
    }
    const double fp16 = 0.000008 * params_b * (double) ctx;
    return fp16 * (bpe / 2.0);
}

static std::string infer_use_case(const json & model) {
    const std::string name = lower_copy(json_value(model, "name", std::string()));
    const std::string uc = lower_copy(json_value(model, "use_case", std::string()));
    if (uc.find("embedding") != std::string::npos || name.find("embed") != std::string::npos || name.find("bge") != std::string::npos) return "embedding";
    if (name.find("code") != std::string::npos || uc.find("code") != std::string::npos || name.find("coder") != std::string::npos) return "coding";
    if (uc.find("vision") != std::string::npos || uc.find("multimodal") != std::string::npos || name.find("-vl") != std::string::npos) return "multimodal";
    if (uc.find("reason") != std::string::npos || name.find("deepseek-r1") != std::string::npos || name.find("think") != std::string::npos) return "reasoning";
    if (uc.find("chat") != std::string::npos || uc.find("instruction") != std::string::npos || name.find("instruct") != std::string::npos) return "chat";
    return "general";
}

static int fit_rank(const std::string & level) {
    if (level == "perfect") return 3;
    if (level == "good") return 2;
    if (level == "marginal") return 1;
    return 0;
}

static double fit_score(double required, double available) {
    if (available <= 0.0 || required > available) {
        return 0.0;
    }
    const double ratio = required / available;
    const double comfort = 0.70;
    const double sigma = 0.20;
    const double z = std::max(0.0, (ratio - comfort) / sigma);
    return std::clamp(100.0 * std::exp(-0.5 * z * z), 0.0, 100.0);
}

static double context_score(int context_length, const std::string & use_case) {
    int target = 4096;
    if (use_case == "coding" || use_case == "reasoning") target = 8192;
    if (use_case == "embedding") target = 512;
    if (context_length >= target) return 100.0;
    if (context_length >= target / 2) return 70.0;
    return 30.0;
}

static double quality_score(const json & model, double params_b, const std::string & quant, const std::string & use_case) {
    double quality_params = params_b;
    if (model.contains("active_parameters") && model["active_parameters"].is_number()) {
        quality_params = model["active_parameters"].get<double>() / 1000000000.0;
    }
    double base = 30.0;
    if (quality_params >= 40.0) base = 95.0;
    else if (quality_params >= 20.0) base = 89.0;
    else if (quality_params >= 10.0) base = 82.0;
    else if (quality_params >= 7.0) base = 75.0;
    else if (quality_params >= 3.0) base = 60.0;
    else if (quality_params >= 1.0) base = 45.0;

    const std::string name = lower_copy(json_value(model, "name", std::string()));
    double family = 0.0;
    if (name.find("deepseek") != std::string::npos) family = 3.0;
    else if (name.find("qwen") != std::string::npos || name.find("llama") != std::string::npos) family = 2.0;
    else if (name.find("mistral") != std::string::npos || name.find("mixtral") != std::string::npos || name.find("gemma") != std::string::npos) family = 1.0;

    double task = 0.0;
    if (use_case == "coding" && (name.find("code") != std::string::npos || name.find("coder") != std::string::npos || name.find("starcoder") != std::string::npos)) task = 6.0;
    if (use_case == "reasoning" && params_b >= 13.0) task = 5.0;
    if (use_case == "multimodal" && (name.find("vision") != std::string::npos || name.find("-vl") != std::string::npos)) task = 6.0;

    return std::clamp(base + family + task + quant_quality_penalty(quant), 0.0, 100.0);
}

static double gpu_bandwidth_gbps(const std::string & name) {
    const std::string n = lower_copy(name);
    if (n.find("rtx 5090") != std::string::npos) return 1792.0;
    if (n.find("rtx 4090") != std::string::npos) return 1008.0;
    if (n.find("rtx 3090") != std::string::npos) return 936.0;
    if (n.find("rtx 3080") != std::string::npos) return 760.0;
    if (n.find("a100") != std::string::npos) return 1555.0;
    if (n.find("h100") != std::string::npos) return 3350.0;
    if (n.find("l40") != std::string::npos) return 864.0;
    if (n.find("t4") != std::string::npos) return 320.0;
    return 0.0;
}

static std::string slugify(std::string input) {
    std::string out;
    for (char c : input) {
        unsigned char uc = (unsigned char) c;
        if (std::isalnum(uc)) {
            out.push_back((char) std::tolower(uc));
        } else if (c == '/' || c == '_' || c == '-' || c == '.') {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "fit-model";
    if (out.size() > 80) out.resize(80);
    return out;
}

static std::string tensor_split_from_system(const json & system) {
    if (!system.contains("gpus") || !system["gpus"].is_array() || system["gpus"].empty()) {
        return "";
    }
    std::string out;
    for (const auto & gpu : system["gpus"]) {
        if (!out.empty()) out += ",";
        int gb = std::max(1, (int) std::llround(json_value(gpu, "vram_gb", 1.0)));
        out += std::to_string(gb);
    }
    return out;
}

static std::string model_path_from_meta(const server_model_meta & meta) {
    std::string path;
    if (meta.preset.get_option("LLAMA_ARG_MODEL", path)) {
        return path;
    }
    return "";
}

static std::string hf_repo_from_meta(const server_model_meta & meta) {
    std::string repo;
    if (meta.preset.get_option("LLAMA_ARG_HF_REPO", repo)) {
        return repo;
    }
    return "";
}

static json router_installed_index(server_models_routes & router) {
    json installed = json::array();
    for (const auto & meta : router.models.get_all_meta()) {
        const std::string path = model_path_from_meta(meta);
        const std::string repo = hf_repo_from_meta(meta);
        installed.push_back({
            {"id", meta.name},
            {"path", path.empty() ? nullptr : json(path)},
            {"hf_repo", repo.empty() ? nullptr : json(repo)},
            {"source", server_model_source_to_string(meta.source)},
            {"status", server_model_status_to_string(meta.status)},
        });
    }
    return installed;
}

static bool local_file_exists(const std::string & path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

static json first_gguf_source(const json & model) {
    if (model.contains("gguf_sources") && model["gguf_sources"].is_array() && !model["gguf_sources"].empty()) {
        for (const auto & source : model["gguf_sources"]) {
            if (source.is_object() && source.contains("repo") && source["repo"].is_string()) {
                return source;
            }
        }
    }
    const std::string format = lower_copy(json_value(model, "format", std::string("gguf")));
    const std::string name = json_value(model, "name", std::string());
    if (format == "gguf" && name.find('/') != std::string::npos) {
        return {{"repo", name}, {"provider", json_value(model, "provider", std::string())}};
    }
    return json::object();
}

static json find_installed(const json & installed, const json & model, const std::string & repo, const std::string & quant) {
    const std::string id = json_value(model, "name", std::string());
    const std::string id_slug = slugify(id);
    const std::string repo_quant = repo.empty() ? "" : repo + ":" + quant;
    for (const auto & item : installed) {
        const std::string installed_id = json_value(item, "id", std::string());
        const std::string path = json_value(item, "path", std::string());
        const std::string hf_repo = json_value(item, "hf_repo", std::string());
        if (installed_id == id || installed_id == repo || installed_id == repo_quant || hf_repo == repo_quant || hf_repo == repo) {
            return item;
        }
        if (!path.empty()) {
            const std::string basename = lower_copy(std::filesystem::path(path).filename().string());
            if (contains_ci(basename, id_slug) || (!repo.empty() && contains_ci(basename, slugify(repo)))) {
                return item;
            }
        }
    }
    return json::object();
}

static json make_recommendation_args(const json & system, const std::string & run_mode, const std::string & model_ref, int ctx, int threads) {
    json args = json::array();
    if (local_file_exists(model_ref)) {
        args.push_back("--model");
        args.push_back(model_ref);
    } else if (!model_ref.empty()) {
        args.push_back("--hf-repo");
        args.push_back(model_ref);
    }
    args.push_back("--ctx-size");
    args.push_back(std::to_string(ctx));
    args.push_back("--cache-type-k");
    args.push_back("q8_0");
    args.push_back("--cache-type-v");
    args.push_back("q8_0");
    args.push_back("--flash-attn");
    args.push_back("on");
    args.push_back("--threads");
    args.push_back(std::to_string(std::max(1, threads)));
    args.push_back("--batch-size");
    args.push_back("512");
    args.push_back("--ubatch-size");
    args.push_back("512");

    if (run_mode == "gpu_single") {
        args.push_back("--n-gpu-layers");
        args.push_back("99");
        args.push_back("--main-gpu");
        args.push_back("0");
    } else if (run_mode == "layer_split") {
        args.push_back("--n-gpu-layers");
        args.push_back("99");
        args.push_back("--split-mode");
        args.push_back("layer");
        const std::string split = tensor_split_from_system(system);
        if (!split.empty()) {
            args.push_back("--tensor-split");
            args.push_back(split);
        }
    } else if (run_mode == "cpu_offload") {
        args.push_back("--n-gpu-layers");
        args.push_back("40");
        args.push_back("--split-mode");
        args.push_back("layer");
        const std::string split = tensor_split_from_system(system);
        if (!split.empty()) {
            args.push_back("--tensor-split");
            args.push_back(split);
        }
    } else {
        args.push_back("--n-gpu-layers");
        args.push_back("0");
    }
    return args;
}

static json preset_from_plan(const json & system, const std::string & run_mode, const std::string & model_ref, int ctx, int threads) {
    json preset = {
        {"ctx_size", ctx},
        {"cache_type_k", "q8_0"},
        {"cache_type_v", "q8_0"},
        {"flash_attn", "on"},
        {"threads", std::max(1, threads)},
        {"batch_size", 512},
        {"ubatch_size", 512},
        {"load_on_startup", false},
    };
    if (local_file_exists(model_ref)) {
        preset["model"] = model_ref;
    } else if (!model_ref.empty()) {
        preset["hf_repo"] = model_ref;
    }
    if (run_mode == "gpu_single") {
        preset["n_gpu_layers"] = 99;
        preset["main_gpu"] = 0;
    } else if (run_mode == "layer_split") {
        preset["n_gpu_layers"] = 99;
        preset["split_mode"] = "layer";
        const std::string split = tensor_split_from_system(system);
        if (!split.empty()) preset["tensor_split"] = split;
    } else if (run_mode == "cpu_offload") {
        preset["n_gpu_layers"] = 40;
        preset["split_mode"] = "layer";
        const std::string split = tensor_split_from_system(system);
        if (!split.empty()) preset["tensor_split"] = split;
    } else {
        preset["n_gpu_layers"] = 0;
    }
    return preset;
}

static json analyze_catalog_model(const json & model, const json & system, const json & installed, int target_ctx) {
    const std::string id = json_value(model, "name", std::string());
    const std::string quant = json_value(model, "quantization", std::string("Q4_K_M"));
    const double params_b = params_b_from_json(model);
    const int native_ctx = std::max(512, json_value(model, "context_length", 4096));
    const int ctx = std::max(512, std::min(target_ctx, native_ctx));
    const double weights_gb = params_b * quant_bpp(quant);
    const double kv_gb = estimate_kv_cache_gb(model, params_b, ctx, "q8_0");
    const double overhead_gb = 0.70;
    const double required_gb = weights_gb + kv_gb + overhead_gb;

    const double single_vram = json_value(system, "gpu_vram_gb", 0.0);
    const double total_vram = json_value(system, "total_gpu_vram_gb", 0.0);
    const double ram = json_value(system, "available_ram_gb", 0.0);
    const int gpu_count = json_value(system, "gpu_count", 0);

    std::string fit_level = "too_tight";
    std::string run_mode = "cpu_only";
    double available_gb = ram;
    std::vector<std::string> notes;
    if (gpu_count > 0 && required_gb <= single_vram * 0.90) {
        fit_level = "perfect";
        run_mode = "gpu_single";
        available_gb = single_vram;
        notes.push_back("Fits comfortably on one GPU.");
    } else if (gpu_count > 1 && required_gb <= total_vram * 0.90) {
        fit_level = "good";
        run_mode = "layer_split";
        available_gb = total_vram;
        notes.push_back("Fits across aggregate VRAM; use layer split/tensor split.");
        notes.push_back("Aggregate VRAM is not the same as one giant GPU, but llama.cpp can split layers.");
    } else if (gpu_count > 1 && required_gb <= total_vram) {
        fit_level = "marginal";
        run_mode = "layer_split";
        available_gb = total_vram;
        notes.push_back("Fits aggregate VRAM with little headroom.");
    } else if (gpu_count > 0 && required_gb <= ram) {
        fit_level = "marginal";
        run_mode = "cpu_offload";
        available_gb = ram;
        notes.push_back("Does not fit VRAM cleanly; CPU offload likely works but will be slower.");
    } else if (required_gb <= ram) {
        fit_level = "good";
        run_mode = "cpu_only";
        available_gb = ram;
        notes.push_back("Fits system RAM, but no GPU acceleration was detected.");
    } else {
        fit_level = "too_tight";
        run_mode = gpu_count > 0 ? "layer_split" : "cpu_only";
        available_gb = gpu_count > 0 ? std::max(total_vram, single_vram) : ram;
        notes.push_back("Estimated memory exceeds available RAM/VRAM.");
    }

    const std::string use_case = infer_use_case(model);
    const double bandwidth = gpu_bandwidth_gbps(json_value(system, "gpu_name", std::string()));
    double tps = 0.1;
    if (bandwidth > 0.0 && run_mode != "cpu_only") {
        double mode_factor = run_mode == "layer_split" ? 0.90 : (run_mode == "cpu_offload" ? 0.50 : 1.0);
        tps = std::max(0.1, (bandwidth / std::max(0.1, params_b * quant_bpp(quant))) * 0.55 * mode_factor);
    } else {
        tps = std::max(0.1, (70.0 / std::max(0.1, params_b)) * quant_speed_multiplier(quant));
    }

    const double q = quality_score(model, params_b, quant, use_case);
    const double speed = std::clamp((tps / (use_case == "reasoning" ? 25.0 : 40.0)) * 100.0, 0.0, 100.0);
    const double fit = fit_score(required_gb, available_gb);
    const double ctx_score = context_score(native_ctx, use_case);
    double wq = 0.45, ws = 0.30, wf = 0.15, wc = 0.10;
    if (use_case == "coding")     { wq = 0.50; ws = 0.20; wf = 0.15; wc = 0.15; }
    if (use_case == "reasoning")  { wq = 0.55; ws = 0.15; wf = 0.15; wc = 0.15; }
    if (use_case == "chat")       { wq = 0.40; ws = 0.35; wf = 0.15; wc = 0.10; }
    if (use_case == "multimodal") { wq = 0.50; ws = 0.20; wf = 0.15; wc = 0.15; }
    const double score = std::round((q*wq + speed*ws + fit*wf + ctx_score*wc) * 10.0) / 10.0;

    const json source = first_gguf_source(model);
    const std::string repo = json_value(source, "repo", std::string());
    const std::string download_ref = repo.empty() ? "" : repo + ":" + quant;
    const json installed_match = find_installed(installed, model, repo, quant);
    const std::string local_path = json_value(installed_match, "path", std::string());
    const std::string model_ref = !local_path.empty() ? local_path : download_ref;
    const int threads = json_value(system, "cpu_cores", 8);
    const json preset = preset_from_plan(system, run_mode, model_ref, ctx, threads);

    return {
        {"id", id},
        {"name", std::filesystem::path(id).filename().string()},
        {"provider", json_value(model, "provider", std::string())},
        {"params_b", params_b},
        {"parameter_count", json_value(model, "parameter_count", std::string())},
        {"is_moe", json_value(model, "is_moe", false)},
        {"quant", quant},
        {"format", json_value(model, "format", std::string("gguf"))},
        {"context_length", native_ctx},
        {"effective_context_length", ctx},
        {"use_case", use_case},
        {"fit_level", fit_level},
        {"score", score},
        {"score_components", {{"quality", q}, {"speed", speed}, {"fit", fit}, {"context", ctx_score}}},
        {"estimated_tps", tps},
        {"memory_required_gb", required_gb},
        {"memory_available_gb", available_gb},
        {"weights_gb", weights_gb},
        {"kv_cache_gb", kv_gb},
        {"overhead_gb", overhead_gb},
        {"utilization_pct", available_gb > 0.0 ? required_gb / available_gb * 100.0 : 0.0},
        {"gpu_mode", run_mode},
        {"runtime", "llama.cpp"},
        {"installed", installed_match.is_object() && !installed_match.empty()},
        {"installed_model_id", json_value(installed_match, "id", std::string())},
        {"local_path", local_path.empty() ? nullptr : json(local_path)},
        {"notes", notes},
        {"download", repo.empty() ? json(nullptr) : json({
            {"repo", repo},
            {"quant", quant},
            {"hf_ref", download_ref},
            {"provider", json_value(source, "provider", std::string())},
            {"url", std::string("https://huggingface.co/") + repo},
        })},
        {"recommended_args", make_recommendation_args(system, run_mode, model_ref, ctx, threads)},
        {"preset", preset},
    };
}

static json sorted_filtered_models(const json & catalog, const json & system, const json & installed, const server_http_req & req) {
    const bool include_too_tight = req.get_param("include_too_tight", "") == "true";
    const std::string use_case_filter = lower_copy(req.get_param("use_case", "all"));
    const std::string fit_filter = lower_copy(req.get_param("min_fit", "marginal"));
    const std::string quant_filter = req.get_param("quant", "");
    const std::string search = req.get_param("search", "");
    const int target_ctx = std::max(512, std::atoi(req.get_param("context", "8192").c_str()));
    const int min_rank = include_too_tight ? 0 : fit_rank(fit_filter.empty() ? "marginal" : fit_filter);
    int limit = std::atoi(req.get_param("limit", "300").c_str());
    if (limit <= 0) limit = 300;
    limit = std::min(limit, 2000);

    json out = json::array();
    if (!catalog.is_array()) {
        return out;
    }
    for (const auto & model : catalog) {
        if (!model.is_object()) continue;
        const std::string format = lower_copy(json_value(model, "format", std::string("gguf")));
        if (format != "gguf") continue;
        const json row = analyze_catalog_model(model, system, installed, target_ctx);
        const std::string row_use_case = json_value(row, "use_case", std::string());
        const std::string row_fit = json_value(row, "fit_level", std::string());
        const std::string row_quant = json_value(row, "quant", std::string());
        const std::string row_id = json_value(row, "id", std::string());
        if (use_case_filter != "all" && use_case_filter != row_use_case) continue;
        if (fit_rank(row_fit) < min_rank) continue;
        if (!quant_filter.empty() && !contains_ci(row_quant, quant_filter)) continue;
        if (!search.empty() && !contains_ci(row_id, search)) continue;
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(), [](const json & a, const json & b) {
        const double score_a = json_value(a, "score", 0.0);
        const double score_b = json_value(b, "score", 0.0);
        if (score_a != score_b) return score_a > score_b;
        return json_value(a, "estimated_tps", 0.0) > json_value(b, "estimated_tps", 0.0);
    });
    if ((int) out.size() > limit) {
        out.erase(out.begin() + limit, out.end());
    }
    return out;
}

} // namespace

struct server_fit_advisor_routes::impl {
    server_models_routes & router;
    std::mutex refresh_mutex;

    explicit impl(server_models_routes & router) : router(router) {}

    json refresh_catalog(bool allow_cache) {
        std::lock_guard<std::mutex> lock(refresh_mutex);
        common_remote_params params;
        params.timeout = 20;
        const auto [code, body] = common_remote_get_content(FIT_CATALOG_URL, params);
        if (code >= 200 && code < 300 && !body.empty()) {
            std::string raw(body.begin(), body.end());
            json parsed = json::parse(raw);
            if (!parsed.is_array()) {
                throw std::runtime_error("remote llmfit catalog is not a JSON array");
            }
            write_text_file(catalog_cache_path(), raw);
            json status = {
                {"source", "llmfit"},
                {"url", FIT_CATALOG_URL},
                {"cache_path", catalog_cache_path().string()},
                {"updated_at", isoish_timestamp()},
                {"from_cache", false},
                {"model_count", parsed.size()},
            };
            write_text_file(catalog_status_path(), status.dump(2) + "\n");
            return status;
        }
        if (allow_cache && std::filesystem::exists(catalog_cache_path())) {
            json status = std::filesystem::exists(catalog_status_path())
                ? json::parse(read_text_file(catalog_status_path()))
                : json::object();
            status["source"] = "llmfit";
            status["url"] = FIT_CATALOG_URL;
            status["cache_path"] = catalog_cache_path().string();
            status["from_cache"] = true;
            status["error"] = string_format("catalog refresh failed with HTTP status %ld", code);
            return status;
        }
        throw std::runtime_error(string_format("catalog refresh failed with HTTP status %ld and no cache is available", code));
    }

    json load_catalog(bool refresh) {
        if (refresh || !std::filesystem::exists(catalog_cache_path())) {
            try {
                refresh_catalog(true);
            } catch (const std::exception &) {
                if (!std::filesystem::exists(catalog_cache_path())) {
                    throw;
                }
            }
        }
        return json::parse(read_text_file(catalog_cache_path()));
    }

    json catalog_status_json(bool from_cache) {
        json status = std::filesystem::exists(catalog_status_path())
            ? json::parse(read_text_file(catalog_status_path()))
            : json::object();
        status["source"] = "llmfit";
        status["url"] = FIT_CATALOG_URL;
        status["cache_path"] = catalog_cache_path().string();
        status["from_cache"] = from_cache;
        return status;
    }

    server_http_res_ptr handle_system(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        fit_res_ok(res, detect_system_json());
        return res;
    }

    server_http_res_ptr handle_models(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        const bool refresh = req.get_param("refresh", "false") == "true";
        bool from_cache = true;
        if (refresh) {
            try {
                refresh_catalog(false);
                from_cache = false;
            } catch (const std::exception & e) {
                if (!std::filesystem::exists(catalog_cache_path())) {
                    fit_res_err(res, format_error_response(e.what(), ERROR_TYPE_SERVER));
                    return res;
                }
                from_cache = true;
            }
        }
        json catalog;
        try {
            catalog = load_catalog(false);
        } catch (const std::exception & e) {
            fit_res_err(res, format_error_response(e.what(), ERROR_TYPE_SERVER));
            return res;
        }
        const json system = detect_system_json();
        const json installed = router_installed_index(router);
        const json models = sorted_filtered_models(catalog, system, installed, req);
        fit_res_ok(res, {
            {"object", "list"},
            {"system", system},
            {"catalog", catalog_status_json(from_cache)},
            {"total_catalog_models", catalog.is_array() ? catalog.size() : 0},
            {"returned_models", models.size()},
            {"installed", installed},
            {"models", models},
        });
        return res;
    }

    server_http_res_ptr handle_refresh(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        try {
            fit_res_ok(res, refresh_catalog(false));
        } catch (const std::exception & e) {
            fit_res_err(res, format_error_response(e.what(), ERROR_TYPE_SERVER));
        }
        return res;
    }

    server_http_res_ptr handle_download(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        std::string hf_ref = json_value(body, "hf_ref", std::string());
        if (hf_ref.empty()) {
            const std::string repo = json_value(body, "repo", std::string());
            const std::string quant = json_value(body, "quant", std::string());
            if (!repo.empty()) {
                hf_ref = quant.empty() ? repo : repo + ":" + quant;
            }
        }
        if (hf_ref.empty()) {
            fit_res_err(res, format_error_response("download request must include hf_ref or repo", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }
        if (router.models.has_model(hf_ref)) {
            fit_res_ok(res, {{"success", true}, {"model", hf_ref}, {"already_present", true}});
            return res;
        }
        try {
            server_models::load_options load_opts;
            load_opts.mode = SERVER_CHILD_MODE_DOWNLOAD;
            load_opts.custom_meta = server_model_meta{};
            load_opts.custom_meta->source = SERVER_MODEL_SOURCE_CACHE;
            load_opts.custom_meta->name = hf_ref;
            router.models.load(hf_ref, load_opts);
            fit_res_ok(res, {{"success", true}, {"model", hf_ref}, {"status", "downloading"}});
        } catch (const std::exception & e) {
            fit_res_err(res, format_error_response(e.what(), ERROR_TYPE_SERVER));
        }
        return res;
    }

    std::optional<json::iterator> find_model_entry(json & models, const std::string & id) {
        if (!models.is_array()) {
            return std::nullopt;
        }
        for (auto it = models.begin(); it != models.end(); ++it) {
            if (it->is_object() && json_value(*it, "id", json_value(*it, "name", std::string())) == id) {
                return it;
            }
        }
        return std::nullopt;
    }

    server_http_res_ptr handle_configure(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        json body = req.body.empty() ? json::object() : json::parse(req.body);

        const std::string model_id = json_value(body, "model_id", json_value(body, "id", std::string()));
        std::string local_path = json_value(body, "local_path", std::string());
        std::string hf_ref = json_value(body, "hf_ref", std::string());
        const bool load_now = json_value(body, "load_now", false);

        if (hf_ref.empty()) {
            const std::string repo = json_value(body, "repo", std::string());
            const std::string quant = json_value(body, "quant", std::string());
            if (!repo.empty()) {
                hf_ref = quant.empty() ? repo : repo + ":" + quant;
            }
        }
        if (model_id.empty() && local_path.empty() && hf_ref.empty()) {
            fit_res_err(res, format_error_response("configure request must include model_id, local_path, or hf_ref", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }

        const std::string id = json_value(body, "preset_id", slugify(!model_id.empty() ? model_id : (!hf_ref.empty() ? hf_ref : local_path)));
        const std::string preset_path = router.params.models_preset;
        if (preset_path.empty() || !string_ends_with(preset_path, ".json")) {
            fit_res_err(res, format_error_response("Fit Advisor configure requires a JSON --models-preset file", ERROR_TYPE_NOT_SUPPORTED));
            return res;
        }

        try {
            json root = json::parse(read_text_file(preset_path));
            if (!root.is_object()) {
                root = json::object();
            }
            if (!root.contains("version")) {
                root["version"] = 1;
            }
            if (!root.contains("models") || !root["models"].is_array()) {
                root["models"] = json::array();
            }

            json entry = json_value(body, "preset", json::object());
            if (!entry.is_object()) {
                entry = json::object();
            }
            entry["id"] = id;
            if (!local_path.empty()) {
                entry["model"] = local_path;
            } else if (!hf_ref.empty()) {
                entry["hf_repo"] = hf_ref;
            }
            if (entry.contains("recommended_args")) {
                entry.erase("recommended_args");
            }
            if (!body.contains("preset")) {
                const json system = detect_system_json();
                const int ctx = json_value(body, "ctx_size", 8192);
                const std::string mode = json_value(body, "gpu_mode", std::string("layer_split"));
                entry.merge_patch(preset_from_plan(system, mode, !local_path.empty() ? local_path : hf_ref, ctx, json_value(system, "cpu_cores", 8)));
                entry["id"] = id;
            }
            if (body.contains("alias")) {
                entry["alias"] = body["alias"];
            }
            if (body.contains("tags")) {
                entry["tags"] = body["tags"];
            }

            auto existing = find_model_entry(root["models"], id);
            if (existing.has_value()) {
                **existing = entry;
            } else {
                root["models"].push_back(entry);
            }

            write_text_file(preset_path, root.dump(2) + "\n");
            router.models.load_models();

            bool loaded = false;
            if (load_now) {
                router.models.load(id);
                loaded = true;
            }
            fit_res_ok(res, {
                {"success", true},
                {"model", id},
                {"models_preset", preset_path},
                {"entry", entry},
                {"loaded", loaded},
            });
        } catch (const std::exception & e) {
            fit_res_err(res, format_error_response(e.what(), ERROR_TYPE_SERVER));
        }
        return res;
    }
};

server_fit_advisor_routes::server_fit_advisor_routes(server_models_routes & router)
        : pimpl(std::make_shared<impl>(router)) {
    init_routes();
}

server_fit_advisor_routes::~server_fit_advisor_routes() = default;

void server_fit_advisor_routes::init_routes() {
    get_system = [p = pimpl](const server_http_req & req) {
        return p->handle_system(req);
    };
    get_models = [p = pimpl](const server_http_req & req) {
        return p->handle_models(req);
    };
    post_catalog_refresh = [p = pimpl](const server_http_req & req) {
        return p->handle_refresh(req);
    };
    post_download = [p = pimpl](const server_http_req & req) {
        return p->handle_download(req);
    };
    post_configure = [p = pimpl](const server_http_req & req) {
        return p->handle_configure(req);
    };
}
