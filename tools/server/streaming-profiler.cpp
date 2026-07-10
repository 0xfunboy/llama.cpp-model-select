#include "streaming-profiler.h"

#include "caliber-scoring.h"
#include "server-common.h"

#include <cpp-httplib/httplib.h>
#include <sheredom/subprocess.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace streaming_profiler {
namespace {

using clock_type = std::chrono::steady_clock;

double elapsed_ms(clock_type::time_point start, clock_type::time_point end = clock_type::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::vector<std::string> split_args(const std::string & text) {
    std::vector<std::string> out;
    std::string current;
    char quote = 0;
    for (char c : text) {
        if (quote) {
            if (c == quote) quote = 0;
            else current.push_back(c);
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (std::isspace((unsigned char) c)) {
            if (!current.empty()) { out.push_back(current); current.clear(); }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string arg_value(const std::vector<std::string> & args, std::initializer_list<const char *> names) {
    for (size_t i = 0; i < args.size(); ++i) {
        for (const char * name : names) {
            if (args[i] == name && i + 1 < args.size()) return args[i + 1];
            const std::string prefix = std::string(name) + "=";
            if (args[i].rfind(prefix, 0) == 0) return args[i].substr(prefix.size());
        }
    }
    return "";
}

int arg_int(const std::vector<std::string> & args, std::initializer_list<const char *> names, int fallback) {
    try {
        const std::string value = arg_value(args, names);
        return value.empty() ? fallback : std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::vector<char *> argv_for(std::vector<std::string> & command) {
    std::vector<char *> out;
    for (auto & value : command) out.push_back(value.data());
    out.push_back(nullptr);
    return out;
}

std::string run_capture(std::vector<std::string> command) {
    auto argv = argv_for(command);
    subprocess_s process{};
    const int options = subprocess_option_no_window | subprocess_option_combined_stdout_stderr |
        subprocess_option_inherit_environment | subprocess_option_search_user_path;
    if (subprocess_create(argv.data(), options, &process) != 0) return "";
    std::string output;
    std::array<char, 4096> buffer{};
    FILE * stream = subprocess_stdout(&process);
    while (stream && fgets(buffer.data(), (int) buffer.size(), stream)) output += buffer.data();
    int exit_code = 0;
    subprocess_join(&process, &exit_code);
    subprocess_destroy(&process);
    return output;
}

int free_loopback_port() {
#ifdef _WIN32
    static std::atomic<int> next{49152};
    return next.fetch_add(1);
#else
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("failed to create loopback socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        throw std::runtime_error("failed to reserve loopback port");
    }
    socklen_t size = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size);
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
#endif
}

struct gpu_snapshot {
    bool available = false;
    bool process_found = false;
    double memory_mib = 0;
    double power_w = 0;
    double temperature_c = 0;
    double utilization_pct = 0;
};

gpu_snapshot read_gpu_snapshot(int process_pid = 0) {
    const std::string output = run_capture({
        "nvidia-smi", "--query-gpu=memory.used,power.draw,temperature.gpu,utilization.gpu",
        "--format=csv,noheader,nounits",
    });
    gpu_snapshot snapshot;
    std::istringstream lines(output);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream values(line);
        double memory = 0, power = 0, temperature = 0, utilization = 0;
        if (!(values >> memory >> power >> temperature >> utilization)) continue;
        snapshot.available = true;
        if (process_pid <= 0) snapshot.memory_mib += memory;
        snapshot.power_w += power;
        snapshot.temperature_c = std::max(snapshot.temperature_c, temperature);
        snapshot.utilization_pct += utilization;
        ++count;
    }
    if (count > 0) snapshot.utilization_pct /= count;
    if (process_pid > 0) {
        const std::string apps = run_capture({
            "nvidia-smi", "--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits",
        });
        std::istringstream app_lines(apps);
        while (std::getline(app_lines, line)) {
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream values(line);
            int pid = 0;
            double memory = 0;
            if (values >> pid >> memory && pid == process_pid) {
                snapshot.process_found = true;
                snapshot.memory_mib += memory;
            }
        }
        if (!snapshot.process_found) {
            snapshot.available = false;
            snapshot.power_w = 0;
            snapshot.temperature_c = 0;
            snapshot.utilization_pct = 0;
        }
    }
    return snapshot;
}

double process_rss_mib(int pid) {
#ifdef _WIN32
    (void) pid;
    return 0;
#else
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string key;
    double kib = 0;
    std::string unit;
    while (status >> key) {
        if (key == "VmRSS:") { status >> kib >> unit; return kib / 1024.0; }
        std::string rest;
        std::getline(status, rest);
    }
    return 0;
#endif
}

struct process_snapshot {
    double rss_mib = 0;
    double read_mib = 0;
    double write_mib = 0;
};

process_snapshot read_process_snapshot(int pid) {
    process_snapshot result;
    result.rss_mib = process_rss_mib(pid);
#ifndef _WIN32
    std::ifstream io("/proc/" + std::to_string(pid) + "/io");
    std::string key;
    double bytes = 0;
    while (io >> key >> bytes) {
        if (key == "read_bytes:") result.read_mib = bytes / (1024.0 * 1024.0);
        if (key == "write_bytes:") result.write_mib = bytes / (1024.0 * 1024.0);
    }
#endif
    return result;
}

int process_id(const subprocess_s & process) {
#ifdef _WIN32
    return (int) process.dwProcessId;
#else
    return (int) process.child;
#endif
}

std::string repeated_prompt(int target_tokens) {
    const std::string unit = "calibration ";
    std::string out;
    out.reserve((size_t) target_tokens * unit.size());
    for (int i = 0; i < target_tokens; ++i) out += unit;
    out += "Respond with exactly: ready";
    return out;
}

json timings_from_event(const json & event) {
    if (event.contains("timings") && event["timings"].is_object()) return event["timings"];
    return json::object();
}

struct stream_measurement {
    bool ok = false;
    int status = 0;
    std::string raw;
    json timings = json::object();
    std::vector<double> token_times_ms;
    double ttft_ms = 0;
    double total_ms = 0;
};

stream_measurement stream_request(httplib::Client & client, const json & request, const std::function<bool()> & cancelled) {
    stream_measurement measurement;
    const auto started = clock_type::now();
    std::string pending;
    bool first_token = false;
    auto parse_lines = [&](bool flush) {
        for (;;) {
            const size_t newline = pending.find('\n');
            if (newline == std::string::npos && !flush) break;
            std::string line = newline == std::string::npos ? std::move(pending) : pending.substr(0, newline);
            if (newline == std::string::npos) pending.clear(); else pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) { if (newline == std::string::npos) break; else continue; }
            std::string data = line.substr(5);
            while (!data.empty() && std::isspace((unsigned char) data.front())) data.erase(data.begin());
            if (data == "[DONE]") { if (newline == std::string::npos) break; else continue; }
            try {
                const json event = json::parse(data);
                const json timings = timings_from_event(event);
                if (!timings.empty()) measurement.timings = timings;
                bool content = false;
                if (event.contains("choices") && event["choices"].is_array() && !event["choices"].empty()) {
                    const auto & choice = event["choices"][0];
                    if (choice.contains("delta") && choice["delta"].is_object()) {
                        const std::string token = choice["delta"].value("content", choice["delta"].value("reasoning_content", std::string()));
                        content = !token.empty();
                    }
                }
                if (content) {
                    const double now = elapsed_ms(started);
                    measurement.token_times_ms.push_back(now);
                    if (!first_token) { measurement.ttft_ms = now; first_token = true; }
                }
            } catch (...) {}
            if (newline == std::string::npos) break;
        }
    };
    auto response = client.Post(
        "/v1/chat/completions", httplib::Headers{}, request.dump(), "application/json",
        [&](const char * data, size_t size) {
            if (cancelled && cancelled()) return false;
            measurement.raw.append(data, size);
            pending.append(data, size);
            parse_lines(false);
            return true;
        });
    parse_lines(true);
    measurement.total_ms = elapsed_ms(started);
    measurement.status = response ? response->status : 0;
    measurement.ok = response && response->status >= 200 && response->status < 300 && first_token;
    return measurement;
}

class child_server {
  public:
    child_server(std::vector<std::string> command) : command_(std::move(command)) {
        auto argv = argv_for(command_);
        const int options = subprocess_option_no_window | subprocess_option_combined_stdout_stderr | subprocess_option_inherit_environment;
        if (subprocess_create(argv.data(), options, &process_) != 0) throw std::runtime_error("failed to spawn isolated llama-server");
        started_ = true;
        reader_ = std::thread([this]() {
            std::array<char, 4096> buffer{};
            FILE * stream = subprocess_stdout(&process_);
            while (stream && fgets(buffer.data(), (int) buffer.size(), stream)) {
                std::lock_guard<std::mutex> lock(log_mutex_);
                logs_ += buffer.data();
                if (logs_.size() > 4 * 1024 * 1024) logs_.erase(0, logs_.size() - 4 * 1024 * 1024);
            }
        });
    }

    ~child_server() { stop(); }
    child_server(const child_server &) = delete;
    child_server & operator=(const child_server &) = delete;

    int pid() const { return process_id(process_); }
    bool alive() { return started_ && subprocess_alive(&process_); }
    std::string logs() const { std::lock_guard<std::mutex> lock(log_mutex_); return logs_; }

    void stop() {
        if (!started_) return;
        if (subprocess_alive(&process_)) subprocess_terminate(&process_);
        int exit_code = 0;
        subprocess_join(&process_, &exit_code);
        if (reader_.joinable()) reader_.join();
        subprocess_destroy(&process_);
        started_ = false;
    }

  private:
    std::vector<std::string> command_;
    subprocess_s process_{};
    bool started_ = false;
    mutable std::mutex log_mutex_;
    std::string logs_;
    std::thread reader_;
};

std::vector<std::string> server_command(const json & item, const json & cfg, const std::filesystem::path & executable, int port) {
    const std::string model_path = item.value("model_path", item.value("path", std::string()));
    if (model_path.empty()) throw std::runtime_error("streaming profile item has no model path");
    const auto logical = split_args(item.value("extra_args", std::string()));
    std::vector<std::string> command = {
        executable.string(), "--host", "127.0.0.1", "--port", std::to_string(port), "--model", model_path,
        "--alias", "caliber-profile", "--parallel", std::to_string(std::max(1, arg_int(logical, {"--parallel", "-np"}, 1))),
    };
    auto map_value = [&](const char * destination, std::initializer_list<const char *> names) {
        const std::string value = arg_value(logical, names);
        if (!value.empty()) { command.push_back(destination); command.push_back(value); }
    };
    map_value("--ctx-size", {"--ctx-size", "-c"});
    map_value("--n-gpu-layers", {"--gpu-layers", "--n-gpu-layers", "-ngl"});
    map_value("--cache-type-k", {"--cache-type-k", "-ctk"});
    map_value("--cache-type-v", {"--cache-type-v", "-ctv"});
    map_value("--n-cpu-moe", {"--n-cpu-moe", "-ncmoe"});
    map_value("--split-mode", {"--split-mode", "-sm"});
    map_value("--main-gpu", {"--main-gpu", "-mg"});
    map_value("--flash-attn", {"--flash-attn", "-fa"});
    map_value("--tensor-split", {"--tensor-split", "-ts"});
    map_value("--batch-size", {"--batch-size", "-b"});
    map_value("--ubatch-size", {"--ubatch-size", "-ub"});
    map_value("--threads", {"--threads", "-t"});
    (void) cfg;
    return command;
}

} // namespace

double percentile_ms(std::vector<double> values, double percentile) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const double position = std::clamp(percentile, 0.0, 1.0) * (values.size() - 1);
    const size_t lower_index = (size_t) std::floor(position);
    const size_t upper_index = (size_t) std::ceil(position);
    const double fraction = position - lower_index;
    return values[lower_index] * (1 - fraction) + values[upper_index] * fraction;
}

json parse_sse_trace(const std::string & body) {
    std::vector<double> token_offsets;
    json timings = json::object();
    std::istringstream lines(body);
    std::string line;
    size_t token_index = 0;
    while (std::getline(lines, line)) {
        if (line.rfind("data:", 0) != 0) continue;
        std::string data = line.substr(5);
        while (!data.empty() && std::isspace((unsigned char) data.front())) data.erase(data.begin());
        if (data == "[DONE]") continue;
        try {
            const json event = json::parse(data);
            const json event_timings = timings_from_event(event);
            if (!event_timings.empty()) timings = event_timings;
            if (event.contains("choices") && event["choices"].is_array() && !event["choices"].empty()) {
                const json delta = event["choices"][0].value("delta", json::object());
                const std::string content = delta.value("content", delta.value("reasoning_content", std::string()));
                if (!content.empty()) token_offsets.push_back((double) token_index++);
            }
        } catch (...) {}
    }
    return {{"token_count", token_offsets.size()}, {"timings", timings}};
}

json encode_timeline(const std::vector<json> & samples) {
    json encoded = {
        {"encoding", "delta-columns-v1"},
        {"sample_count", samples.size()},
        {"columns", {"dt_ms", "vram_mib", "power_w", "temperature_c", "gpu_util_pct", "process_rss_mib", "read_mib", "write_mib"}},
        {"rows", json::array()},
    };
    double previous_t = 0;
    for (const auto & sample : samples) {
        const double t = sample.value("t_ms", previous_t);
        encoded["rows"].push_back({
            std::round((t - previous_t) * 10.0) / 10.0,
            sample.value("vram_mib", json(nullptr)), sample.value("power_w", json(nullptr)),
            sample.value("temperature_c", json(nullptr)), sample.value("gpu_util_pct", json(nullptr)),
            sample.value("process_rss_mib", 0.0), sample.value("read_mib", 0.0), sample.value("write_mib", 0.0),
        });
        previous_t = t;
    }
    return encoded;
}

json profile(const json & item, const json & cfg, const std::filesystem::path & executable, const std::function<bool()> & cancelled) {
    const int port = free_loopback_port();
    const gpu_snapshot host_gpu = read_gpu_snapshot();
    const auto load_started = clock_type::now();
    child_server server(server_command(item, cfg, executable, port));
    const gpu_snapshot baseline_gpu = read_gpu_snapshot(server.pid());
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(3600, 0);
    bool ready = false;
    const int startup_timeout_sec = std::max(10, cfg.value("bench", json::object()).value("startup_timeout_sec", 900));
    const auto ready_deadline = clock_type::now() + std::chrono::seconds(startup_timeout_sec);
    while (clock_type::now() < ready_deadline && server.alive() && !(cancelled && cancelled())) {
        auto response = client.Get("/health");
        if (response && response->status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ready) throw std::runtime_error("isolated llama-server did not become ready: " + server.logs().substr(0, 2000));
    const double load_ms = elapsed_ms(load_started);

    const auto logical = split_args(item.value("extra_args", std::string()));
    const int requested_context = std::max(0, arg_int(logical, {"--ctx-size", "-c"}, 0));
    const std::string workload = item.value("workload_kind", std::string("baseline"));
    int prompt_tokens = 512;
    if (workload == "prefill") prompt_tokens = std::max(1, item.value("prefill_target_tokens", 2048));
    if (workload == "kv-fill") prompt_tokens = std::max(1, item.value("kv_fill_target_tokens", 2048));
    const int max_tokens = std::max(1, cfg.value("bench", json::object()).value("n_predict", 128));
    json request = {
        {"model", "caliber-profile"},
        {"messages", {{{"role", "user"}, {"content", repeated_prompt(prompt_tokens)}}}},
        {"temperature", 0}, {"seed", 42}, {"max_tokens", max_tokens}, {"stream", true},
    };

    json warmup_request = request;
    warmup_request["max_tokens"] = std::min(16, max_tokens);
    const auto warmup_started = clock_type::now();
    const auto warmup = stream_request(client, warmup_request, cancelled);
    const double warmup_ms = elapsed_ms(warmup_started);
    if (!warmup.ok) throw std::runtime_error("streaming warmup failed with HTTP " + std::to_string(warmup.status));

    std::atomic<bool> sampling{true};
    std::mutex samples_mutex;
    std::vector<json> timeline;
    gpu_snapshot peak = baseline_gpu;
    double process_peak_mib = 0;
    double process_read_peak_mib = 0;
    double process_write_peak_mib = 0;
    std::thread sampler([&]() {
        const auto started = clock_type::now();
        while (sampling.load()) {
            const gpu_snapshot gpu = read_gpu_snapshot(server.pid());
            const process_snapshot process = read_process_snapshot(server.pid());
            {
                std::lock_guard<std::mutex> lock(samples_mutex);
                if (gpu.available) {
                    peak.available = true;
                    peak.process_found = peak.process_found || gpu.process_found;
                    peak.memory_mib = std::max(peak.memory_mib, gpu.memory_mib);
                    peak.power_w = std::max(peak.power_w, gpu.power_w);
                    peak.temperature_c = std::max(peak.temperature_c, gpu.temperature_c);
                    peak.utilization_pct = std::max(peak.utilization_pct, gpu.utilization_pct);
                }
                process_peak_mib = std::max(process_peak_mib, process.rss_mib);
                process_read_peak_mib = std::max(process_read_peak_mib, process.read_mib);
                process_write_peak_mib = std::max(process_write_peak_mib, process.write_mib);
                timeline.push_back({{"t_ms", elapsed_ms(started)}, {"vram_mib", gpu.available ? json(gpu.memory_mib) : json(nullptr)},
                    {"power_w", gpu.available ? json(gpu.power_w) : json(nullptr)}, {"temperature_c", gpu.available ? json(gpu.temperature_c) : json(nullptr)},
                    {"gpu_util_pct", gpu.available ? json(gpu.utilization_pct) : json(nullptr)}, {"process_rss_mib", process.rss_mib},
                    {"read_mib", process.read_mib}, {"write_mib", process.write_mib}});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    const int repetitions = std::max(2, cfg.value("bench", json::object()).value("repetitions", 3));
    std::vector<json> runs;
    std::string request_error;
    for (int index = 0; index < repetitions; ++index) {
        if (cancelled && cancelled()) break;
        const auto measured = stream_request(client, request, cancelled);
        if (!measured.ok) {
            request_error = "streaming request failed with HTTP " + std::to_string(measured.status);
            break;
        }
        std::vector<double> itl;
        for (size_t i = 1; i < measured.token_times_ms.size(); ++i) itl.push_back(measured.token_times_ms[i] - measured.token_times_ms[i - 1]);
        const json timings = measured.timings;
        const int prompt_n = timings.value("prompt_n", prompt_tokens);
        const int predicted_n = timings.value("predicted_n", (int) measured.token_times_ms.size());
        gpu_snapshot observed_peak;
        double observed_process_peak = 0;
        {
            std::lock_guard<std::mutex> lock(samples_mutex);
            observed_peak = peak;
            observed_process_peak = process_peak_mib;
        }
        runs.push_back({
            {"run_index", index}, {"ok", true}, {"benchmark_backend", "llama-server-streaming"},
            {"prompt_n", prompt_n}, {"prompt_tps", timings.value("prompt_per_second", 0.0)},
            {"eval_n", predicted_n}, {"eval_tps", timings.value("predicted_per_second", predicted_n > 0 ? predicted_n / std::max(0.001, measured.total_ms / 1000.0) : 0.0)},
            {"e2e_ttft_ms", measured.ttft_ms}, {"client_ttft_ms", measured.ttft_ms}, {"ttft_sec", measured.ttft_ms / 1000.0},
            {"total_request_ms", measured.total_ms}, {"latency_total_request_ms", measured.total_ms},
            {"tpot_ms", itl.empty() ? 0.0 : std::accumulate(itl.begin(), itl.end(), 0.0) / itl.size()},
            {"itl_p50_ms", percentile_ms(itl, 0.50)}, {"itl_p95_ms", percentile_ms(itl, 0.95)}, {"itl_p99_ms", percentile_ms(itl, 0.99)},
            {"ctx_size", requested_context}, {"measured_context_size", requested_context}, {"context_measurement_kind", "server-effective"},
            {"context_target_met", requested_context > 0}, {"memory_measurement_kind", observed_peak.process_found ? "observed" : "process-observed"},
            {"vram_baseline_mib", baseline_gpu.available ? baseline_gpu.memory_mib : 0.0},
            {"vram_peak_mib", observed_peak.available ? observed_peak.memory_mib : 0.0}, {"vram_total_peak_mib", observed_peak.available ? observed_peak.memory_mib : 0.0},
            {"shared_peak_mib", 0.0}, {"gpu_power_peak_w", observed_peak.power_w}, {"gpu_temp_peak_c", observed_peak.temperature_c},
            {"gpu_util_avg_pct", observed_peak.utilization_pct}, {"process_working_set_peak_mib", observed_process_peak},
            {"process_read_peak_mib", process_read_peak_mib}, {"process_write_peak_mib", process_write_peak_mib},
            {"ready", true}, {"fit_status", "success"}, {"fit_status_source", "llama-server"},
        });
    }
    sampling.store(false);
    sampler.join();
    if (!request_error.empty()) throw std::runtime_error(request_error);

    const json parsed_logs = caliber::parse_llama_server_stderr(server.logs());
    for (auto & run : runs) {
        for (auto it = parsed_logs.begin(); it != parsed_logs.end(); ++it) run[it.key()] = it.value();
        const int effective = parsed_logs.value("effective_context_size", requested_context);
        run["ctx_size"] = effective;
        run["measured_context_size"] = effective;
        run["context_target_met"] = requested_context <= 0 || effective >= requested_context;
        run["load_sec"] = load_ms / 1000.0;
        run["warmup_ms"] = warmup_ms;
        run["kv_fill_measured_tokens"] = run.value("prompt_n", 0);
        if (workload == "kv-fill") {
            const int target = item.value("kv_fill_target_tokens", prompt_tokens);
            run["kv_fill_target_met"] = run.value("prompt_n", 0) >= std::max(1, target * 9 / 10);
        }
    }
    if (runs.empty()) throw std::runtime_error("streaming profile cancelled before a measured request");
    json result = caliber::aggregate_bench_result(item, cfg, runs);
    result["benchmark_backend"] = "llama-server-streaming";
    result["evidence_level"] = "streaming-measured";
    result["timeline"] = encode_timeline(timeline);
    result["streaming_profile"] = {{"repetitions", runs.size()}, {"prompt_target_tokens", prompt_tokens}, {"generate_target_tokens", max_tokens},
        {"load_ms", load_ms}, {"warmup_ms", warmup_ms}, {"gpu_process_scoped", host_gpu.available},
        {"process_read_peak_mib", process_read_peak_mib}, {"process_write_peak_mib", process_write_peak_mib}};

    std::vector<int> concurrency_profiles;
    const json bench = cfg.value("bench", json::object());
    if (bench.contains("concurrency_profiles") && bench["concurrency_profiles"].is_array()) {
        for (const auto & value : bench["concurrency_profiles"]) if (value.is_number_integer() && value.get<int>() > 0) concurrency_profiles.push_back(value.get<int>());
    }
    std::sort(concurrency_profiles.begin(), concurrency_profiles.end());
    concurrency_profiles.erase(std::unique(concurrency_profiles.begin(), concurrency_profiles.end()), concurrency_profiles.end());
    result["concurrency_profiles"] = json::array();
    for (const int concurrency : concurrency_profiles) {
        if (concurrency == 1) {
            result["concurrency_profiles"].push_back({{"concurrency", 1}, {"ok", true}, {"request_p50_ms", result.value("latency_total_request_ms", 0.0)}, {"aggregate_tps", result.value("eval_tps", 0.0)}});
            continue;
        }
        if (cancelled && cancelled()) break;
        std::vector<stream_measurement> measurements((size_t) concurrency);
        std::vector<std::thread> clients;
        for (int index = 0; index < concurrency; ++index) {
            clients.emplace_back([&, index]() {
                httplib::Client concurrent_client("127.0.0.1", port);
                concurrent_client.set_connection_timeout(1, 0);
                concurrent_client.set_read_timeout(3600, 0);
                measurements[(size_t) index] = stream_request(concurrent_client, request, cancelled);
            });
        }
        for (auto & thread : clients) thread.join();
        bool all_ok = true;
        std::vector<double> latencies;
        double aggregate_tps = 0;
        for (const auto & measurement : measurements) {
            all_ok = all_ok && measurement.ok;
            latencies.push_back(measurement.total_ms);
            const int predicted = measurement.timings.value("predicted_n", (int) measurement.token_times_ms.size());
            aggregate_tps += measurement.timings.value("predicted_per_second", predicted / std::max(0.001, measurement.total_ms / 1000.0));
        }
        result["concurrency_profiles"].push_back({{"concurrency", concurrency}, {"ok", all_ok},
            {"request_p50_ms", percentile_ms(latencies, 0.50)}, {"request_p95_ms", percentile_ms(latencies, 0.95)}, {"aggregate_tps", aggregate_tps}});
    }
    return result;
}

} // namespace streaming_profiler
