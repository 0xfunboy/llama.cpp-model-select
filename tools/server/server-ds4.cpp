#include "server-ds4.h"

#include "common.h"
#include "server-common.h"
#include "server-models.h"

#include <cpp-httplib/httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifndef LLAMA_DS4_EVAL_CASES_PATH
#define LLAMA_DS4_EVAL_CASES_PATH "tools/server/ds4-eval-cases.json"
#endif

#ifndef LLAMA_DS4_REPORTS_DIR
#define LLAMA_DS4_REPORTS_DIR "tools/ui/static/reports"
#endif

namespace {

static constexpr const char * DS4_CHILD_HOST = "127.0.0.1";
static constexpr int DS4_HTTP_TIMEOUT_SEC = 600;
static constexpr size_t DS4_MAX_EVENTS = 20000;

struct ds4_case {
    std::string source;
    std::string id;
    std::string domain;
    std::string title;
    std::string question;
    std::vector<std::string> choices;
    std::string answer;
};

struct ds4_event {
    uint64_t seq = 0;
    std::string event;
    json data;
};

struct ds4_job {
    std::string id;
    std::string kind;
    std::string model_selector;
    std::string status = "queued";
    std::string error;
    int current = 0;
    int total = 0;
    uint64_t next_seq = 1;
    bool finished = false;
    json report = json::object();

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<ds4_event> events;
};

struct child_json_response {
    int status = 0;
    json body = json::object();
    std::string raw;
    std::string error;
};

struct stream_chat_result {
    bool ok = false;
    int status = 0;
    std::string content;
    std::string reasoning;
    json usage = json::object();
    json timings = json::object();
    std::string error;
};

static void ds4_res_ok(std::unique_ptr<server_http_res> & res, const json & response_data) {
    res->status = 200;
    res->data = safe_json_to_str(response_data);
}

static void ds4_res_err(std::unique_ptr<server_http_res> & res, const json & error_data) {
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

static bool starts_with(const std::string & text, const std::string & prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

static bool ends_with(const std::string & text, const std::string & suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool is_compsec_case(const ds4_case & tc) {
    return tc.source == "COMPSEC" || starts_with(tc.id, "compsec-");
}

static std::string isoish_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static json model_meta_to_ds4_json(const server_model_meta & meta) {
    std::string model_path;
    meta.preset.get_option("LLAMA_ARG_MODEL", model_path);

    json status = {
        {"value", server_model_status_to_string(meta.status)},
        {"loaded", meta.is_ready()},
        {"running", meta.is_running()},
    };

    return {
        {"id", meta.name},
        {"aliases", meta.aliases},
        {"tags", meta.tags},
        {"path", model_path},
        {"loadable", !model_path.empty()},
        {"source", server_model_source_to_string(meta.source)},
        {"status", status},
    };
}

static bool line_is_safe_report_id(const std::string & id) {
    if (id.empty()) {
        return false;
    }
    for (unsigned char c : id) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return id.find("..") == std::string::npos;
}

static std::string header_value_ci_ds4(const server_http_req & req, const std::string & name) {
    const std::string needle = lower_copy(name);
    for (const auto & [key, value] : req.headers) {
        if (lower_copy(key) == needle) {
            return value;
        }
    }
    return "";
}

static std::string id_from_req(const server_http_req & req) {
    std::string id = req.get_param("id");
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-Job-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-Report-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-Bench-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-Bench-Job-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-Bench-Report-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-DS4-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-DS4-Job-Id");
    }
    if (id.empty()) {
        id = header_value_ci_ds4(req, "X-DS4-Report-Id");
    }
    if (!id.empty() || req.body.empty()) {
        return id;
    }
    try {
        json body = json::parse(req.body);
        id = json_value(body, "id", std::string());
        if (id.empty()) {
            id = json_value(body, "job_id", std::string());
        }
        if (id.empty()) {
            id = json_value(body, "report_id", std::string());
        }
    } catch (const std::exception &) {
        return "";
    }
    return id;
}

static std::filesystem::path reports_dir() {
    return std::filesystem::path(LLAMA_DS4_REPORTS_DIR);
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

static std::vector<ds4_case> load_eval_cases() {
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(LLAMA_DS4_EVAL_CASES_PATH),
        std::filesystem::current_path() / "tools/server/ds4-eval-cases.json",
        std::filesystem::path("/home/cooper/llama.cpp-model-select/tools/server/ds4-eval-cases.json"),
    };

    std::string raw;
    std::filesystem::path used;
    for (const auto & path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        raw = read_text_file(path);
        used = path;
        break;
    }

    if (raw.empty()) {
        throw std::runtime_error("DS4 eval cases JSON not found");
    }

    json root = json::parse(raw);
    std::vector<ds4_case> out;
    for (const auto & item : root.at("cases")) {
        ds4_case tc;
        tc.source = json_value(item, "source", std::string());
        tc.id = json_value(item, "id", std::string());
        tc.domain = json_value(item, "domain", std::string());
        tc.title = json_value(item, "title", std::string());
        tc.question = json_value(item, "question", std::string());
        tc.answer = json_value(item, "answer", std::string());
        if (item.contains("choices") && item["choices"].is_array()) {
            for (const auto & choice : item["choices"]) {
                tc.choices.push_back(choice.get<std::string>());
            }
        }
        out.push_back(std::move(tc));
    }

    SRV_INF("loaded %zu DS4 eval cases from %s\n", out.size(), used.string().c_str());
    return out;
}

static std::string eval_system_prompt() {
    return "You are solving a hard benchmark question. Reason carefully. "
           "The final answer must follow the requested format exactly.";
}

static std::string build_question_prompt(const ds4_case & tc) {
    std::ostringstream out;
    out << tc.question << "\n";
    if (!tc.choices.empty()) {
        out << "\nChoices:\n";
        for (size_t i = 0; i < tc.choices.size(); i++) {
            out << (char) ('A' + (int) i) << ". " << tc.choices[i] << "\n";
        }
        out << "\nSolve the question. At the end, write exactly one final line in this "
               "format and do not write anything after it:\n"
               "Answer: <letter>";
    } else if (is_compsec_case(tc)) {
        out << "\nAt the end, write exactly one final line in this format and do not "
               "write anything after it:\n"
               "Answer: <line number or comma-separated line numbers>";
    } else {
        out << "\nSolve the problem. At the end, write exactly one final line in this "
               "format and do not write anything after it:\n"
               "Answer: <integer>";
    }
    return out.str();
}

static bool is_letter_boundary(char before, char after) {
    return !std::isalpha((unsigned char) before) && !std::isalpha((unsigned char) after);
}

static size_t find_case_insensitive(const std::string & hay, const std::string & needle, size_t start = 0) {
    if (needle.empty()) {
        return start <= hay.size() ? start : std::string::npos;
    }
    const std::string low_hay = lower_copy(hay);
    const std::string low_needle = lower_copy(needle);
    return low_hay.find(low_needle, start);
}

static size_t find_last_answer_marker(const std::string & visible) {
    size_t last = std::string::npos;
    const std::string word = "answer";
    const std::string low = lower_copy(visible);
    for (size_t p = 0; p + word.size() <= low.size(); p++) {
        if (low.compare(p, word.size(), word) != 0) {
            continue;
        }
        const char before = p == 0 ? ' ' : visible[p - 1];
        const char after = p + word.size() < visible.size() ? visible[p + word.size()] : ' ';
        if (!is_letter_boundary(before, after)) {
            continue;
        }
        size_t q = p + word.size();
        while (q < visible.size() && std::isspace((unsigned char) visible[q])) {
            q++;
        }
        if (q < visible.size() && visible[q] == ':') {
            last = p;
        }
    }
    return last != std::string::npos ? last : find_case_insensitive(visible, "answer");
}

static bool mc_letter_is_negated(const std::string & line_start, size_t letter_pos) {
    size_t p = letter_pos;
    while (p > 0) {
        const char c = line_start[p - 1];
        if (c == '\n') {
            return false;
        }
        if (c == ' ' || c == '\t' || c == ',' || c == ';') {
            p--;
        } else {
            break;
        }
    }

    const size_t wend = p;
    while (p > 0 && (std::isalpha((unsigned char) line_start[p - 1]) || line_start[p - 1] == '\'')) {
        p--;
    }
    const std::string w = lower_copy(line_start.substr(p, wend - p));
    if (w.empty() || w.size() >= 16) {
        return false;
    }
    if (w.size() >= 3 && ends_with(w, "n't")) {
        return true;
    }

    static const std::set<std::string> cues = {
        "not", "except", "excluding", "exclude", "excludes",
        "eliminate", "eliminates", "eliminated",
        "reject", "rejects", "rejected", "rejecting",
    };
    if (cues.count(w)) {
        return true;
    }

    if (w == "out") {
        size_t q = p;
        while (q > 0 && (line_start[q - 1] == ' ' || line_start[q - 1] == '\t')) {
            q--;
        }
        const size_t rend = q;
        while (q > 0 && std::isalpha((unsigned char) line_start[q - 1])) {
            q--;
        }
        const std::string r = lower_copy(line_start.substr(q, rend - q));
        return r == "rule" || r == "rules" || r == "ruled";
    }
    return false;
}

static std::string visible_after_think(const std::string & generated) {
    const size_t pos = generated.find("</think>");
    if (pos == std::string::npos) {
        return generated;
    }
    return generated.substr(pos + 8);
}

static char find_answer_letter(const std::string & generated, int nchoices) {
    if (nchoices <= 0) {
        return '?';
    }
    const std::string visible = visible_after_think(generated);
    const char max_answer = (char) ('A' + nchoices - 1);
    const size_t answer = find_last_answer_marker(visible);
    if (answer != std::string::npos) {
        const size_t end = std::min(visible.size(), answer + 96);
        const std::string answer_line = visible.substr(answer, end - answer);
        for (size_t i = 0; i < answer_line.size(); i++) {
            const char c = (char) std::toupper((unsigned char) answer_line[i]);
            if (c < 'A' || c > max_answer) {
                continue;
            }
            const char before = i == 0 ? ' ' : answer_line[i - 1];
            const char after = i + 1 < answer_line.size() ? answer_line[i + 1] : ' ';
            if (!is_letter_boundary(before, after)) {
                continue;
            }
            if (after == '\'') {
                continue;
            }
            if (after == ' ' || after == '\t') {
                size_t w = i + 1;
                while (w < answer_line.size() && (answer_line[w] == ' ' || answer_line[w] == '\t')) {
                    w++;
                }
                if (w < answer_line.size() && std::islower((unsigned char) answer_line[w])) {
                    continue;
                }
            }
            if (mc_letter_is_negated(answer_line, i)) {
                continue;
            }
            return c;
        }
    }

    for (size_t p = visible.size(); p > 0; p--) {
        const char c = (char) std::toupper((unsigned char) visible[p - 1]);
        if (c >= 'A' && c <= max_answer) {
            const char before = p == 1 ? ' ' : visible[p - 2];
            const char after = p < visible.size() ? visible[p] : ' ';
            if (is_letter_boundary(before, after)) {
                return c;
            }
        }
    }
    return '?';
}

static std::string normalize_integer_answer(std::string text) {
    text = trim_copy(text);
    size_t i = 0;
    while (i + 1 < text.size() && text[i] == '0' && std::isdigit((unsigned char) text[i + 1])) {
        i++;
    }
    return text.substr(i);
}

static bool scan_first_integer(const std::string & text, std::string & out) {
    for (size_t p = 0; p < text.size(); p++) {
        if (!std::isdigit((unsigned char) text[p])) {
            continue;
        }
        size_t q = p + 1;
        while (q < text.size() && std::isdigit((unsigned char) text[q])) {
            q++;
        }
        out = normalize_integer_answer(text.substr(p, q - p));
        return true;
    }
    return false;
}

static std::string find_integer_answer(const std::string & generated) {
    const std::string visible = visible_after_think(generated);
    const size_t answer = find_last_answer_marker(visible);
    if (answer != std::string::npos) {
        size_t end = std::min(visible.size(), answer + 160);
        const size_t nl = visible.find('\n', answer);
        if (nl != std::string::npos && nl < end) {
            end = nl;
        }
        const std::string line = visible.substr(answer, end - answer);
        const size_t eq = line.rfind('=');
        std::string out;
        if (scan_first_integer(eq == std::string::npos ? line : line.substr(eq + 1), out)) {
            return out;
        }
        if (eq != std::string::npos && scan_first_integer(line, out)) {
            return out;
        }
    }

    std::string last = "?";
    for (size_t p = 0; p < visible.size(); p++) {
        if (!std::isdigit((unsigned char) visible[p])) {
            continue;
        }
        size_t q = p + 1;
        while (q < visible.size() && std::isdigit((unsigned char) visible[q])) {
            q++;
        }
        last = normalize_integer_answer(visible.substr(p, q - p));
        p = q - 1;
    }
    return last;
}

static std::string normalize_compsec_line_spec(const std::string & text) {
    std::ostringstream out;
    bool any = false;
    for (size_t p = 0; p < text.size(); p++) {
        if (!std::isdigit((unsigned char) text[p])) {
            continue;
        }
        if (any) {
            out << ',';
        }
        any = true;
        while (p < text.size() && std::isdigit((unsigned char) text[p])) {
            out << text[p++];
        }
        while (p < text.size() && std::isspace((unsigned char) text[p])) {
            p++;
        }
        if (p < text.size() && text[p] == '-') {
            out << '-';
            p++;
            while (p < text.size() && std::isspace((unsigned char) text[p])) {
                p++;
            }
            while (p < text.size() && std::isdigit((unsigned char) text[p])) {
                out << text[p++];
            }
        }
        if (p >= text.size()) {
            break;
        }
    }
    std::string result = out.str();
    while (!result.empty() && (result.back() == ',' || result.back() == '-')) {
        result.pop_back();
    }
    return result.empty() ? "?" : result;
}

static std::string find_compsec_answer(const std::string & generated) {
    const std::string visible = visible_after_think(generated);
    const size_t answer = find_last_answer_marker(visible);
    if (answer != std::string::npos) {
        size_t end = std::min(visible.size(), answer + 160);
        const size_t nl = visible.find('\n', answer);
        if (nl != std::string::npos && nl < end) {
            end = nl;
        }
        const std::string got = normalize_compsec_line_spec(visible.substr(answer, end - answer));
        if (got != "?") {
            return got;
        }
    }
    return find_integer_answer(generated);
}

static bool parse_line_spec(const std::string & spec, std::vector<bool> & set) {
    bool any = false;
    for (size_t p = 0; p < spec.size();) {
        while (p < spec.size() && !std::isdigit((unsigned char) spec[p])) {
            p++;
        }
        if (p >= spec.size()) {
            break;
        }
        long a = 0;
        while (p < spec.size() && std::isdigit((unsigned char) spec[p])) {
            a = a * 10 + (spec[p++] - '0');
        }
        long b = a;
        if (p < spec.size() && spec[p] == '-') {
            p++;
            b = 0;
            while (p < spec.size() && std::isdigit((unsigned char) spec[p])) {
                b = b * 10 + (spec[p++] - '0');
            }
        }
        if (a > b) {
            std::swap(a, b);
        }
        a = std::max<long>(0, a);
        b = std::min<long>((long) set.size() - 1, b);
        for (long i = a; i <= b; i++) {
            set[(size_t) i] = true;
            any = true;
        }
    }
    return any;
}

static bool compsec_answer_matches(const std::string & expected_spec, const std::string & got_spec) {
    std::vector<bool> expected(256, false);
    std::vector<bool> got(256, false);
    if (!parse_line_spec(expected_spec, expected)) {
        return false;
    }
    if (!parse_line_spec(got_spec, got)) {
        return false;
    }
    bool hit = false;
    for (size_t i = 0; i < got.size(); i++) {
        if (!got[i]) {
            continue;
        }
        if (!expected[i]) {
            return false;
        }
        hit = true;
    }
    return hit;
}

static std::string find_case_answer(const ds4_case & tc, const std::string & generated) {
    if (!tc.choices.empty()) {
        return std::string(1, find_answer_letter(generated, (int) tc.choices.size()));
    }
    if (is_compsec_case(tc)) {
        return find_compsec_answer(generated);
    }
    return find_integer_answer(generated);
}

static bool answer_matches(const ds4_case & tc, const std::string & got) {
    if (!tc.choices.empty()) {
        return !got.empty() && !tc.answer.empty() && got[0] == tc.answer[0];
    }
    if (is_compsec_case(tc)) {
        return compsec_answer_matches(tc.answer, got);
    }
    return got == normalize_integer_answer(tc.answer);
}

static void add_auth_header(httplib::Request & req, const common_params & params) {
    if (!params.api_keys.empty()) {
        req.set_header("Authorization", "Bearer " + params.api_keys.front());
    }
}

static void configure_child_client(httplib::Client & cli) {
    cli.set_connection_timeout(30, 0);
    cli.set_read_timeout(DS4_HTTP_TIMEOUT_SEC, 0);
    cli.set_write_timeout(DS4_HTTP_TIMEOUT_SEC, 0);
}

static child_json_response post_child_json(const common_params & params, int port, const std::string & path, const json & body) {
    child_json_response out;
    httplib::Client cli(DS4_CHILD_HOST, port);
    configure_child_client(cli);

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.body = safe_json_to_str(body);
    req.set_header("Content-Type", "application/json");
    add_auth_header(req, params);

    auto result = cli.send(req);
    if (result.error() != httplib::Error::Success) {
        out.error = httplib::to_string(result.error());
        return out;
    }

    out.status = result->status;
    out.raw = result->body;
    try {
        out.body = json::parse(out.raw);
    } catch (const std::exception & e) {
        out.error = e.what();
    }
    return out;
}

static int tokenize_count(const common_params & params, int port, const std::string & text) {
    if (text.empty()) {
        return 0;
    }
    auto resp = post_child_json(params, port, "/tokenize", {
        {"content", text},
        {"add_special", false},
        {"parse_special", false},
    });
    if (resp.status != 200 || !resp.body.contains("tokens") || !resp.body["tokens"].is_array()) {
        return -1;
    }
    return (int) resp.body["tokens"].size();
}

static void process_sse_buffer(
        std::string & buffer,
        const std::function<void(const json &)> & on_json,
        bool & done) {
    for (;;) {
        const size_t pos = buffer.find('\n');
        if (pos == std::string::npos) {
            return;
        }
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!starts_with(line, "data:")) {
            continue;
        }
        std::string payload = trim_copy(line.substr(5));
        if (payload.empty()) {
            continue;
        }
        if (payload == "[DONE]") {
            done = true;
            continue;
        }
        try {
            on_json(json::parse(payload));
        } catch (const std::exception &) {
            continue;
        }
    }
}

static stream_chat_result stream_child_chat(
        const common_params & params,
        int port,
        const json & body,
        const std::function<void(const std::string &, const std::string &)> & on_delta) {
    stream_chat_result out;
    httplib::Client cli(DS4_CHILD_HOST, port);
    configure_child_client(cli);

    httplib::Request req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.body = safe_json_to_str(body);
    req.set_header("Content-Type", "application/json");
    add_auth_header(req, params);

    std::string raw_error_body;
    std::string sse_buffer;
    bool done = false;

    req.response_handler = [&out](const httplib::Response & response) {
        out.status = response.status;
        return true;
    };

    req.content_receiver = [&](const char * data, size_t data_length, size_t, size_t) {
        if (out.status != 200) {
            raw_error_body.append(data, data_length);
            return true;
        }
        sse_buffer.append(data, data_length);
        process_sse_buffer(sse_buffer, [&](const json & chunk) {
            if (chunk.contains("error")) {
                out.error = safe_json_to_str(chunk["error"]);
                return;
            }
            if (chunk.contains("usage")) {
                out.usage = chunk["usage"];
            }
            if (chunk.contains("timings")) {
                out.timings = chunk["timings"];
            }
            if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty()) {
                return;
            }
            const json & choice = chunk["choices"][0];
            json delta = json_value(choice, "delta", json::object());
            std::string content = json_value(delta, "content", std::string());
            std::string reasoning = json_value(delta, "reasoning_content", std::string());
            if (reasoning.empty()) {
                reasoning = json_value(delta, "reasoning", std::string());
            }
            if (!reasoning.empty()) {
                out.reasoning += reasoning;
                on_delta("reasoning", reasoning);
            }
            if (!content.empty()) {
                out.content += content;
                on_delta("content", content);
            }
            if (choice.contains("timings")) {
                out.timings = choice["timings"];
            }
        }, done);
        return true;
    };

    auto result = cli.send(req);
    if (result.error() != httplib::Error::Success) {
        out.error = httplib::to_string(result.error());
        return out;
    }
    if (out.status != 200) {
        out.error = raw_error_body.empty()
            ? string_format("child server returned HTTP %d", out.status)
            : raw_error_body;
        return out;
    }
    out.ok = out.error.empty();
    return out;
}

static std::string read_bench_prompt() {
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("/home/cooper/ds4/speed-bench/promessi_sposi.txt"),
        std::filesystem::current_path() / "ds4/speed-bench/promessi_sposi.txt",
    };
    for (const auto & path : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return read_text_file(path);
        }
    }

    std::string fallback =
        "Quel ramo del lago di Como, che volge a mezzogiorno, tra due catene non interrotte "
        "di monti, tutto a seni e a golfi, a seconda dello sporgere e del rientrare di quelli, "
        "vien, quasi a un tratto, a ristringersi, e a prender corso e figura di fiume. ";
    std::string out;
    out.reserve(fallback.size() * 4096);
    for (int i = 0; i < 4096; i++) {
        out += fallback;
    }
    return out;
}

} // namespace

