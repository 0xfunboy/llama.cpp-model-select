#include "server-fit-advisor.h"

#include "common.h"
#include "download.h"
#include "server-common.h"
#include "server-models.h"
#include "server-persistence.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
#include <sys/sysinfo.h>
#include <sys/wait.h>
#endif

namespace {

static constexpr const char * FIT_CATALOG_URL =
    "https://raw.githubusercontent.com/AlexsJones/llmfit/main/llmfit-core/data/hf_models.json";
static constexpr size_t FIT_MAX_DOWNLOAD_EVENTS = 2000;

struct fit_download_file {
    std::string filename;
    std::string url;
    std::filesystem::path local_path;
    uint64_t total_bytes = 0;
};

struct fit_download_job {
    std::string id;
    std::string model_id;
    std::string repo;
    std::string quant;
    std::string hf_ref;
    std::filesystem::path target_dir;
    std::string requested_filename;
    std::vector<fit_download_file> files;
    std::vector<uint64_t> file_downloaded;
    size_t active_file_index = 0;
    std::string status = "queued";
    std::string error;
    std::string started_at;
    std::string updated_at;
    std::string finished_at;
    std::string local_path;
    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes = 0;
    double speed_bps = 0.0;
    double percent = 0.0;
    int exit_code = 0;
    uint64_t queue_seq = 0;
    int64_t last_progress_ms = 0;
    uint64_t last_progress_bytes = 0;

    mutable std::mutex mutex;
};

struct fit_download_event {
    uint64_t seq = 0;
    std::string event;
    json data;
};

static std::mutex g_fit_download_mutex;
static std::condition_variable g_fit_download_cv;
static std::map<std::string, std::shared_ptr<fit_download_job>> g_fit_download_jobs;
static std::deque<fit_download_event> g_fit_download_events;
static uint64_t g_fit_download_next_seq = 0;
static uint64_t g_fit_download_next_queue_seq = 0;
static bool g_fit_download_dispatcher_running = false;

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

static std::string lower_copy(std::string text);
static std::string read_text_file(const std::filesystem::path & path);

static bool ends_with_ci(const std::string & text, const std::string & suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    return lower_copy(text.substr(text.size() - suffix.size())) == lower_copy(suffix);
}

static std::string home_dir() {
    const char * home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return home;
    }
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}

static std::filesystem::path models_root_dir(const server_models_routes & router) {
    if (!router.params.models_dir.empty()) {
        return router.params.models_dir;
    }
    return std::filesystem::path(home_dir()) / "models";
}

static std::string shell_quote(const std::string & value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

static std::string url_encode_path(const std::string & value) {
    static const char * hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back((char) c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::string read_hf_token() {
    const char * env = std::getenv("HF_TOKEN");
    if (env && env[0] != '\0') {
        return trim_copy(env);
    }
    const std::filesystem::path token_path = std::filesystem::path(home_dir()) / ".cache/huggingface/token";
    std::error_code ec;
    if (!std::filesystem::exists(token_path, ec)) {
        return "";
    }
    try {
        return trim_copy(read_text_file(token_path));
    } catch (...) {
        return "";
    }
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

static std::optional<double> linux_meminfo_gib(const std::string & key) {
#if defined(__linux__)
    try {
        std::ifstream in("/proc/meminfo");
        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find(':');
            if (pos == std::string::npos) {
                continue;
            }
            if (trim_copy(line.substr(0, pos)) != key) {
                continue;
            }
            std::istringstream iss(line.substr(pos + 1));
            double kb = 0.0;
            std::string unit;
            iss >> kb >> unit;
            if (kb > 0.0) {
                return bytes_to_gib(kb * 1024.0);
            }
        }
    } catch (...) {
    }
#else
    (void) key;
#endif
    return std::nullopt;
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
    if (auto mem_total = linux_meminfo_gib("MemTotal")) {
        total_ram_gb = *mem_total;
    }
    if (auto mem_available = linux_meminfo_gib("MemAvailable")) {
        available_ram_gb = *mem_available;
    }
#endif
    if (total_ram_gb <= 0.0) {
        total_ram_gb = 0.0;
    }
    const double fit_ram_capacity_gb = total_ram_gb;

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
        {"fit_ram_capacity_gb", fit_ram_capacity_gb},
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

static bool model_bool(const json & model, const char * key) {
    return model.contains(key) && model[key].is_boolean() && model[key].get<bool>();
}

static double model_number(const json & model, const char * key, double fallback = 0.0) {
    if (model.contains(key) && model[key].is_number()) {
        return model[key].get<double>();
    }
    return fallback;
}

static bool is_moe_model(const json & model) {
    return model_bool(model, "is_moe") ||
           model_number(model, "active_parameters") > 0.0 ||
           model_number(model, "num_experts") > 0.0 ||
           model_number(model, "moe_intermediate_size") > 0.0;
}

static bool is_creative_uncensored_tune(const std::string & name) {
    const std::string n = lower_copy(name);
    return n.find("uncensored") != std::string::npos ||
           n.find("abliterated") != std::string::npos ||
           n.find("heretic") != std::string::npos ||
           n.find("deckard") != std::string::npos ||
           n.find("nsfw") != std::string::npos;
}

static bool is_reasoning_named(const std::string & name) {
    const std::string n = lower_copy(name);
    return n.find("reason") != std::string::npos ||
           n.find("thinking") != std::string::npos ||
           n.find("think") != std::string::npos ||
           n.find("deepseek-r1") != std::string::npos;
}

static std::optional<std::pair<double, double>> moe_memory_for_quant(const json & model, const std::string & quant, double kv_gb, double overhead_gb) {
    if (!is_moe_model(model)) {
        return std::nullopt;
    }
    const double active = model_number(model, "active_parameters");
    const double total = model_number(model, "parameters_raw");
    if (active <= 0.0 || total <= 0.0 || active >= total) {
        return std::nullopt;
    }
    const double bpp = quant_bpp(quant);
    const double active_vram = std::max(0.5, (active * bpp) / 1073741824.0 * 1.10) + kv_gb + overhead_gb;
    const double offloaded_ram = std::max(0.0, ((total - active) * bpp) / 1073741824.0);
    return std::make_pair(active_vram, offloaded_ram);
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
    if (uc.find("reason") != std::string::npos || name.find("deepseek-r1") != std::string::npos || name.find("think") != std::string::npos) {
        if (is_creative_uncensored_tune(name) && uc.find("reason") == std::string::npos) {
            return "general";
        }
        return "reasoning";
    }
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

static double context_score(int context_length, int target_context, const std::string & use_case) {
    int target = std::max(512, target_context);
    if (use_case == "embedding") {
        target = std::min(target, 512);
    }
    if (context_length >= target) return 100.0;
    const double ratio = (double) context_length / (double) target;
    if (ratio >= 0.75) return 85.0;
    if (ratio >= 0.50) return 65.0;
    if (ratio >= 0.25) return 40.0;
    if (context_length >= 8192) return 22.0;
    if (context_length >= 4096) return 12.0;
    return 5.0;
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
    if ((use_case == "reasoning" || use_case == "coding") && is_creative_uncensored_tune(name)) {
        task -= 12.0;
    }
    if (is_reasoning_named(name) && !is_creative_uncensored_tune(name)) {
        task += 3.0;
    }

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

static int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string normalize_token(std::string input) {
    std::string out;
    for (char c : input) {
        const unsigned char uc = (unsigned char) c;
        if (std::isalnum(uc)) {
            out.push_back((char) std::toupper(uc));
        }
    }
    return out;
}

static bool filename_matches_quant(const std::string & filename, const std::string & quant) {
    if (quant.empty()) {
        return true;
    }
    return normalize_token(filename).find(normalize_token(quant)) != std::string::npos;
}

static bool parse_shard_filename(const std::string & filename, std::string & prefix, int & index, int & total) {
    static const std::regex shard_re(R"((.*)-([0-9]{5})-of-([0-9]{5})\.gguf$)", std::regex_constants::icase);
    std::smatch match;
    const std::string base = std::filesystem::path(filename).filename().string();
    if (!std::regex_match(base, match, shard_re)) {
        return false;
    }
    prefix = match[1].str();
    index = std::stoi(match[2].str());
    total = std::stoi(match[3].str());
    return true;
}

static bool is_auxiliary_gguf(const std::string & filename) {
    const std::string lower = lower_copy(filename);
    return lower.find("mmproj") != std::string::npos ||
           lower.find("mmvec") != std::string::npos ||
           lower.find("projector") != std::string::npos ||
           lower.find("tokenizer") != std::string::npos;
}

static std::string download_dir_name(const std::string & repo, const std::string & model_id) {
    std::string base = repo.empty() ? model_id : std::filesystem::path(repo).filename().string();
    if (base.empty()) {
        base = "fit-model";
    }
    if (ends_with_ci(base, "-GGUF")) {
        base = base.substr(0, base.size() - 5);
    }
    return slugify(base);
}

static std::filesystem::path default_download_dir(const server_models_routes & router, const std::string & repo, const std::string & model_id) {
    return models_root_dir(router) / download_dir_name(repo, model_id);
}

static std::optional<std::filesystem::path> find_local_gguf(const std::filesystem::path & dir, const std::string & quant) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(dir, ec)) {
        return std::nullopt;
    }

    struct candidate {
        int score = 0;
        std::string path;
    };

    std::vector<candidate> candidates;
    for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path path = it->path();
        if (!ends_with_ci(path.filename().string(), ".gguf")) {
            continue;
        }
        const std::string filename = path.filename().string();
        std::string prefix;
        int shard_index = 0;
        int shard_total = 0;
        const bool sharded = parse_shard_filename(filename, prefix, shard_index, shard_total);
        int score = 0;
        if (filename_matches_quant(filename, quant)) score += 1000;
        if (!is_auxiliary_gguf(filename)) score += 200;
        if (!sharded) score += 100;
        if (sharded && shard_index == 1) score += 80;
        if (sharded && shard_index > 1) score -= 100;
        candidates.push_back({score, path.string()});
    }

    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const candidate & a, const candidate & b) {
        if (a.score != b.score) return a.score > b.score;
        return a.path < b.path;
    });
    return std::filesystem::path(candidates.front().path);
}

static uint64_t parse_u64_header_value(const std::string & line) {
    auto pos = line.find(':');
    if (pos == std::string::npos) {
        return 0;
    }
    const std::string raw = trim_copy(line.substr(pos + 1));
    try {
        return (uint64_t) std::stoull(raw);
    } catch (...) {
        return 0;
    }
}

static uint64_t remote_file_size(const std::string & url, const std::string & token) {
    std::string command = "curl -fsSIL -L --max-time 30 --connect-timeout 15";
    if (!token.empty()) {
        command += " -H " + shell_quote("Authorization: Bearer " + token);
    }
    command += " " + shell_quote(url) + " 2>/dev/null";

    uint64_t content_length = 0;
    for (const auto & line : shell_lines(command)) {
        const std::string lower = lower_copy(line);
        if (lower.rfind("x-linked-size:", 0) == 0) {
            const uint64_t linked = parse_u64_header_value(line);
            if (linked > 0) {
                return linked;
            }
        }
        if (lower.rfind("content-length:", 0) == 0) {
            const uint64_t size = parse_u64_header_value(line);
            if (size > 0) {
                content_length = size;
            }
        }
    }
    return content_length;
}

struct fit_hf_remote_file {
    std::string filename;
    uint64_t size = 0;
};

static uint64_t json_file_size(const json & file) {
    if (file.contains("size") && file["size"].is_number_unsigned()) {
        return file["size"].get<uint64_t>();
    }
    if (file.contains("size") && file["size"].is_number_integer()) {
        return (uint64_t) std::max<int64_t>(0, file["size"].get<int64_t>());
    }
    if (file.contains("lfs") && file["lfs"].is_object()) {
        const json & lfs = file["lfs"];
        if (lfs.contains("size") && lfs["size"].is_number()) {
            return (uint64_t) std::max<double>(0.0, lfs["size"].get<double>());
        }
    }
    return 0;
}

static std::vector<fit_hf_remote_file> list_hf_gguf_files(const std::string & repo, const std::string & token) {
    common_remote_params params;
    params.timeout = 30;
    params.max_size = 32 * 1024 * 1024;
    if (!token.empty()) {
        params.headers.push_back({"Authorization", "Bearer " + token});
    }

    const std::string api_url = "https://huggingface.co/api/models/" + url_encode_path(repo);
    const auto [code, body] = common_remote_get_content(api_url, params);
    if (code < 200 || code >= 300 || body.empty()) {
        throw std::runtime_error(string_format("Hugging Face model metadata request failed for %s (HTTP %ld)", repo.c_str(), code));
    }

    const std::string raw(body.begin(), body.end());
    const json meta = json::parse(raw);
    std::vector<fit_hf_remote_file> files;
    if (meta.contains("siblings") && meta["siblings"].is_array()) {
        for (const auto & item : meta["siblings"]) {
            const std::string filename = json_value(item, "rfilename", std::string());
            if (!filename.empty() && ends_with_ci(filename, ".gguf")) {
                files.push_back({filename, json_file_size(item)});
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const fit_hf_remote_file & a, const fit_hf_remote_file & b) {
        return a.filename < b.filename;
    });
    return files;
}

static int hf_file_candidate_score(const fit_hf_remote_file & file, const std::string & quant, const std::string & requested_filename) {
    if (!requested_filename.empty()) {
        if (file.filename == requested_filename || std::filesystem::path(file.filename).filename().string() == requested_filename) {
            return 100000;
        }
        return -100000;
    }

    std::string prefix;
    int shard_index = 0;
    int shard_total = 0;
    const bool sharded = parse_shard_filename(file.filename, prefix, shard_index, shard_total);
    int score = 0;
    if (filename_matches_quant(file.filename, quant)) score += 2000;
    if (!is_auxiliary_gguf(file.filename)) score += 400;
    if (!sharded) score += 200;
    if (sharded && shard_index == 1) score += 150;
    if (sharded && shard_index > 1) score -= 200;
    score += std::min<int>(50, (int) (file.size / (1024ull * 1024ull * 1024ull)));
    return score;
}

static std::vector<fit_download_file> resolve_hf_download_files(
        const std::string & repo,
        const std::string & quant,
        const std::string & requested_filename,
        const std::filesystem::path & target_dir,
        const std::string & token) {
    const auto remote_files = list_hf_gguf_files(repo, token);
    if (remote_files.empty()) {
        throw std::runtime_error("no .gguf files found in Hugging Face repo " + repo);
    }

    auto best = remote_files.end();
    int best_score = std::numeric_limits<int>::min();
    for (auto it = remote_files.begin(); it != remote_files.end(); ++it) {
        const int score = hf_file_candidate_score(*it, quant, requested_filename);
        if (score > best_score) {
            best = it;
            best_score = score;
        }
    }
    if (best == remote_files.end() || best_score < -1000) {
        throw std::runtime_error("requested GGUF file was not found in Hugging Face repo " + repo);
    }

    std::vector<fit_hf_remote_file> selected;
    std::string prefix;
    int shard_index = 0;
    int shard_total = 0;
    if (parse_shard_filename(best->filename, prefix, shard_index, shard_total) && shard_total > 1) {
        for (const auto & file : remote_files) {
            std::string file_prefix;
            int file_index = 0;
            int file_total = 0;
            if (parse_shard_filename(file.filename, file_prefix, file_index, file_total) &&
                file_prefix == prefix && file_total == shard_total) {
                selected.push_back(file);
            }
        }
        std::sort(selected.begin(), selected.end(), [](const fit_hf_remote_file & a, const fit_hf_remote_file & b) {
            std::string pa, pb;
            int ia = 0, ib = 0, ta = 0, tb = 0;
            parse_shard_filename(a.filename, pa, ia, ta);
            parse_shard_filename(b.filename, pb, ib, tb);
            return ia < ib;
        });
        if ((int) selected.size() != shard_total) {
            throw std::runtime_error("could not resolve all GGUF shard files for " + best->filename);
        }
    } else {
        selected.push_back(*best);
    }

    std::vector<fit_download_file> out;
    for (const auto & file : selected) {
        fit_download_file item;
        item.filename = file.filename;
        item.url = "https://huggingface.co/" + repo + "/resolve/main/" + url_encode_path(file.filename);
        item.local_path = target_dir / file.filename;
        item.total_bytes = file.size > 0 ? file.size : remote_file_size(item.url, token);
        out.push_back(std::move(item));
    }
    return out;
}

static double aria2_unit_multiplier(std::string unit) {
    unit = lower_copy(unit);
    if (unit.empty() || unit == "b") return 1.0;
    if (unit == "k" || unit == "kb") return 1000.0;
    if (unit == "m" || unit == "mb") return 1000.0 * 1000.0;
    if (unit == "g" || unit == "gb") return 1000.0 * 1000.0 * 1000.0;
    if (unit == "t" || unit == "tb") return 1000.0 * 1000.0 * 1000.0 * 1000.0;
    if (unit == "kib") return 1024.0;
    if (unit == "mib") return 1024.0 * 1024.0;
    if (unit == "gib") return 1024.0 * 1024.0 * 1024.0;
    if (unit == "tib") return 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return 1.0;
}

static uint64_t aria2_to_bytes(const std::string & value, const std::string & unit) {
    try {
        return (uint64_t) std::llround(std::stod(value) * aria2_unit_multiplier(unit));
    } catch (...) {
        return 0;
    }
}

static bool parse_aria2_progress(const std::string & line, uint64_t & done, uint64_t & total, double & speed) {
    static const std::regex amount_re(R"(([0-9]+(?:\.[0-9]+)?)\s*([KMGT]?i?B?)\s*/\s*([0-9]+(?:\.[0-9]+)?)\s*([KMGT]?i?B?)\s*\(\s*([0-9]+)%\s*\))", std::regex_constants::icase);
    static const std::regex speed_re(R"(DL:\s*([0-9]+(?:\.[0-9]+)?)\s*([KMGT]?i?B?))", std::regex_constants::icase);
    std::smatch amount_match;
    if (!std::regex_search(line, amount_match, amount_re)) {
        return false;
    }
    done = aria2_to_bytes(amount_match[1].str(), amount_match[2].str());
    total = aria2_to_bytes(amount_match[3].str(), amount_match[4].str());

    std::smatch speed_match;
    if (std::regex_search(line, speed_match, speed_re)) {
        speed = (double) aria2_to_bytes(speed_match[1].str(), speed_match[2].str());
    } else {
        speed = 0.0;
    }
    return true;
}

static void recompute_download_progress_locked(fit_download_job & job) {
    uint64_t total = 0;
    for (const auto & file : job.files) {
        total += file.total_bytes;
    }
    if (total > 0) {
        job.total_bytes = total;
    }

    uint64_t downloaded = 0;
    for (uint64_t value : job.file_downloaded) {
        downloaded += value;
    }
    job.downloaded_bytes = downloaded;
    job.percent = job.total_bytes > 0 ? std::min(100.0, (double) job.downloaded_bytes * 100.0 / (double) job.total_bytes) : 0.0;
}

static json download_job_snapshot_locked(const std::shared_ptr<fit_download_job> & job) {
    json files = json::array();
    for (size_t i = 0; i < job->files.size(); ++i) {
        const auto & file = job->files[i];
        const uint64_t downloaded = i < job->file_downloaded.size() ? job->file_downloaded[i] : 0;
        files.push_back({
            {"filename", file.filename},
            {"url", file.url},
            {"local_path", file.local_path.string()},
            {"downloaded_bytes", downloaded},
            {"total_bytes", file.total_bytes},
        });
    }

    return {
        {"id", job->id},
        {"model_id", job->model_id},
        {"repo", job->repo},
        {"quant", job->quant},
        {"hf_ref", job->hf_ref},
        {"status", job->status},
        {"error", job->error.empty() ? nullptr : json(job->error)},
        {"target_dir", job->target_dir.string()},
        {"requested_filename", job->requested_filename.empty() ? nullptr : json(job->requested_filename)},
        {"local_path", job->local_path.empty() ? nullptr : json(job->local_path)},
        {"downloaded_bytes", job->downloaded_bytes},
        {"total_bytes", job->total_bytes},
        {"speed_bps", job->speed_bps},
        {"percent", job->percent},
        {"exit_code", job->exit_code},
        {"queue_seq", job->queue_seq},
        {"started_at", job->started_at.empty() ? nullptr : json(job->started_at)},
        {"updated_at", job->updated_at.empty() ? nullptr : json(job->updated_at)},
        {"finished_at", job->finished_at.empty() ? nullptr : json(job->finished_at)},
        {"active_file_index", job->active_file_index},
        {"files", files},
    };
}

static json download_job_snapshot(const std::shared_ptr<fit_download_job> & job) {
    std::lock_guard<std::mutex> lock(job->mutex);
    return download_job_snapshot_locked(job);
}

static void publish_download_snapshot(const std::shared_ptr<fit_download_job> & job) {
    json data = download_job_snapshot(job);
    {
        std::lock_guard<std::mutex> lock(g_fit_download_mutex);
        fit_download_event event;
        event.seq = ++g_fit_download_next_seq;
        event.event = "download";
        data["seq"] = event.seq;
        event.data = data;
        g_fit_download_events.push_back(std::move(event));
        while (g_fit_download_events.size() > FIT_MAX_DOWNLOAD_EVENTS) {
            g_fit_download_events.pop_front();
        }
    }
    g_fit_download_cv.notify_all();
    server_persistence::record_download(data);
}

static void update_download_job(const std::shared_ptr<fit_download_job> & job, const std::function<void(fit_download_job &)> & fn) {
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        fn(*job);
        job->updated_at = isoish_timestamp();
        recompute_download_progress_locked(*job);
    }
    publish_download_snapshot(job);
}

static bool download_status_active(const std::string & status) {
    return status == "queued" || status == "resolving" || status == "downloading";
}

static std::shared_ptr<fit_download_job> find_download_job(const std::string & model_id, const std::string & hf_ref) {
    std::shared_ptr<fit_download_job> fallback;
    std::string fallback_updated_at;
    std::lock_guard<std::mutex> lock(g_fit_download_mutex);
    for (const auto & [_, job] : g_fit_download_jobs) {
        std::lock_guard<std::mutex> job_lock(job->mutex);
        const bool matches = (!model_id.empty() && job->model_id == model_id) || (!hf_ref.empty() && job->hf_ref == hf_ref);
        if (!matches) {
            continue;
        }
        if (download_status_active(job->status)) {
            return job;
        }
        if (!fallback || job->updated_at > fallback_updated_at) {
            fallback = job;
            fallback_updated_at = job->updated_at;
        }
    }
    return fallback;
}

static json download_snapshot_for_model(const std::string & model_id, const std::string & hf_ref) {
    auto job = find_download_job(model_id, hf_ref);
    return job ? download_job_snapshot(job) : json(nullptr);
}

static json all_download_snapshots() {
    std::vector<std::shared_ptr<fit_download_job>> jobs;
    {
        std::lock_guard<std::mutex> lock(g_fit_download_mutex);
        for (const auto & [_, job] : g_fit_download_jobs) {
            jobs.push_back(job);
        }
    }
    std::sort(jobs.begin(), jobs.end(), [](const auto & a, const auto & b) {
        std::lock_guard<std::mutex> la(a->mutex);
        std::lock_guard<std::mutex> lb(b->mutex);
        return a->updated_at > b->updated_at;
    });

    json out = json::array();
    for (const auto & job : jobs) {
        out.push_back(download_job_snapshot(job));
    }
    return out;
}

static int run_aria2c_for_file(const std::shared_ptr<fit_download_job> & job, size_t index, const std::string & token) {
    fit_download_file file;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        file = job->files.at(index);
        job->active_file_index = index;
        job->updated_at = isoish_timestamp();
    }
    publish_download_snapshot(job);

    std::filesystem::create_directories(file.local_path.parent_path());
    std::string command =
        "aria2c"
        " --continue=true"
        " --max-connection-per-server=8"
        " --split=8"
        " --min-split-size=64M"
        " --summary-interval=1"
        " --console-log-level=notice"
        " --auto-file-renaming=false"
        " --allow-overwrite=true"
        " --file-allocation=none";
    if (!token.empty()) {
        command += " --header=" + shell_quote("Authorization: Bearer " + token);
    }
    command += " --dir=" + shell_quote(file.local_path.parent_path().string());
    command += " -o " + shell_quote(file.local_path.filename().string());
    command += " " + shell_quote(file.url);
    command += " 2>&1";

#if defined(_WIN32)
    FILE * pipe = _popen(command.c_str(), "r");
#else
    FILE * pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        return 127;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string chunk(buffer);
        std::replace(chunk.begin(), chunk.end(), '\r', '\n');
        std::istringstream in(chunk);
        std::string line;
        while (std::getline(in, line)) {
            line = trim_copy(line);
            if (line.empty()) {
                continue;
            }
            uint64_t done = 0;
            uint64_t total = 0;
            double speed = 0.0;
            if (parse_aria2_progress(line, done, total, speed)) {
                update_download_job(job, [&](fit_download_job & j) {
                    if (index < j.file_downloaded.size()) {
                        j.file_downloaded[index] = done;
                    }
                    if (total > 0 && index < j.files.size()) {
                        j.files[index].total_bytes = total;
                    }
                    j.speed_bps = speed;
                    j.last_progress_ms = steady_ms();
                    j.last_progress_bytes = j.downloaded_bytes;
                });
            }
        }
    }