struct server_ds4_routes::impl {
    server_models_routes & router;
    std::mutex jobs_mutex;
    std::map<std::string, std::shared_ptr<ds4_job>> jobs;
    std::atomic<uint64_t> job_counter{0};
    bool active_job = false;

    explicit impl(server_models_routes & router) : router(router) {}

    void publish(const std::shared_ptr<ds4_job> & job, const std::string & event, json data) {
        if (!data.is_object()) {
            data = json::object({{"value", data}});
        }
        std::lock_guard<std::mutex> lock(job->mutex);
        data["job_id"] = job->id;
        data["kind"] = job->kind;
        data["status"] = job->status;
        data["current"] = job->current;
        data["total"] = job->total;
        data["ts"] = isoish_timestamp();

        ds4_event ev;
        ev.seq = job->next_seq++;
        ev.event = event;
        ev.data = std::move(data);
        ev.data["seq"] = ev.seq;
        job->events.push_back(std::move(ev));
        while (job->events.size() > DS4_MAX_EVENTS) {
            job->events.pop_front();
        }
        job->cv.notify_all();
    }

    void log(const std::shared_ptr<ds4_job> & job, const std::string & line, const std::string & stream = "stdout") {
        publish(job, "log", {{"stream", stream}, {"text", line}});
    }