#if defined(_WIN32)
    const int raw_status = _pclose(pipe);
    return raw_status;
#else
    const int raw_status = pclose(pipe);
    if (WIFEXITED(raw_status)) {
        return WEXITSTATUS(raw_status);
    }
    if (WIFSIGNALED(raw_status)) {
        return 128 + WTERMSIG(raw_status);
    }
    return raw_status;
#endif
}

static void run_download_job(const std::shared_ptr<fit_download_job> & job) {
    const std::string token = read_hf_token();
    try {
        if (shell_lines("command -v aria2c 2>/dev/null").empty()) {
            throw std::runtime_error("aria2c is not installed or is not in PATH");
        }

        update_download_job(job, [](fit_download_job & j) {
            j.status = "resolving";
            j.started_at = isoish_timestamp();
            j.error.clear();
        });

        auto files = resolve_hf_download_files(job->repo, job->quant, job->requested_filename, job->target_dir, token);
        uint64_t total = 0;
        for (const auto & file : files) {
            total += file.total_bytes;
        }
        update_download_job(job, [&](fit_download_job & j) {
            j.files = files;
            j.file_downloaded.assign(files.size(), 0);
            j.total_bytes = total;
            j.status = "downloading";
        });

        for (size_t i = 0; i < files.size(); ++i) {
            const int rc = run_aria2c_for_file(job, i, token);
            if (rc != 0) {
                update_download_job(job, [&](fit_download_job & j) {
                    j.exit_code = rc;
                });
                throw std::runtime_error(string_format("aria2c failed for %s with exit code %d", files[i].filename.c_str(), rc));
            }
            update_download_job(job, [&](fit_download_job & j) {
                if (i < j.file_downloaded.size()) {
                    uint64_t size = i < j.files.size() ? j.files[i].total_bytes : 0;
                    if (size == 0) {
                        std::error_code ec;
                        size = std::filesystem::file_size(files[i].local_path, ec);
                        if (ec) {
                            size = j.file_downloaded[i];
                        }
                    }
                    j.file_downloaded[i] = size;
                }
            });
        }

        update_download_job(job, [&](fit_download_job & j) {
            j.status = "downloaded";
            j.finished_at = isoish_timestamp();
            j.speed_bps = 0.0;
            j.local_path = j.files.empty() ? "" : j.files.front().local_path.string();
            if (j.local_path.empty()) {
                auto local = find_local_gguf(j.target_dir, j.quant);
                if (local) {
                    j.local_path = local->string();
                }
            }
            recompute_download_progress_locked(j);
            if (j.total_bytes > 0) {
                j.downloaded_bytes = j.total_bytes;
                j.percent = 100.0;
            }
        });
    } catch (const std::exception & e) {
        update_download_job(job, [&](fit_download_job & j) {
            j.status = "failed";
            j.error = e.what();
            j.finished_at = isoish_timestamp();
            j.speed_bps = 0.0;
        });
    }
}

static std::shared_ptr<fit_download_job> next_queued_download_job() {
    std::shared_ptr<fit_download_job> next;
    uint64_t best_seq = std::numeric_limits<uint64_t>::max();
    std::lock_guard<std::mutex> lock(g_fit_download_mutex);
    for (const auto & [_, job] : g_fit_download_jobs) {
        std::lock_guard<std::mutex> job_lock(job->mutex);
        if (job->status != "queued") {
            continue;
        }
        const uint64_t seq = job->queue_seq == 0 ? std::numeric_limits<uint64_t>::max() : job->queue_seq;
        if (!next || seq < best_seq) {
            next = job;
            best_seq = seq;
        }
    }
    return next;
}

static void download_dispatcher_loop() {
    for (;;) {
        auto job = next_queued_download_job();
        if (!job) {
            std::lock_guard<std::mutex> lock(g_fit_download_mutex);
            g_fit_download_dispatcher_running = false;
            return;
        }
        run_download_job(job);
    }
}

static void ensure_download_dispatcher_running() {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(g_fit_download_mutex);
        if (!g_fit_download_dispatcher_running) {
            g_fit_download_dispatcher_running = true;
            start = true;
        }
    }
    if (start) {
        std::thread(download_dispatcher_loop).detach();
    }
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

static json router_installed_index(server_models_routes & router) {
    json installed = json::array();
    const json registry = router.scan_model_registry(false);
    if (!registry.contains("artifacts") || !registry["artifacts"].is_array()) return installed;
    for (const auto & artifact : registry["artifacts"]) {
        std::string id = json_value(artifact, "model_id", std::string());
        if (artifact.contains("configured_ids") && artifact["configured_ids"].is_array() && !artifact["configured_ids"].empty()) {
            id = artifact["configured_ids"].front().get<std::string>();
        }
        installed.push_back({
            {"id", id},
            {"artifact_id", json_value(artifact, "artifact_id", std::string())},
            {"model_id", json_value(artifact, "model_id", std::string())},
            {"preset_id", json_value(artifact, "preset_id", std::string())},
            {"path", artifact.value("primary_path", json(nullptr))},
            {"hf_repo", artifact.value("hf_repo", json(nullptr))},
            {"source", "registry"},
            {"status", json_value(artifact, "health", std::string())},
            {"configured", artifact.value("configured", false)},
            {"loadable", artifact.value("loadable", false)},
        });
    }
    return installed;
}