    void finish_job(const std::shared_ptr<ds4_job> & job, const std::string & status, const std::string & error = "") {
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->status = status;
            job->error = error;
            job->finished = true;
        }
        publish(job, "done", {{"error", error}});
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            active_job = false;
        }
    }

    bool switch_model_for_job(const std::string & requested, std::string & resolved, json & details, std::string & error) {
        struct guard_t {
            server_models_routes & routes;
            bool active = false;
            explicit guard_t(server_models_routes & routes) : routes(routes) {}
            bool start() {
                std::lock_guard<std::mutex> lk(routes.switch_mutex);
                if (routes.switch_in_progress) {
                    return false;
                }
                routes.switch_in_progress = true;
                active = true;
                return true;
            }
            ~guard_t() {
                if (!active) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(routes.switch_mutex);
                    routes.switch_in_progress = false;
                }
                routes.switch_cv.notify_all();
            }
        } guard(router);

        if (!guard.start()) {
            error = "model switch already in progress";
            return false;
        }

        auto target = router.models.get_meta(requested);
        if (!target.has_value()) {
            error = string_format("model '%s' not found", requested.c_str());
            return false;
        }
        resolved = target->name;

        std::vector<std::string> to_unload;
        bool target_running = false;
        for (const auto & meta : router.models.get_all_meta()) {
            if (meta.name == resolved && meta.is_running()) {
                target_running = true;
            }
            if ((meta.is_running() || meta.status == SERVER_MODEL_STATUS_DOWNLOADING) && meta.name != resolved) {
                to_unload.push_back(meta.name);
            }
        }

        for (const auto & name : to_unload) {
            router.models.unload(name);
        }
        for (const auto & name : to_unload) {
            router.models.wait(name, [](const server_model_meta & meta) {
                return !meta.is_running() && meta.status != SERVER_MODEL_STATUS_DOWNLOADING;
            });
        }

        if (!target_running) {
            router.models.ensure_model_ready(resolved);
        }

        auto final_meta = router.models.get_meta(resolved);
        const std::string final_status = final_meta.has_value()
            ? server_model_status_to_string(final_meta->status)
            : "missing";
        if (!final_meta.has_value() || (!final_meta->is_ready() && final_meta->status != SERVER_MODEL_STATUS_SLEEPING)) {
            error = string_format("model '%s' did not become ready (status: %s)", resolved.c_str(), final_status.c_str());
            return false;
        }

        details = {
            {"model", resolved},
            {"status", final_status},
            {"unloaded", to_unload},
            {"port", final_meta->port},
        };
        return true;
    }

    std::vector<std::string> expand_model_selector(const std::string & selector) {
        std::vector<std::string> names;
        if (selector == "ALL") {
            for (const auto & meta : router.models.get_all_meta()) {
                names.push_back(meta.name);
            }
        } else {
            auto meta = router.models.get_meta(selector);
            if (meta.has_value()) {
                names.push_back(meta->name);
            } else {
                names.push_back(selector);
            }
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    std::shared_ptr<ds4_job> create_job(const std::string & kind, const std::string & selector, std::string & error) {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        if (active_job) {
            error = "another DS4 job is already running";
            return nullptr;
        }
        active_job = true;

        auto job = std::make_shared<ds4_job>();
        job->kind = kind;
        job->model_selector = selector;
        job->id = string_format("ds4-%s-%" PRId64 "-%" PRIu64,
                kind.c_str(),
                (int64_t) ggml_time_ms(),
                job_counter.fetch_add(1, std::memory_order_relaxed));
        jobs[job->id] = job;
        return job;
    }

    void save_report(const std::shared_ptr<ds4_job> & job) {
        std::filesystem::create_directories(reports_dir());
        const auto path = reports_dir() / (job->id + ".json");
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw std::runtime_error(string_format("cannot write report '%s'", path.string().c_str()));
        }
        out << job->report.dump(2) << "\n";
    }

    void run_eval_job(const std::shared_ptr<ds4_job> & job, json request) {
        try {
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->status = "running";
            }
            const int max_tokens = std::max(1, json_value(request, "max_tokens", 16000));
            const int think_budget = std::max(1, json_value(request, "thinking_budget_tokens", max_tokens));
            const int timeout_tokens = max_tokens;
            const int limit = json_value(request, "limit", 0);
            const bool thinking = json_value(request, "thinking", true);
            const double temperature = json_value(request, "temperature", 0.0);
            const std::string selector = json_value(request, "model", std::string("ALL"));
            const auto model_names = expand_model_selector(selector);
            auto cases = load_eval_cases();
            if (limit > 0 && limit < (int) cases.size()) {
                cases.resize((size_t) limit);
            }

            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->total = (int) (cases.size() * std::max<size_t>(1, model_names.size()));
            }
            publish(job, "status", {{"message", "DS4-Eval started"}, {"models", model_names}, {"cases", cases.size()}});

            json report = {
                {"id", job->id},
                {"kind", "eval"},
                {"created_at", isoish_timestamp()},
                {"model_selector", selector},
                {"max_tokens", max_tokens},
                {"thinking_budget_tokens", think_budget},
                {"thinking", thinking},
                {"temperature", temperature},
                {"results", json::array()},
                {"summary", json::object()},
            };

            int total_pass = 0;
            int total_fail = 0;
            for (const auto & model_name : model_names) {
                log(job, string_format("\033[1;36m==> Loading %s\033[0m\n", model_name.c_str()));
                std::string resolved;
                json switch_details;
                std::string switch_error;
                if (!switch_model_for_job(model_name, resolved, switch_details, switch_error)) {
                    throw std::runtime_error(switch_error);
                }
                auto meta = router.models.get_meta(resolved);
                if (!meta.has_value()) {
                    throw std::runtime_error("model disappeared after switch");
                }
                const int port = meta->port;

                int model_pass = 0;
                int model_fail = 0;
                int case_index = 0;
                for (const auto & tc : cases) {
                    case_index++;
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->current++;
                    }
                    log(job, string_format("\n\033[1m[%s] case %d/%zu %s / %s\033[0m\n",
                            resolved.c_str(), case_index, cases.size(), tc.source.c_str(), tc.id.c_str()));

                    const std::string user_prompt = build_question_prompt(tc);
                    const int64_t start_ms = ggml_time_ms();
                    bool think_open = false;

                    json body = {
                        {"model", resolved},
                        {"stream", true},
                        {"temperature", temperature},
                        {"top_p", 1.0},
                        {"top_k", 1},
                        {"max_tokens", timeout_tokens},
                        {"cache_prompt", false},
                        {"reasoning_format", "deepseek"},
                        {"reasoning_control", true},
                        {"thinking_budget_tokens", think_budget},
                        {"chat_template_kwargs", {{"enable_thinking", thinking}}},
                        {"messages", json::array({
                            {{"role", "system"}, {"content", eval_system_prompt()}},
                            {{"role", "user"}, {"content", user_prompt}},
                        })},
                    };

                    stream_chat_result generated = stream_child_chat(router.params, port, body,
                        [&](const std::string & channel, const std::string & delta) {
                            if (channel == "reasoning") {
                                if (!think_open) {
                                    think_open = true;
                                    log(job, "\033[2m<think>\n", "token");
                                }
                                log(job, "\033[2m" + delta + "\033[0m", "token");
                            } else {
                                if (think_open) {
                                    think_open = false;
                                    log(job, "\n\033[2m</think>\033[0m\n", "token");
                                }
                                log(job, delta, "token");
                            }
                        });
                    if (think_open) {
                        log(job, "\n\033[2m</think>\033[0m\n", "token");
                    }
                    if (!generated.ok) {
                        throw std::runtime_error(string_format("generation failed for %s: %s",
                                tc.id.c_str(), generated.error.c_str()));
                    }

                    const int64_t elapsed_ms = std::max<int64_t>(1, ggml_time_ms() - start_ms);
                    const std::string combined = generated.reasoning.empty()
                        ? generated.content
                        : "<think>" + generated.reasoning + "</think>" + generated.content;
                    const std::string got = find_case_answer(tc, combined);
                    const bool pass = answer_matches(tc, got);
                    const int think_tokens = tokenize_count(router.params, port, generated.reasoning);
                    const int content_tokens = tokenize_count(router.params, port, generated.content);
                    const double toks_per_sec = (think_tokens + content_tokens) > 0
                        ? (double) (think_tokens + content_tokens) * 1000.0 / (double) elapsed_ms
                        : 0.0;

                    if (pass) {
                        model_pass++;
                        total_pass++;
                    } else {
                        model_fail++;
                        total_fail++;
                    }

                    json row = {
                        {"model", resolved},
                        {"case_index", case_index},
                        {"source", tc.source},
                        {"id", tc.id},
                        {"domain", tc.domain},
                        {"title", tc.title},
                        {"expected", tc.answer},
                        {"got", got},
                        {"pass", pass},
                        {"elapsed_ms", elapsed_ms},
                        {"reasoning_tokens", think_tokens},
                        {"content_tokens", content_tokens},
                        {"tokens_per_second", toks_per_sec},
                        {"usage", generated.usage},
                        {"timings", generated.timings},
                        {"reasoning", generated.reasoning},
                        {"content", generated.content},
                    };
                    report["results"].push_back(row);
                    publish(job, "case", row);
                    log(job, string_format("\n\033[%sm%s expected=%s got=%s elapsed=%.2fs tps=%.2f\033[0m\n",
                            pass ? "1;32" : "1;31",
                            pass ? "PASS" : "FAIL",
                            tc.answer.c_str(),
                            got.c_str(),
                            (double) elapsed_ms / 1000.0,
                            toks_per_sec));
                }
                publish(job, "model-summary", {
                    {"model", resolved},
                    {"pass", model_pass},
                    {"fail", model_fail},
                    {"score", cases.empty() ? 0.0 : (double) model_pass / (double) cases.size()},
                });
            }

            report["summary"] = {
                {"pass", total_pass},
                {"fail", total_fail},
                {"total", total_pass + total_fail},
                {"score", (total_pass + total_fail) ? (double) total_pass / (double) (total_pass + total_fail) : 0.0},
            };
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->report = report;
            }
            save_report(job);
            finish_job(job, "completed");
        } catch (const std::exception & e) {
            log(job, std::string("\n\033[1;31mERROR: ") + e.what() + "\033[0m\n", "stderr");
            finish_job(job, "failed", e.what());
        }
    }

    void run_bench_job(const std::shared_ptr<ds4_job> & job, json request) {
        try {
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->status = "running";
            }
            const std::string selector = json_value(request, "model", std::string("ALL"));
            const int ctx_start = std::max(128, json_value(request, "ctx_start", 2048));
            const int ctx_max = std::max(ctx_start, json_value(request, "ctx_max", 131072));
            const int ctx_step = std::max(128, json_value(request, "ctx_step", 4096));
            const int gen_tokens = std::max(1, json_value(request, "gen_tokens", 64));
            const auto model_names = expand_model_selector(selector);
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->total = (int) model_names.size();
            }
            publish(job, "status", {{"message", "DS4-Bench started"}, {"models", model_names}});

            json report = {
                {"id", job->id},
                {"kind", "bench"},
                {"created_at", isoish_timestamp()},
                {"model_selector", selector},
                {"ctx_start", ctx_start},
                {"ctx_max", ctx_max},
                {"ctx_step", ctx_step},
                {"gen_tokens", gen_tokens},
                {"results", json::array()},
                {"summary", json::object()},
            };

            const std::string prompt_text = read_bench_prompt();
            for (const auto & model_name : model_names) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->current++;
                }
                log(job, string_format("\033[1;36m==> Loading %s\033[0m\n", model_name.c_str()));
                std::string resolved;
                json switch_details;
                std::string switch_error;
                if (!switch_model_for_job(model_name, resolved, switch_details, switch_error)) {
                    throw std::runtime_error(switch_error);
                }
                auto meta = router.models.get_meta(resolved);
                if (!meta.has_value()) {
                    throw std::runtime_error("model disappeared after switch");
                }
                const int port = meta->port;

                log(job, "Tokenizing Promessi Sposi corpus...\n");
                auto tok_resp = post_child_json(router.params, port, "/tokenize", {
                    {"content", prompt_text},
                    {"add_special", false},
                    {"parse_special", false},
                });
                if (tok_resp.status != 200 || !tok_resp.body.contains("tokens")) {
                    throw std::runtime_error("tokenize failed: " + tok_resp.raw);
                }

                std::vector<int> tokens;
                for (const auto & tok : tok_resp.body["tokens"]) {
                    tokens.push_back(tok.get<int>());
                }
                if ((int) tokens.size() < ctx_start) {
                    throw std::runtime_error("bench corpus is too short after tokenization");
                }

                int last_ctx = std::min(ctx_max, (int) tokens.size());
                json model_rows = json::array();
                for (int ctx = ctx_start; ctx <= last_ctx; ctx += ctx_step) {
                    json prefix = json::array();
                    for (int i = 0; i < ctx; i++) {
                        prefix.push_back(tokens[(size_t) i]);
                    }

                    auto prefill_start = std::chrono::steady_clock::now();
                    auto prefill = post_child_json(router.params, port, "/completion", {
                        {"prompt", prefix},
                        {"n_predict", 0},
                        {"cache_prompt", false},
                        {"temperature", 0.0},
                        {"stream", false},
                        {"timings_per_token", false},
                    });
                    auto prefill_end = std::chrono::steady_clock::now();
                    if (prefill.status != 200) {
                        throw std::runtime_error("prefill failed: " + prefill.raw);
                    }

                    auto decode_start = std::chrono::steady_clock::now();
                    auto decode = post_child_json(router.params, port, "/completion", {
                        {"prompt", prefix},
                        {"n_predict", gen_tokens},
                        {"cache_prompt", true},
                        {"temperature", 0.0},
                        {"stream", false},
                        {"timings_per_token", false},
                    });
                    auto decode_end = std::chrono::steady_clock::now();
                    if (decode.status != 200) {
                        throw std::runtime_error("decode failed: " + decode.raw);
                    }

                    const double prefill_sec = std::max(0.001,
                            std::chrono::duration<double>(prefill_end - prefill_start).count());
                    const double decode_sec = std::max(0.001,
                            std::chrono::duration<double>(decode_end - decode_start).count());
                    json timings = json_value(decode.body, "timings", json::object());
                    json prefill_timings = json_value(prefill.body, "timings", json::object());
                    const double prompt_tps = json_value(prefill_timings, "prompt_per_second", (double) ctx / prefill_sec);
                    const double decode_tps = json_value(timings, "predicted_per_second", (double) gen_tokens / decode_sec);

                    json row = {
                        {"model", resolved},
                        {"ctx", ctx},
                        {"prompt_tokens", ctx},
                        {"gen_tokens", gen_tokens},
                        {"prompt_seconds", prefill_sec},
                        {"decode_seconds", decode_sec},
                        {"prompt_tokens_per_second", prompt_tps},
                        {"decode_tokens_per_second", decode_tps},
                        {"timings", timings},
                        {"prefill_timings", prefill_timings},
                    };
                    report["results"].push_back(row);
                    model_rows.push_back(row);
                    publish(job, "bench-row", row);
                    log(job, string_format("ctx=%d prompt=%.2f tok/s decode=%.2f tok/s\n",
                            ctx, prompt_tps, decode_tps));

                    if (ctx + ctx_step > last_ctx && ctx != last_ctx && last_ctx <= ctx_max) {
                        ctx = last_ctx - ctx_step;
                    }
                }

                publish(job, "model-summary", {{"model", resolved}, {"rows", model_rows}});
            }

            double best_decode = 0.0;
            double best_prompt = 0.0;
            for (const auto & row : report["results"]) {
                best_decode = std::max(best_decode, json_value(row, "decode_tokens_per_second", 0.0));
                best_prompt = std::max(best_prompt, json_value(row, "prompt_tokens_per_second", 0.0));
            }
            report["summary"] = {
                {"rows", report["results"].size()},
                {"best_decode_tokens_per_second", best_decode},
                {"best_prompt_tokens_per_second", best_prompt},
            };
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->report = report;
            }
            save_report(job);
            finish_job(job, "completed");
        } catch (const std::exception & e) {
            log(job, std::string("\n\033[1;31mERROR: ") + e.what() + "\033[0m\n", "stderr");
            finish_job(job, "failed", e.what());
        }
    }

    server_http_res_ptr start_job(const server_http_req & req, const std::string & kind) {
        auto res = std::make_unique<server_http_res>();
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        const std::string selector = json_value(body, "model", std::string("ALL"));
        std::string error;
        auto job = create_job(kind, selector, error);
        if (!job) {
            ds4_res_err(res, format_error_response(error, ERROR_TYPE_UNAVAILABLE));
            return res;
        }

        publish(job, "status", {{"message", "queued"}});
        std::thread worker([this, job, body, kind]() mutable {
            if (kind == "eval") {
                run_eval_job(job, std::move(body));
            } else {
                run_bench_job(job, std::move(body));
            }
        });
        worker.detach();

        ds4_res_ok(res, {
            {"id", job->id},
            {"kind", kind},
            {"status", job->status},
            {"model", selector},
        });
        return res;
    }

    std::shared_ptr<ds4_job> find_job(const std::string & id) {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        auto it = jobs.find(id);
        if (it == jobs.end()) {
            return nullptr;
        }
        return it->second;
    }

    json job_snapshot(const std::shared_ptr<ds4_job> & job) {
        std::lock_guard<std::mutex> lock(job->mutex);
        return {
            {"id", job->id},
            {"kind", job->kind},
            {"model", job->model_selector},
            {"status", job->status},
            {"error", job->error},
            {"current", job->current},
            {"total", job->total},
            {"finished", job->finished},
            {"report", job->report.is_null() ? json::object() : job->report},
        };
    }

    server_http_res_ptr handle_job_status(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        auto job = find_job(id_from_req(req));
        if (!job) {
            ds4_res_err(res, format_error_response("DS4 job not found", ERROR_TYPE_NOT_FOUND));
            return res;
        }
        ds4_res_ok(res, job_snapshot(job));
        return res;
    }

    server_http_res_ptr handle_job_events(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        auto job = find_job(id_from_req(req));
        if (!job) {
            ds4_res_err(res, format_error_response("DS4 job not found", ERROR_TYPE_NOT_FOUND));
            return res;
        }

        uint64_t since = 0;
        try {
            const std::string raw_since = req.get_param("since", "0");
            since = raw_since.empty() ? 0 : (uint64_t) std::stoull(raw_since);
        } catch (const std::exception &) {
            since = 0;
        }

        res->status = 200;
        res->content_type = "text/event-stream";
        res->headers["Cache-Control"] = "no-cache";
        res->headers["X-Accel-Buffering"] = "no";
        res->next = [job, since, &req](std::string & output) mutable -> bool {
            std::unique_lock<std::mutex> lock(job->mutex);
            job->cv.wait_for(lock, std::chrono::seconds(2), [&]() {
                if (req.should_stop()) {
                    return true;
                }
                for (const auto & event : job->events) {
                    if (event.seq > since) {
                        return true;
                    }
                }
                return job->finished;
            });

            if (req.should_stop()) {
                return false;
            }

            for (const auto & event : job->events) {
                if (event.seq <= since) {
                    continue;
                }
                since = event.seq;
                output = "event: " + event.event + "\n";
                output += "data: " + safe_json_to_str(event.data) + "\n\n";
                return true;
            }

            if (job->finished) {
                return false;
            }
            output = ": keepalive\n\n";
            return true;
        };
        return res;
    }

    server_http_res_ptr handle_report(const server_http_req & req) {
        auto res = std::make_unique<server_http_res>();
        std::string id = id_from_req(req);
        if (!line_is_safe_report_id(id)) {
            ds4_res_err(res, format_error_response("invalid report id", ERROR_TYPE_INVALID_REQUEST));
            return res;
        }
        if (!ends_with(id, ".json")) {
            id += ".json";
        }
        const auto path = reports_dir() / id;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            ds4_res_err(res, format_error_response("DS4 report not found", ERROR_TYPE_NOT_FOUND));
            return res;
        }
        res->status = 200;
        res->data = read_text_file(path);
        return res;
    }

    server_http_res_ptr handle_eval_action_or_start(const server_http_req & req) {
        if (!req.body.empty()) {
            try {
                json body = json::parse(req.body);
                std::string action = json_value(body, "cmd", std::string());
                if (action.empty()) {
                    action = json_value(body, "ds4_mode", std::string());
                }
                if (action.empty()) {
                    action = json_value(body, "ds4_action", std::string());
                }
                if (action.empty()) {
                    action = json_value(body, "action", std::string());
                }
                if (action == "status") {
                    return handle_job_status(req);
                }
                if (action == "events") {
                    return handle_job_events(req);
                }
                if (action == "report") {
                    return handle_report(req);
                }
            } catch (const std::exception &) {
                // Let start_job surface the parse error consistently.
            }
        }
        return start_job(req, "eval");
    }
};