static bool local_file_exists(const std::string & path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

static std::filesystem::path aria2_sidecar_path(const std::string & path) {
    return std::filesystem::path(path + ".aria2");
}

static bool local_file_has_aria2_sidecar(const std::string & path) {
    if (path.empty()) return false;
    std::error_code ec;
    const auto sidecar = aria2_sidecar_path(path);
    return std::filesystem::exists(sidecar, ec) && std::filesystem::is_regular_file(sidecar, ec);
}

static bool local_model_file_ready(const std::string & path) {
    return local_file_exists(path) && !local_file_has_aria2_sidecar(path);
}

static bool local_model_file_partial(const std::string & path) {
    return local_file_exists(path) && local_file_has_aria2_sidecar(path);
}

static std::string preset_model_path(const std::string & path) {
    if (path.empty()) return path;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (ec) return path;
    const auto relative = std::filesystem::relative(std::filesystem::path(path), cwd, ec);
    if (!ec && !relative.empty()) {
        return relative.string();
    }
    return path;
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

static bool preset_should_disable_reasoning(const std::string & model_id) {
    return is_creative_uncensored_tune(model_id);
}

static json make_recommendation_args(const json & system, const std::string & run_mode, const std::string & model_ref, int ctx, int threads, const std::string & model_id) {
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
    if (preset_should_disable_reasoning(model_id)) {
        args.push_back("--reasoning");
        args.push_back("off");
    } else if (is_reasoning_named(model_id)) {
        args.push_back("--reasoning");
        args.push_back("on");
        args.push_back("--reasoning-budget");
        args.push_back("4096");
    }

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
    } else if (run_mode == "moe_offload") {
        args.push_back("--n-gpu-layers");
        args.push_back("99");
        args.push_back("--split-mode");
        args.push_back("layer");
        const std::string split = tensor_split_from_system(system);
        if (!split.empty()) {
            args.push_back("--tensor-split");
            args.push_back(split);
        }
        args.push_back("--n-cpu-moe");
        args.push_back("999");
    } else {
        args.push_back("--n-gpu-layers");
        args.push_back("0");
    }
    return args;
}

static json preset_from_plan(const json & system, const std::string & run_mode, const std::string & model_ref, int ctx, int threads, const std::string & model_id) {
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
    if (preset_should_disable_reasoning(model_id)) {
        preset["reasoning"] = "off";
    } else if (is_reasoning_named(model_id)) {
        preset["reasoning"] = "on";
        preset["reasoning_budget"] = 4096;
    }
    if (local_file_exists(model_ref)) {
        preset["model"] = preset_model_path(model_ref);
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
    } else if (run_mode == "moe_offload") {
        preset["n_gpu_layers"] = 99;
        preset["split_mode"] = "layer";
        const std::string split = tensor_split_from_system(system);
        if (!split.empty()) preset["tensor_split"] = split;
        preset["n_cpu_moe"] = 999;
    } else {
        preset["n_gpu_layers"] = 0;
    }
    return preset;
}

static json analyze_catalog_model(
        const json & model,
        const json & system,
        const json & installed,
        const server_models_routes & router,
        int target_ctx,
        const std::string & strategy) {
    const std::string id = json_value(model, "name", std::string());
    const std::string quant = json_value(model, "quantization", std::string("Q4_K_M"));
    const double params_b = params_b_from_json(model);
    const int native_ctx = std::max(512, json_value(model, "context_length", 4096));
    const int ctx = std::max(512, std::min(target_ctx, native_ctx));
    const double weights_gb = params_b * quant_bpp(quant);
    const double kv_gb = estimate_kv_cache_gb(model, params_b, ctx, "q8_0");
    const double overhead_gb = 0.70;
    const double required_gb = weights_gb + kv_gb + overhead_gb;
    const auto moe_memory = moe_memory_for_quant(model, quant, kv_gb, overhead_gb);

    const double single_vram = json_value(system, "gpu_vram_gb", 0.0);
    const double total_vram = json_value(system, "total_gpu_vram_gb", 0.0);
    const double ram_capacity = json_value(system, "fit_ram_capacity_gb", json_value(system, "total_ram_gb", 0.0));
    const double ram_available_now = json_value(system, "available_ram_gb", ram_capacity);
    const int gpu_count = json_value(system, "gpu_count", 0);

    std::string fit_level = "too_tight";
    std::string run_mode = "cpu_only";
    double available_gb = ram_capacity;
    double effective_required_gb = required_gb;
    std::vector<std::string> notes;
    double moe_offloaded_gb = 0.0;
    if (strategy == "moe_offload" && moe_memory && gpu_count > 0 &&
            moe_memory->first <= total_vram * 0.92 && moe_memory->second <= ram_capacity) {
        fit_level = moe_memory->first <= total_vram * 0.75 ? "good" : "marginal";
        run_mode = "moe_offload";
        available_gb = total_vram;
        effective_required_gb = moe_memory->first;
        moe_offloaded_gb = moe_memory->second;
        notes.push_back("MoE offload: active experts stay in VRAM; inactive experts spill to system RAM.");
    } else if (gpu_count > 0 && required_gb <= single_vram * 0.90 && strategy != "multi_gpu") {
        fit_level = "perfect";
        run_mode = "gpu_single";
        available_gb = single_vram;
        notes.push_back("Fits comfortably on one GPU.");
    } else if (gpu_count > 1 && strategy == "moe_offload" && moe_memory) {
        fit_level = "too_tight";
        run_mode = "moe_offload";
        available_gb = total_vram;
        effective_required_gb = moe_memory->first;
        moe_offloaded_gb = moe_memory->second;
        notes.push_back("MoE offload requested, but active experts or offloaded RAM exceed this machine.");
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
    } else if (gpu_count > 0 && required_gb <= ram_capacity) {
        fit_level = "marginal";
        run_mode = "cpu_offload";
        available_gb = ram_capacity;
        notes.push_back("Does not fit VRAM cleanly; CPU offload likely works but will be slower.");
    } else if (required_gb <= ram_capacity) {
        fit_level = "good";
        run_mode = "cpu_only";
        available_gb = ram_capacity;
        notes.push_back("Fits system RAM, but no GPU acceleration was detected.");
    } else {
        fit_level = "too_tight";
        run_mode = gpu_count > 0 ? "layer_split" : "cpu_only";
        available_gb = gpu_count > 0 ? std::max(total_vram, single_vram) : ram_capacity;
        notes.push_back("Estimated memory exceeds total RAM/VRAM capacity.");
    }
    if (strategy == "multi_gpu" && gpu_count > 1 && run_mode == "gpu_single" && required_gb <= total_vram * 0.90) {
        run_mode = "layer_split";
        available_gb = total_vram;
        fit_level = "good";
        notes.push_back("MultiGPU mode requested; using layer split even though the model also fits on one GPU.");
    }
    if (strategy == "hybrid_offload" && gpu_count > 0 && run_mode != "cpu_only" && run_mode != "cpu_offload" && required_gb > single_vram * 0.90) {
        run_mode = "cpu_offload";
        available_gb = ram_capacity;
        fit_level = required_gb <= ram_capacity * 0.85 ? "good" : (required_gb <= ram_capacity ? "marginal" : "too_tight");
        notes.push_back("Hybrid offload requested; prioritizing GPU layers with CPU/RAM fallback.");
    }
    if (ram_available_now > 0.0 && available_gb == ram_capacity && effective_required_gb > ram_available_now * 0.90) {
        notes.push_back("Fit is calculated on total RAM capacity; current free RAM is lower because the machine is in use.");
    }

    const std::string use_case = infer_use_case(model);
    const double bandwidth = gpu_bandwidth_gbps(json_value(system, "gpu_name", std::string()));
    double tps = 0.1;
    if (bandwidth > 0.0 && run_mode != "cpu_only") {
        double mode_factor = run_mode == "layer_split" ? 0.90 : (run_mode == "moe_offload" ? 0.80 : (run_mode == "cpu_offload" ? 0.50 : 1.0));
        tps = std::max(0.1, (bandwidth / std::max(0.1, params_b * quant_bpp(quant))) * 0.55 * mode_factor);
    } else {
        tps = std::max(0.1, (70.0 / std::max(0.1, params_b)) * quant_speed_multiplier(quant));
    }

    const double q = quality_score(model, params_b, quant, use_case);
    const double speed = std::clamp((tps / (use_case == "reasoning" ? 25.0 : 40.0)) * 100.0, 0.0, 100.0);
    const double fit = fit_score(effective_required_gb, available_gb);
    const double ctx_score = context_score(native_ctx, target_ctx, use_case);
    double wq = 0.45, ws = 0.30, wf = 0.15, wc = 0.10;
    if (use_case == "coding")     { wq = 0.50; ws = 0.20; wf = 0.15; wc = 0.15; }
    if (use_case == "reasoning")  { wq = 0.55; ws = 0.15; wf = 0.15; wc = 0.15; }
    if (use_case == "chat")       { wq = 0.40; ws = 0.35; wf = 0.15; wc = 0.10; }
    if (use_case == "multimodal") { wq = 0.50; ws = 0.20; wf = 0.15; wc = 0.15; }
    const bool high_capacity_host = total_vram >= 40.0 || ram_capacity >= 96.0;
    if (high_capacity_host && target_ctx >= 65536 && (use_case == "coding" || use_case == "reasoning")) {
        wq = use_case == "reasoning" ? 0.64 : 0.62;
        ws = 0.08;
        wf = 0.12;
        wc = use_case == "reasoning" ? 0.16 : 0.18;
    }
    double strategy_bonus = 0.0;
    if (strategy == "multi_gpu" && run_mode == "layer_split") strategy_bonus = 7.0;
    if (strategy == "moe_offload" && run_mode == "moe_offload") strategy_bonus = 9.0;
    if (strategy == "hybrid_offload" && run_mode == "cpu_offload") strategy_bonus = 5.0;
    double capacity_bonus = 0.0;
    if (high_capacity_host && (use_case == "coding" || use_case == "reasoning")) {
        if (params_b >= 40.0) capacity_bonus = 6.0;
        else if (params_b >= 20.0) capacity_bonus = 4.0;
        else if (params_b < 13.0 && target_ctx >= 65536) capacity_bonus = -3.0;
    }
    const double score = std::round(std::clamp(q*wq + speed*ws + fit*wf + ctx_score*wc + strategy_bonus + capacity_bonus, 0.0, 100.0) * 10.0) / 10.0;

    const json source = first_gguf_source(model);
    const std::string repo = json_value(source, "repo", std::string());
    const std::string download_ref = repo.empty() ? "" : repo + ":" + quant;
    const json installed_match = find_installed(installed, model, repo, quant);
    const bool configured_entry = installed_match.is_object() && !installed_match.empty();
    const std::filesystem::path target_dir = default_download_dir(router, repo, id);
    const json download_job = download_snapshot_for_model(id, download_ref);
    const std::string job_status = download_job.is_object() ? json_value(download_job, "status", std::string()) : std::string();
    const bool active_download = download_status_active(job_status);

    std::string local_path = json_value(installed_match, "path", std::string());
    if (local_path.empty() && download_job.is_object() && job_status == "downloaded") {
        local_path = json_value(download_job, "local_path", std::string());
    }
    if (local_path.empty() && !active_download) {
        auto found = find_local_gguf(target_dir, quant);
        if (found) {
            local_path = found->string();
        }
    }

    const bool partial = local_model_file_partial(local_path);
    const bool downloaded = local_model_file_ready(local_path);
    const bool configured = configured_entry && downloaded;
    std::string download_status = "available";
    if (configured) {
        download_status = "configured";
    } else if (active_download) {
        download_status = "downloading";
    } else if (job_status == "failed") {
        download_status = "failed";
    } else if (partial) {
        download_status = "partial";
    } else if (downloaded) {
        download_status = "downloaded";
    }

    const std::string model_ref = !local_path.empty() ? local_path : download_ref;
    const int threads = json_value(system, "cpu_cores", 8);
    json preset = preset_from_plan(system, run_mode, model_ref, ctx, threads, id);
    if (is_creative_uncensored_tune(id)) {
        notes.push_back("Creative/uncensored finetune: not prioritized for DS4-style correctness; reasoning is disabled in the generated preset.");
    }

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
        {"requested_context_length", target_ctx},
        {"effective_context_length", ctx},
        {"use_case", use_case},
        {"fit_level", fit_level},
        {"score", score},
        {"score_components", {{"quality", q}, {"speed", speed}, {"fit", fit}, {"context", ctx_score}, {"capacity", capacity_bonus}}},
        {"estimated_tps", tps},
        {"memory_required_gb", effective_required_gb},
        {"full_memory_required_gb", required_gb},
        {"memory_available_gb", available_gb},
        {"ram_available_now_gb", ram_available_now},
        {"ram_capacity_gb", ram_capacity},
        {"weights_gb", weights_gb},
        {"kv_cache_gb", kv_gb},
        {"overhead_gb", overhead_gb},
        {"moe_offloaded_gb", moe_offloaded_gb},
        {"utilization_pct", available_gb > 0.0 ? effective_required_gb / available_gb * 100.0 : 0.0},
        {"gpu_mode", run_mode},
        {"fit_strategy", strategy},
        {"runtime", "llama.cpp"},
        {"installed", configured},
        {"configured", configured},
        {"downloaded", downloaded},
        {"partial", partial},
        {"download_status", download_status},
        {"installed_model_id", json_value(installed_match, "id", std::string())},
        {"local_path", local_path.empty() ? nullptr : json(local_path)},
        {"target_dir", target_dir.string()},
        {"download_progress", download_job},
        {"notes", notes},
        {"download", repo.empty() ? json(nullptr) : json({
            {"repo", repo},
            {"quant", quant},
            {"hf_ref", download_ref},
            {"provider", json_value(source, "provider", std::string())},
            {"url", std::string("https://huggingface.co/") + repo},
            {"target_dir", target_dir.string()},
        })},
        {"recommended_args", make_recommendation_args(system, run_mode, model_ref, ctx, threads, id)},
        {"preset", preset},
    };
}