server_ds4_routes::server_ds4_routes(server_models_routes & router)
        : pimpl(std::make_shared<impl>(router)) {
    init_routes();
}

server_ds4_routes::~server_ds4_routes() = default;

void server_ds4_routes::init_routes() {
    get_models = [p = pimpl](const server_http_req & req) {
        if (!req.get_param("reload", "").empty()) {
            p->router.models.load_models();
        }
        auto res = std::make_unique<server_http_res>();
        json models = json::array();
        models.push_back({
            {"id", "ALL"},
            {"source", "virtual"},
            {"status", {{"value", "virtual"}, {"loaded", false}, {"running", false}}},
            {"tags", json::array({"batch"})},
            {"aliases", json::array()},
        });
        for (const auto & meta : p->router.models.get_all_meta()) {
            models.push_back(model_meta_to_ds4_json(meta));
        }
        ds4_res_ok(res, {{"data", models}, {"object", "list"}});
        return res;
    };

    post_run_eval = [p = pimpl](const server_http_req & req) {
        return p->handle_eval_action_or_start(req);
    };

    post_run_bench = [p = pimpl](const server_http_req & req) {
        return p->start_job(req, "bench");
    };

    get_job = [p = pimpl](const server_http_req & req) {
        return p->handle_job_status(req);
    };

    get_job_events = [p = pimpl](const server_http_req & req) {
        return p->handle_job_events(req);
    };

    get_reports = [p = pimpl](const server_http_req &) {
        auto res = std::make_unique<server_http_res>();
        std::filesystem::create_directories(reports_dir());
        json reports = json::array();
        for (const auto & entry : std::filesystem::directory_iterator(reports_dir())) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            try {
                json report = json::parse(read_text_file(entry.path()));
                reports.push_back({
                    {"id", json_value(report, "id", entry.path().stem().string())},
                    {"kind", json_value(report, "kind", std::string())},
                    {"created_at", json_value(report, "created_at", std::string())},
                    {"model_selector", json_value(report, "model_selector", std::string())},
                    {"summary", json_value(report, "summary", json::object())},
                });
            } catch (const std::exception & e) {
                SRV_WRN("skipping DS4 report %s: %s\n", entry.path().string().c_str(), e.what());
            }
        }
        ds4_res_ok(res, {{"data", reports}, {"object", "list"}});
        return res;
    };

    get_report = [p = pimpl](const server_http_req & req) {
        std::string command = lower_copy(header_value_ci_ds4(req, "X-Cmd"));
        if (command.empty()) {
            command = lower_copy(header_value_ci_ds4(req, "X-Bench-Cmd"));
        }
        if (command.empty()) {
            command = lower_copy(header_value_ci_ds4(req, "X-Bench-Command"));
        }
        if (command.empty()) {
            command = lower_copy(header_value_ci_ds4(req, "X-DS4-Command"));
        }
        if (command == "status") {
            return p->handle_job_status(req);
        }
        if (command == "events") {
            return p->handle_job_events(req);
        }
        return p->handle_report(req);
    };
}