static json sorted_filtered_models(
        const json & catalog,
        const json & system,
        const json & installed,
        const server_models_routes & router,
        const server_http_req & req) {
    const bool include_too_tight = req.get_param("include_too_tight", "") == "true";
    const std::string use_case_filter = lower_copy(req.get_param("use_case", "all"));
    const std::string fit_filter = lower_copy(req.get_param("min_fit", "marginal"));
    const std::string quant_filter = req.get_param("quant", "");
    const std::string search = req.get_param("search", "");
    const std::string strategy = lower_copy(req.get_param("strategy", "balanced"));
    const int target_ctx = std::max(512, std::atoi(req.get_param("context", "131072").c_str()));
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
        const json row = analyze_catalog_model(model, system, installed, router, target_ctx, strategy);
        const std::string row_use_case = json_value(row, "use_case", std::string());
        const std::string row_fit = json_value(row, "fit_level", std::string());
        const std::string row_quant = json_value(row, "quant", std::string());
        const std::string row_id = json_value(row, "id", std::string());
        if (use_case_filter != "all" && use_case_filter != row_use_case) continue;
        if (fit_rank(row_fit) < min_rank) continue;
        const std::string row_mode = json_value(row, "gpu_mode", std::string());
        if (strategy == "multi_gpu") {
            if (row_mode != "layer_split") continue;
            const double single_vram = json_value(system, "gpu_vram_gb", 0.0);
            const double row_required = json_value(row, "full_memory_required_gb", json_value(row, "memory_required_gb", 0.0));
            if (single_vram > 0.0 && row_required <= single_vram * 0.85) continue;
        }
        if (strategy == "moe_offload" && row_mode != "moe_offload") continue;
        if (strategy == "hybrid_offload" && row_mode != "cpu_offload") continue;
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
        const json models = sorted_filtered_models(catalog, system, installed, router, req);
        const json response = {
            {"object", "list"},
            {"system", system},
            {"catalog", catalog_status_json(from_cache)},
            {"total_catalog_models", catalog.is_array() ? catalog.size() : 0},
            {"returned_models", models.size()},
            {"installed", installed},
            {"models", models},
        };
        server_persistence::record_fit_recommendations(response);
        fit_res_ok(res, response);
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
        const std::string model_id = json_value(body, "model_id", json_value(body, "id", std::string()));
        std::string hf_ref = json_value(body, "hf_ref", std::string());
        std::string repo = json_value(body, "repo", std::string());
        std::string quant = json_value(body, "quant", std::string());
        if (hf_ref.empty()) {
            if (!repo.empty()) {
                hf_ref = quant.empty() ? repo : repo + ":" + quant;
            }
        }
        if (!hf_ref.empty() && repo.empty()) {
            auto split = common_download_split_repo_tag(hf_ref);
            repo = split.first;
            if (quant.empty()) {
                quant = split.second;
            }
        }
        if (hf_ref.empty()) {
            fit_res_err(res, format_error_response("download request must include hf_ref or repo", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }
        if (repo.empty()) {
            fit_res_err(res, format_error_response("download request could not resolve a Hugging Face repo", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }

        std::filesystem::path target_dir = json_value(body, "target_dir", std::string());
        if (target_dir.empty()) {
            target_dir = default_download_dir(router, repo, model_id.empty() ? hf_ref : model_id);
        }
        const std::string requested_filename = json_value(body, "filename", std::string());

        try {
            auto local = find_local_gguf(target_dir, quant);
            if (local && local_model_file_ready(local->string())) {
                fit_res_ok(res, {
                    {"success", true},
                    {"model", hf_ref},
                    {"status", "downloaded"},
                    {"already_present", true},
                    {"local_path", local->string()},
                    {"target_dir", target_dir.string()},
                });
                return res;
            }

            if (auto existing = find_download_job(model_id, hf_ref)) {
                const json snapshot = download_job_snapshot(existing);
                const std::string status = json_value(snapshot, "status", std::string());
                if (download_status_active(status) || status == "downloaded") {
                    fit_res_ok(res, {
                        {"success", true},
                        {"model", hf_ref},
                        {"status", status},
                        {"already_present", status == "downloaded"},
                        {"job", snapshot},
                    });
                    return res;
                }
            }

            auto job = std::make_shared<fit_download_job>();
            job->id = slugify((model_id.empty() ? hf_ref : model_id) + "-" + quant) + "-" + std::to_string(steady_ms());
            job->model_id = model_id;
            job->repo = repo;
            job->quant = quant;
            job->hf_ref = hf_ref;
            job->target_dir = target_dir;
            job->requested_filename = requested_filename;
            job->status = "queued";
            job->started_at = isoish_timestamp();
            job->updated_at = job->started_at;

            {
                std::lock_guard<std::mutex> lock(g_fit_download_mutex);
                job->queue_seq = ++g_fit_download_next_queue_seq;
                g_fit_download_jobs[job->id] = job;
            }
            publish_download_snapshot(job);
            ensure_download_dispatcher_running();

            fit_res_ok(res, {
                {"success", true},
                {"model", hf_ref},
                {"status", "queued"},
                {"job", download_job_snapshot(job)},
            });
        } catch (const std::exception & e) {
            fit_res_err(res, format_error_response(e.what(), ERROR_TYPE_SERVER));
        }
        return res;
    }

    server_http_res_ptr handle_downloads(const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        fit_res_ok(res, {
            {"object", "list"},
            {"data", all_download_snapshots()},
        });
        return res;
    }

    server_http_res_ptr handle_download_events(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        uint64_t since = 0;
        try {
            since = req.get_param("since").empty() ? 0 : (uint64_t) std::stoull(req.get_param("since"));
        } catch (...) {
            since = 0;
        }

        res->status = 200;
        res->content_type = "text/event-stream";
        res->headers["Cache-Control"] = "no-cache";
        res->headers["X-Accel-Buffering"] = "no";
        res->next = [since, &req](std::string & output) mutable -> bool {
            std::unique_lock<std::mutex> lock(g_fit_download_mutex);
            g_fit_download_cv.wait_for(lock, std::chrono::seconds(2), [&]() {
                if (req.should_stop()) {
                    return true;
                }
                for (const auto & event : g_fit_download_events) {
                    if (event.seq > since) {
                        return true;
                    }
                }
                return false;
            });

            if (req.should_stop()) {
                return false;
            }

            for (const auto & event : g_fit_download_events) {
                if (event.seq <= since) {
                    continue;
                }
                since = event.seq;
                output = "event: " + event.event + "\n";
                output += "data: " + safe_json_to_str(event.data) + "\n\n";
                return true;
            }

            output = ": keepalive\n\n";
            return true;
        };
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
        if (local_path.empty()) {
            std::string repo = json_value(body, "repo", std::string());
            std::string quant = json_value(body, "quant", std::string());
            if (repo.empty() && !hf_ref.empty()) {
                auto split = common_download_split_repo_tag(hf_ref);
                repo = split.first;
                if (quant.empty()) {
                    quant = split.second;
                }
            }

            const std::filesystem::path target_dir = json_value(
                body,
                "target_dir",
                repo.empty() ? std::string() : default_download_dir(router, repo, model_id.empty() ? hf_ref : model_id).string());
            if (!target_dir.empty()) {
                auto found = find_local_gguf(target_dir, quant);
                if (found) {
                    local_path = found->string();
                }
            }

            if (local_path.empty()) {
                const json snapshot = download_snapshot_for_model(model_id, hf_ref);
                if (snapshot.is_object() && json_value(snapshot, "status", std::string()) == "downloaded") {
                    local_path = json_value(snapshot, "local_path", std::string());
                }
            }
        }
        if (model_id.empty() && local_path.empty() && hf_ref.empty()) {
            fit_res_err(res, format_error_response("configure request must include model_id, local_path, or hf_ref", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }
        if (!local_path.empty() && local_model_file_partial(local_path)) {
            fit_res_err(res, format_error_response("model download is incomplete; resume the download before configuring it", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }
        if (!local_path.empty() && !local_model_file_ready(local_path)) {
            fit_res_err(res, format_error_response("model file is not ready or is missing on disk", ERROR_TYPE_INVALID_REQUEST));
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
                entry["model"] = preset_model_path(local_path);
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
                entry.merge_patch(preset_from_plan(system, mode, !local_path.empty() ? local_path : hf_ref, ctx, json_value(system, "cpu_cores", 8), id));
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
            const json response = {
                {"success", true},
                {"model", id},
                {"models_preset", preset_path},
                {"entry", entry},
                {"loaded", loaded},
            };
            server_persistence::record_configuration("fit-advisor", id, id, response);
            fit_res_ok(res, response);
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
    get_downloads = [p = pimpl](const server_http_req & req) {
        return p->handle_downloads(req);
    };
    get_download_events = [p = pimpl](const server_http_req & req) {
        return p->handle_download_events(req);
    };
    post_configure = [p = pimpl](const server_http_req & req) {
        return p->handle_configure(req);
    };
}
