#include "server-persistence.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifndef LLAMA_PLATFORM_DB_PATH
#define LLAMA_PLATFORM_DB_PATH "data/llm-model-select.sqlite"
#endif

#ifndef LLAMA_DS4_REPORTS_DIR
#define LLAMA_DS4_REPORTS_DIR "tools/ui/static/reports"
#endif

#ifndef LLAMA_CALIBER_REPORTS_DIR
#define LLAMA_CALIBER_REPORTS_DIR "tools/ui/static/reports/caliber"
#endif

namespace server_persistence {
namespace {

static std::mutex g_mutex;
static bool g_schema_ready = false;
static bool g_imported_reports = false;

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

static std::string read_text_file(const std::filesystem::path & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to read " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string json_dump(const json & value) {
    return value.is_discarded() ? "null" : value.dump();
}

static std::string json_string_value(const json & value, const std::string & key) {
    if (!value.is_object() || !value.contains(key) || value.at(key).is_null()) {
        return "";
    }
    try {
        if (value.at(key).is_string()) {
            return value.at(key).get<std::string>();
        }
        if (value.at(key).is_number_integer()) {
            return std::to_string(value.at(key).get<int64_t>());
        }
        if (value.at(key).is_number_unsigned()) {
            return std::to_string(value.at(key).get<uint64_t>());
        }
        if (value.at(key).is_number_float()) {
            return std::to_string(value.at(key).get<double>());
        }
        if (value.at(key).is_boolean()) {
            return value.at(key).get<bool>() ? "true" : "false";
        }
    } catch (...) {
    }
    return "";
}

static double json_number_value(const json & value, const std::string & key, double fallback = 0.0) {
    if (!value.is_object() || !value.contains(key) || value.at(key).is_null()) {
        return fallback;
    }
    try {
        if (value.at(key).is_number()) {
            return value.at(key).get<double>();
        }
        if (value.at(key).is_string()) {
            return std::stod(value.at(key).get<std::string>());
        }
    } catch (...) {
    }
    return fallback;
}

static int64_t json_i64_value(const json & value, const std::string & key, int64_t fallback = 0) {
    if (!value.is_object() || !value.contains(key) || value.at(key).is_null()) {
        return fallback;
    }
    try {
        if (value.at(key).is_number_integer()) {
            return value.at(key).get<int64_t>();
        }
        if (value.at(key).is_number_unsigned()) {
            return (int64_t) value.at(key).get<uint64_t>();
        }
        if (value.at(key).is_number_float()) {
            return (int64_t) value.at(key).get<double>();
        }
        if (value.at(key).is_string()) {
            return std::stoll(value.at(key).get<std::string>());
        }
    } catch (...) {
    }
    return fallback;
}

static std::string first_string(const json & value, std::initializer_list<const char *> keys) {
    for (const char * key : keys) {
        const std::string out = json_string_value(value, key);
        if (!out.empty()) {
            return out;
        }
    }
    return "";
}

static double first_number(const json & value, std::initializer_list<const char *> keys, double fallback = 0.0) {
    for (const char * key : keys) {
        if (value.is_object() && value.contains(key) && !value.at(key).is_null()) {
            const double out = json_number_value(value, key, fallback);
            if (out != fallback || fallback == 0.0) {
                return out;
            }
        }
    }
    return fallback;
}

struct db_handle {
    sqlite3 * db = nullptr;
    ~db_handle() {
        if (db) {
            sqlite3_close(db);
        }
    }
};

static void exec_sql(sqlite3 * db, const char * sql) {
    char * err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string message = err ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        throw std::runtime_error(message);
    }
}

static db_handle open_database_locked() {
    const auto path = database_path();
    std::filesystem::create_directories(path.parent_path());

    db_handle handle;
    const int rc = sqlite3_open_v2(path.string().c_str(), &handle.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = handle.db ? sqlite3_errmsg(handle.db) : "sqlite open failed";
        throw std::runtime_error(message);
    }
    sqlite3_busy_timeout(handle.db, 5000);

    if (!g_schema_ready) {
        exec_sql(handle.db, "PRAGMA journal_mode=WAL;");
        exec_sql(handle.db, "PRAGMA foreign_keys=ON;");
        exec_sql(handle.db,
            "CREATE TABLE IF NOT EXISTS metadata ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS reports ("
            "  module TEXT NOT NULL,"
            "  id TEXT NOT NULL,"
            "  kind TEXT,"
            "  status TEXT,"
            "  title TEXT,"
            "  created_at TEXT,"
            "  updated_at TEXT,"
            "  path TEXT,"
            "  payload_json TEXT NOT NULL,"
            "  PRIMARY KEY(module, id)"
            ");"
            "CREATE TABLE IF NOT EXISTS results ("
            "  module TEXT NOT NULL,"
            "  report_id TEXT NOT NULL,"
            "  result_id TEXT NOT NULL,"
            "  model TEXT,"
            "  domain TEXT,"
            "  status TEXT,"
            "  score REAL,"
            "  payload_json TEXT NOT NULL,"
            "  created_at TEXT,"
            "  PRIMARY KEY(module, report_id, result_id)"
            ");"
            "CREATE TABLE IF NOT EXISTS best_results ("
            "  module TEXT NOT NULL,"
            "  model TEXT NOT NULL,"
            "  domain TEXT NOT NULL,"
            "  score REAL NOT NULL,"
            "  report_id TEXT NOT NULL,"
            "  result_id TEXT NOT NULL,"
            "  payload_json TEXT NOT NULL,"
            "  updated_at TEXT NOT NULL,"
            "  PRIMARY KEY(module, model, domain)"
            ");"
            "CREATE TABLE IF NOT EXISTS downloads ("
            "  id TEXT PRIMARY KEY,"
            "  model_id TEXT,"
            "  repo TEXT,"
            "  hf_ref TEXT,"
            "  quant TEXT,"
            "  status TEXT,"
            "  target_dir TEXT,"
            "  local_path TEXT,"
            "  downloaded_bytes INTEGER,"
            "  total_bytes INTEGER,"
            "  percent REAL,"
            "  started_at TEXT,"
            "  updated_at TEXT,"
            "  finished_at TEXT,"
            "  payload_json TEXT NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS fit_recommendations ("
            "  id TEXT NOT NULL,"
            "  quant TEXT NOT NULL,"
            "  strategy TEXT NOT NULL,"
            "  score REAL,"
            "  fit_level TEXT,"
            "  use_case TEXT,"
            "  context_length INTEGER,"
            "  payload_json TEXT NOT NULL,"
            "  updated_at TEXT NOT NULL,"
            "  PRIMARY KEY(id, quant, strategy)"
            ");"
            "CREATE TABLE IF NOT EXISTS configurations ("
            "  rowid INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  module TEXT NOT NULL,"
            "  model_id TEXT NOT NULL,"
            "  preset_id TEXT,"
            "  created_at TEXT NOT NULL,"
            "  payload_json TEXT NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS events ("
            "  rowid INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  module TEXT NOT NULL,"
            "  event_type TEXT NOT NULL,"
            "  object_id TEXT,"
            "  created_at TEXT NOT NULL,"
            "  payload_json TEXT NOT NULL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_results_model ON results(module, model);"
            "CREATE INDEX IF NOT EXISTS idx_downloads_status ON downloads(status);"
            "CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(module, status);");
        exec_sql(handle.db,
            "INSERT INTO metadata(key, value) VALUES('schema_version', '1') "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
        g_schema_ready = true;
    }
    return handle;
}

struct stmt_handle {
    sqlite3_stmt * stmt = nullptr;
    ~stmt_handle() {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

static stmt_handle prepare(sqlite3 * db, const char * sql) {
    stmt_handle out;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &out.stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return out;
}

static void bind_text(sqlite3_stmt * stmt, int index, const std::string & value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

static void bind_i64(sqlite3_stmt * stmt, int index, int64_t value) {
    sqlite3_bind_int64(stmt, index, value);
}

static void bind_double(sqlite3_stmt * stmt, int index, double value) {
    sqlite3_bind_double(stmt, index, value);
}

static void step_done(sqlite3 * db, sqlite3_stmt * stmt) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

static std::string col_text(sqlite3_stmt * stmt, int index) {
    const unsigned char * text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char *>(text) : "";
}

static json col_json(sqlite3_stmt * stmt, int index) {
    const std::string text = col_text(stmt, index);
    if (text.empty()) {
        return nullptr;
    }
    try {
        return json::parse(text);
    } catch (...) {
        return text;
    }
}

static std::string report_time(const json & payload) {
    return first_string(payload, {"updated_at", "completed_at", "paused_at", "created_at"});
}

static std::string result_status(const json & row) {
    if (row.contains("ok") && row["ok"].is_boolean()) {
        return row["ok"].get<bool>() ? "ok" : "failed";
    }
    if (row.contains("pass") && row["pass"].is_boolean()) {
        return row["pass"].get<bool>() ? "pass" : "fail";
    }
    return first_number(row, {"eval_tps", "decode_tokens_per_second", "tokens_per_second"}, 0.0) > 0.0 ? "ok" : "unknown";
}

static double result_score(const std::string & module, const json & row) {
    if (module == "ds4-eval") {
        if (row.contains("pass") && row["pass"].is_boolean()) {
            return row["pass"].get<bool>() ? 100.0 : 0.0;
        }
        return first_number(row, {"tokens_per_second"}, 0.0);
    }
    if (module == "ds4-bench") {
        return first_number(row, {"decode_tokens_per_second", "prompt_tokens_per_second"}, 0.0);
    }
    return first_number(row, {"eval_tps", "tps", "decode_tokens_per_second", "tokens_per_second"}, 0.0);
}

static std::string result_domain(const json & row) {
    std::string out = first_string(row, {"domain", "source", "workload_kind", "row_role"});
    return out.empty() ? "overall" : out;
}

static std::string result_model(const json & row) {
    std::string out = first_string(row, {"model", "model_id", "name"});
    return out.empty() ? "unknown" : out;
}

static std::string result_id(const json & row, size_t index) {
    const std::string model = result_model(row);
    const std::string raw = first_string(row, {"id", "case_id", "label", "variant"});
    if (!raw.empty()) {
        return model + ":" + raw;
    }
    return model + ":" + std::to_string(index);
}

static void insert_event_locked(sqlite3 * db, const std::string & module, const std::string & event_type, const std::string & object_id, const json & payload) {
    auto stmt = prepare(db,
        "INSERT INTO events(module, event_type, object_id, created_at, payload_json) "
        "VALUES(?, ?, ?, ?, ?);");
    bind_text(stmt.stmt, 1, module);
    bind_text(stmt.stmt, 2, event_type);
    bind_text(stmt.stmt, 3, object_id);
    bind_text(stmt.stmt, 4, isoish_timestamp());
    bind_text(stmt.stmt, 5, json_dump(payload));
    step_done(db, stmt.stmt);
}

static void insert_result_locked(sqlite3 * db, const std::string & module, const std::string & report_id, const std::string & report_status, const json & row, size_t index) {
    const std::string rid = result_id(row, index);
    const std::string model = result_model(row);
    const std::string domain = result_domain(row);
    const std::string status = result_status(row);
    const double score = result_score(module, row);
    const std::string created_at = first_string(row, {"created_at", "updated_at"});

    auto stmt = prepare(db,
        "INSERT INTO results(module, report_id, result_id, model, domain, status, score, payload_json, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(module, report_id, result_id) DO UPDATE SET "
        "model=excluded.model, domain=excluded.domain, status=excluded.status, score=excluded.score, "
        "payload_json=excluded.payload_json, created_at=excluded.created_at;");
    bind_text(stmt.stmt, 1, module);
    bind_text(stmt.stmt, 2, report_id);
    bind_text(stmt.stmt, 3, rid);
    bind_text(stmt.stmt, 4, model);
    bind_text(stmt.stmt, 5, domain);
    bind_text(stmt.stmt, 6, status);
    bind_double(stmt.stmt, 7, score);
    bind_text(stmt.stmt, 8, json_dump(row));
    bind_text(stmt.stmt, 9, created_at);
    step_done(db, stmt.stmt);

    const bool eligible = report_status == "completed" && (status == "ok" || status == "pass") && score > 0.0;
    if (!eligible || model == "unknown") {
        return;
    }

    auto best = prepare(db,
        "INSERT INTO best_results(module, model, domain, score, report_id, result_id, payload_json, updated_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(module, model, domain) DO UPDATE SET "
        "score=excluded.score, report_id=excluded.report_id, result_id=excluded.result_id, "
        "payload_json=excluded.payload_json, updated_at=excluded.updated_at "
        "WHERE excluded.score >= best_results.score;");
    bind_text(best.stmt, 1, module);
    bind_text(best.stmt, 2, model);
    bind_text(best.stmt, 3, domain);
    bind_double(best.stmt, 4, score);
    bind_text(best.stmt, 5, report_id);
    bind_text(best.stmt, 6, rid);
    bind_text(best.stmt, 7, json_dump(row));
    bind_text(best.stmt, 8, isoish_timestamp());
    step_done(db, best.stmt);
}

static void record_report_locked(sqlite3 * db,
        const std::string & module,
        const std::string & id,
        const std::string & kind,
        const std::string & status,
        const std::string & title,
        const std::filesystem::path & path,
        const json & payload) {
    auto stmt = prepare(db,
        "INSERT INTO reports(module, id, kind, status, title, created_at, updated_at, path, payload_json) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(module, id) DO UPDATE SET "
        "kind=excluded.kind, status=excluded.status, title=excluded.title, created_at=excluded.created_at, "
        "updated_at=excluded.updated_at, path=excluded.path, payload_json=excluded.payload_json;");
    bind_text(stmt.stmt, 1, module);
    bind_text(stmt.stmt, 2, id);
    bind_text(stmt.stmt, 3, kind);
    bind_text(stmt.stmt, 4, status);
    bind_text(stmt.stmt, 5, title);
    bind_text(stmt.stmt, 6, first_string(payload, {"created_at"}));
    bind_text(stmt.stmt, 7, report_time(payload));
    bind_text(stmt.stmt, 8, path.string());
    bind_text(stmt.stmt, 9, json_dump(payload));
    step_done(db, stmt.stmt);

    const json rows = payload.contains("rows") && payload["rows"].is_array()
        ? payload["rows"]
        : (payload.contains("results") && payload["results"].is_array() ? payload["results"] : json::array());
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].is_object()) {
            insert_result_locked(db, module, id, status, rows[i], i);
        }
    }
}

static json report_summary_from_file(const std::filesystem::path & path) {
    return json::parse(read_text_file(path));
}

static void import_report_file_locked(sqlite3 * db, const std::filesystem::path & path, const std::string & module_hint) {
    json report = report_summary_from_file(path);
    if (!report.is_object()) {
        return;
    }
    const std::string id = first_string(report, {"id"});
    if (id.empty()) {
        return;
    }
    std::string module = module_hint;
    std::string kind = first_string(report, {"kind"});
    if (module == "ds4") {
        module = kind == "bench" ? "ds4-bench" : "ds4-eval";
    }
    if (kind.empty()) {
        kind = module == "caliber-advisor" ? "campaign" : "";
    }
    const std::string status = first_string(report, {"status"});
    const std::string title = first_string(report, {"model", "model_selector", "id"});
    record_report_locked(db, module, id, kind, status, title, path, report);
}

static int64_t scalar_count(sqlite3 * db, const char * sql) {
    auto stmt = prepare(db, sql);
    const int rc = sqlite3_step(stmt.stmt);
    if (rc != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(stmt.stmt, 0);
}

static json select_rows(sqlite3 * db, const char * sql, const std::vector<std::string> & columns, const std::vector<std::string> & json_columns = {}) {
    auto stmt = prepare(db, sql);
    json out = json::array();
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        json row = json::object();
        for (size_t i = 0; i < columns.size(); ++i) {
            const std::string & name = columns[i];
            if (std::find(json_columns.begin(), json_columns.end(), name) != json_columns.end()) {
                row[name] = col_json(stmt.stmt, (int) i);
            } else if (sqlite3_column_type(stmt.stmt, (int) i) == SQLITE_INTEGER) {
                row[name] = (int64_t) sqlite3_column_int64(stmt.stmt, (int) i);
            } else if (sqlite3_column_type(stmt.stmt, (int) i) == SQLITE_FLOAT) {
                row[name] = sqlite3_column_double(stmt.stmt, (int) i);
            } else if (sqlite3_column_type(stmt.stmt, (int) i) == SQLITE_NULL) {
                row[name] = nullptr;
            } else {
                row[name] = col_text(stmt.stmt, (int) i);
            }
        }
        out.push_back(row);
    }
    return out;
}

static void import_reports_table_locked(sqlite3 * db, const json & rows) {
    if (!rows.is_array()) return;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const json payload = row.value("payload_json", json::object());
        record_report_locked(db,
            json_string_value(row, "module"),
            json_string_value(row, "id"),
            json_string_value(row, "kind"),
            json_string_value(row, "status"),
            json_string_value(row, "title"),
            json_string_value(row, "path"),
            payload);
    }
}

static void import_downloads_table_locked(sqlite3 * db, const json & rows) {
    if (!rows.is_array()) return;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const json payload = row.value("payload_json", row);
        auto stmt = prepare(db,
            "INSERT INTO downloads(id, model_id, repo, hf_ref, quant, status, target_dir, local_path, downloaded_bytes, total_bytes, percent, started_at, updated_at, finished_at, payload_json) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "model_id=excluded.model_id, repo=excluded.repo, hf_ref=excluded.hf_ref, quant=excluded.quant, status=excluded.status, "
            "target_dir=excluded.target_dir, local_path=excluded.local_path, downloaded_bytes=excluded.downloaded_bytes, "
            "total_bytes=excluded.total_bytes, percent=excluded.percent, started_at=excluded.started_at, updated_at=excluded.updated_at, "
            "finished_at=excluded.finished_at, payload_json=excluded.payload_json;");
        bind_text(stmt.stmt, 1, json_string_value(row, "id"));
        bind_text(stmt.stmt, 2, json_string_value(row, "model_id"));
        bind_text(stmt.stmt, 3, json_string_value(row, "repo"));
        bind_text(stmt.stmt, 4, json_string_value(row, "hf_ref"));
        bind_text(stmt.stmt, 5, json_string_value(row, "quant"));
        bind_text(stmt.stmt, 6, json_string_value(row, "status"));
        bind_text(stmt.stmt, 7, json_string_value(row, "target_dir"));
        bind_text(stmt.stmt, 8, json_string_value(row, "local_path"));
        bind_i64(stmt.stmt, 9, json_i64_value(row, "downloaded_bytes"));
        bind_i64(stmt.stmt, 10, json_i64_value(row, "total_bytes"));
        bind_double(stmt.stmt, 11, json_number_value(row, "percent"));
        bind_text(stmt.stmt, 12, json_string_value(row, "started_at"));
        bind_text(stmt.stmt, 13, json_string_value(row, "updated_at"));
        bind_text(stmt.stmt, 14, json_string_value(row, "finished_at"));
        bind_text(stmt.stmt, 15, json_dump(payload));
        step_done(db, stmt.stmt);
    }
}

static void import_fit_table_locked(sqlite3 * db, const json & rows) {
    if (!rows.is_array()) return;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const json payload = row.value("payload_json", row);
        auto stmt = prepare(db,
            "INSERT INTO fit_recommendations(id, quant, strategy, score, fit_level, use_case, context_length, payload_json, updated_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id, quant, strategy) DO UPDATE SET "
            "score=excluded.score, fit_level=excluded.fit_level, use_case=excluded.use_case, context_length=excluded.context_length, "
            "payload_json=excluded.payload_json, updated_at=excluded.updated_at;");
        bind_text(stmt.stmt, 1, json_string_value(row, "id"));
        bind_text(stmt.stmt, 2, json_string_value(row, "quant"));
        bind_text(stmt.stmt, 3, json_string_value(row, "strategy"));
        bind_double(stmt.stmt, 4, json_number_value(row, "score"));
        bind_text(stmt.stmt, 5, json_string_value(row, "fit_level"));
        bind_text(stmt.stmt, 6, json_string_value(row, "use_case"));
        bind_i64(stmt.stmt, 7, json_i64_value(row, "context_length"));
        bind_text(stmt.stmt, 8, json_dump(payload));
        bind_text(stmt.stmt, 9, json_string_value(row, "updated_at").empty() ? isoish_timestamp() : json_string_value(row, "updated_at"));
        step_done(db, stmt.stmt);
    }
}

static void import_configurations_table_locked(sqlite3 * db, const json & rows) {
    if (!rows.is_array()) return;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const json payload = row.value("payload_json", row);
        auto stmt = prepare(db,
            "INSERT INTO configurations(module, model_id, preset_id, created_at, payload_json) "
            "VALUES(?, ?, ?, ?, ?);");
        bind_text(stmt.stmt, 1, json_string_value(row, "module"));
        bind_text(stmt.stmt, 2, json_string_value(row, "model_id"));
        bind_text(stmt.stmt, 3, json_string_value(row, "preset_id"));
        bind_text(stmt.stmt, 4, json_string_value(row, "created_at").empty() ? isoish_timestamp() : json_string_value(row, "created_at"));
        bind_text(stmt.stmt, 5, json_dump(payload));
        step_done(db, stmt.stmt);
    }
}

static void import_events_table_locked(sqlite3 * db, const json & rows) {
    if (!rows.is_array()) return;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const json payload = row.value("payload_json", row);
        auto stmt = prepare(db,
            "INSERT INTO events(module, event_type, object_id, created_at, payload_json) "
            "VALUES(?, ?, ?, ?, ?);");
        bind_text(stmt.stmt, 1, json_string_value(row, "module"));
        bind_text(stmt.stmt, 2, json_string_value(row, "event_type"));
        bind_text(stmt.stmt, 3, json_string_value(row, "object_id"));
        bind_text(stmt.stmt, 4, json_string_value(row, "created_at").empty() ? isoish_timestamp() : json_string_value(row, "created_at"));
        bind_text(stmt.stmt, 5, json_dump(payload));
        step_done(db, stmt.stmt);
    }
}

} // namespace

std::filesystem::path database_path() {
    return std::filesystem::path(LLAMA_PLATFORM_DB_PATH);
}

void import_existing_reports_once() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_imported_reports) {
        return;
    }
    try {
        auto db = open_database_locked();
        std::error_code ec;
        const std::filesystem::path ds4_dir(LLAMA_DS4_REPORTS_DIR);
        if (std::filesystem::exists(ds4_dir, ec)) {
            for (const auto & entry : std::filesystem::directory_iterator(ds4_dir, ec)) {
                if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                    continue;
                }
                try {
                    import_report_file_locked(db.db, entry.path(), "ds4");
                } catch (const std::exception & e) {
                    SRV_WRN("archive import skipped %s: %s\n", entry.path().string().c_str(), e.what());
                }
            }
        }
        const std::filesystem::path caliber_dir(LLAMA_CALIBER_REPORTS_DIR);
        if (std::filesystem::exists(caliber_dir, ec)) {
            for (const auto & entry : std::filesystem::directory_iterator(caliber_dir, ec)) {
                if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                    continue;
                }
                try {
                    import_report_file_locked(db.db, entry.path(), "caliber-advisor");
                } catch (const std::exception & e) {
                    SRV_WRN("archive import skipped %s: %s\n", entry.path().string().c_str(), e.what());
                }
            }
        }
        g_imported_reports = true;
    } catch (const std::exception & e) {
        SRV_WRN("archive import initialization failed: %s\n", e.what());
    }
}

void record_report(
        const std::string & module,
        const std::string & id,
        const std::string & kind,
        const std::string & status,
        const std::string & title,
        const std::filesystem::path & path,
        const json & payload) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        record_report_locked(db.db, module, id, kind, status, title, path, payload);
        insert_event_locked(db.db, module, "report_saved", id, {{"status", status}, {"path", path.string()}});
    } catch (const std::exception & e) {
        SRV_WRN("archive report write failed: %s\n", e.what());
    }
}

void delete_report(const std::string & module, const std::string & id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        auto del_best = prepare(db.db, "DELETE FROM best_results WHERE module = ? AND report_id = ?;");
        bind_text(del_best.stmt, 1, module);
        bind_text(del_best.stmt, 2, id);
        step_done(db.db, del_best.stmt);
        auto del_results = prepare(db.db, "DELETE FROM results WHERE module = ? AND report_id = ?;");
        bind_text(del_results.stmt, 1, module);
        bind_text(del_results.stmt, 2, id);
        step_done(db.db, del_results.stmt);
        auto del_report = prepare(db.db, "DELETE FROM reports WHERE module = ? AND id = ?;");
        bind_text(del_report.stmt, 1, module);
        bind_text(del_report.stmt, 2, id);
        step_done(db.db, del_report.stmt);
        insert_event_locked(db.db, module, "report_deleted", id, json::object());
    } catch (const std::exception & e) {
        SRV_WRN("archive report delete failed: %s\n", e.what());
    }
}

void record_download(const json & snapshot) {
    if (!snapshot.is_object()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        const std::string id = json_string_value(snapshot, "id");
        if (id.empty()) {
            return;
        }
        auto stmt = prepare(db.db,
            "INSERT INTO downloads(id, model_id, repo, hf_ref, quant, status, target_dir, local_path, downloaded_bytes, total_bytes, percent, started_at, updated_at, finished_at, payload_json) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "model_id=excluded.model_id, repo=excluded.repo, hf_ref=excluded.hf_ref, quant=excluded.quant, status=excluded.status, "
            "target_dir=excluded.target_dir, local_path=excluded.local_path, downloaded_bytes=excluded.downloaded_bytes, "
            "total_bytes=excluded.total_bytes, percent=excluded.percent, started_at=excluded.started_at, updated_at=excluded.updated_at, "
            "finished_at=excluded.finished_at, payload_json=excluded.payload_json;");
        bind_text(stmt.stmt, 1, id);
        bind_text(stmt.stmt, 2, json_string_value(snapshot, "model_id"));
        bind_text(stmt.stmt, 3, json_string_value(snapshot, "repo"));
        bind_text(stmt.stmt, 4, json_string_value(snapshot, "hf_ref"));
        bind_text(stmt.stmt, 5, json_string_value(snapshot, "quant"));
        bind_text(stmt.stmt, 6, json_string_value(snapshot, "status"));
        bind_text(stmt.stmt, 7, json_string_value(snapshot, "target_dir"));
        bind_text(stmt.stmt, 8, json_string_value(snapshot, "local_path"));
        bind_i64(stmt.stmt, 9, json_i64_value(snapshot, "downloaded_bytes"));
        bind_i64(stmt.stmt, 10, json_i64_value(snapshot, "total_bytes"));
        bind_double(stmt.stmt, 11, json_number_value(snapshot, "percent"));
        bind_text(stmt.stmt, 12, json_string_value(snapshot, "started_at"));
        bind_text(stmt.stmt, 13, json_string_value(snapshot, "updated_at"));
        bind_text(stmt.stmt, 14, json_string_value(snapshot, "finished_at"));
        bind_text(stmt.stmt, 15, json_dump(snapshot));
        step_done(db.db, stmt.stmt);
    } catch (const std::exception & e) {
        SRV_WRN("archive download write failed: %s\n", e.what());
    }
}

void record_fit_recommendations(const json & response) {
    if (!response.is_object() || !response.contains("models") || !response["models"].is_array()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        const std::string updated_at = isoish_timestamp();
        for (const auto & model : response["models"]) {
            if (!model.is_object()) {
                continue;
            }
            const std::string id = json_string_value(model, "id");
            if (id.empty()) {
                continue;
            }
            auto stmt = prepare(db.db,
                "INSERT INTO fit_recommendations(id, quant, strategy, score, fit_level, use_case, context_length, payload_json, updated_at) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(id, quant, strategy) DO UPDATE SET "
                "score=excluded.score, fit_level=excluded.fit_level, use_case=excluded.use_case, context_length=excluded.context_length, "
                "payload_json=excluded.payload_json, updated_at=excluded.updated_at;");
            bind_text(stmt.stmt, 1, id);
            bind_text(stmt.stmt, 2, json_string_value(model, "quant"));
            bind_text(stmt.stmt, 3, json_string_value(model, "fit_strategy"));
            bind_double(stmt.stmt, 4, json_number_value(model, "score"));
            bind_text(stmt.stmt, 5, json_string_value(model, "fit_level"));
            bind_text(stmt.stmt, 6, json_string_value(model, "use_case"));
            bind_i64(stmt.stmt, 7, json_i64_value(model, "effective_context_length"));
            bind_text(stmt.stmt, 8, json_dump(model));
            bind_text(stmt.stmt, 9, updated_at);
            step_done(db.db, stmt.stmt);
        }
        insert_event_locked(db.db, "fit-advisor", "recommendations_saved", "", {
            {"returned_models", json_i64_value(response, "returned_models")},
            {"total_catalog_models", json_i64_value(response, "total_catalog_models")},
        });
    } catch (const std::exception & e) {
        SRV_WRN("archive fit recommendations write failed: %s\n", e.what());
    }
}

void record_configuration(const std::string & module, const std::string & model_id, const std::string & preset_id, const json & payload) {
    if (model_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        auto stmt = prepare(db.db,
            "INSERT INTO configurations(module, model_id, preset_id, created_at, payload_json) "
            "VALUES(?, ?, ?, ?, ?);");
        bind_text(stmt.stmt, 1, module);
        bind_text(stmt.stmt, 2, model_id);
        bind_text(stmt.stmt, 3, preset_id);
        bind_text(stmt.stmt, 4, isoish_timestamp());
        bind_text(stmt.stmt, 5, json_dump(payload));
        step_done(db.db, stmt.stmt);
        insert_event_locked(db.db, module, "configured", model_id, payload);
    } catch (const std::exception & e) {
        SRV_WRN("archive configuration write failed: %s\n", e.what());
    }
}

server_http_res_ptr handle_archive_status(const server_http_req &) {
    import_existing_reports_once();
    auto res = std::make_unique<server_http_res>();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto db = open_database_locked();
    json out = {
        {"object", "llm-model-select.archive.status"},
        {"database_path", database_path().string()},
        {"reports", scalar_count(db.db, "SELECT COUNT(*) FROM reports")},
        {"results", scalar_count(db.db, "SELECT COUNT(*) FROM results")},
        {"best_results", scalar_count(db.db, "SELECT COUNT(*) FROM best_results")},
        {"downloads", scalar_count(db.db, "SELECT COUNT(*) FROM downloads")},
        {"fit_recommendations", scalar_count(db.db, "SELECT COUNT(*) FROM fit_recommendations")},
        {"configurations", scalar_count(db.db, "SELECT COUNT(*) FROM configurations")},
        {"events", scalar_count(db.db, "SELECT COUNT(*) FROM events")},
    };
    res->data = safe_json_to_str(out);
    return res;
}

server_http_res_ptr handle_archive_export(const server_http_req &) {
    import_existing_reports_once();
    auto res = std::make_unique<server_http_res>();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto db = open_database_locked();
    json archive = {
        {"object", "llm-model-select.archive"},
        {"version", 1},
        {"exported_at", isoish_timestamp()},
        {"database_path", database_path().string()},
        {"tables", {
            {"reports", select_rows(db.db,
                "SELECT module, id, kind, status, title, created_at, updated_at, path, payload_json FROM reports ORDER BY updated_at DESC, created_at DESC",
                {"module", "id", "kind", "status", "title", "created_at", "updated_at", "path", "payload_json"},
                {"payload_json"})},
            {"results", select_rows(db.db,
                "SELECT module, report_id, result_id, model, domain, status, score, payload_json, created_at FROM results ORDER BY module, report_id, result_id",
                {"module", "report_id", "result_id", "model", "domain", "status", "score", "payload_json", "created_at"},
                {"payload_json"})},
            {"best_results", select_rows(db.db,
                "SELECT module, model, domain, score, report_id, result_id, payload_json, updated_at FROM best_results ORDER BY module, domain, score DESC",
                {"module", "model", "domain", "score", "report_id", "result_id", "payload_json", "updated_at"},
                {"payload_json"})},
            {"downloads", select_rows(db.db,
                "SELECT id, model_id, repo, hf_ref, quant, status, target_dir, local_path, downloaded_bytes, total_bytes, percent, started_at, updated_at, finished_at, payload_json FROM downloads ORDER BY updated_at DESC",
                {"id", "model_id", "repo", "hf_ref", "quant", "status", "target_dir", "local_path", "downloaded_bytes", "total_bytes", "percent", "started_at", "updated_at", "finished_at", "payload_json"},
                {"payload_json"})},
            {"fit_recommendations", select_rows(db.db,
                "SELECT id, quant, strategy, score, fit_level, use_case, context_length, payload_json, updated_at FROM fit_recommendations ORDER BY updated_at DESC, score DESC",
                {"id", "quant", "strategy", "score", "fit_level", "use_case", "context_length", "payload_json", "updated_at"},
                {"payload_json"})},
            {"configurations", select_rows(db.db,
                "SELECT module, model_id, preset_id, created_at, payload_json FROM configurations ORDER BY rowid DESC",
                {"module", "model_id", "preset_id", "created_at", "payload_json"},
                {"payload_json"})},
            {"events", select_rows(db.db,
                "SELECT module, event_type, object_id, created_at, payload_json FROM events ORDER BY rowid DESC LIMIT 5000",
                {"module", "event_type", "object_id", "created_at", "payload_json"},
                {"payload_json"})},
        }},
    };
    res->headers["Content-Disposition"] = "attachment; filename=\"llm-model-select-archive.json\"";
    res->data = archive.dump(2);
    return res;
}

server_http_res_ptr handle_archive_import(const server_http_req & req) {
    auto res = std::make_unique<server_http_res>();
    const json archive = req.body.empty() ? json::object() : json::parse(req.body);
    if (!archive.is_object() || json_string_value(archive, "object") != "llm-model-select.archive") {
        res->status = 400;
        res->data = safe_json_to_str({{"error", {{"message", "invalid archive object"}, {"code", 400}}}});
        return res;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto db = open_database_locked();
    exec_sql(db.db, "BEGIN IMMEDIATE;");
    try {
        const json tables = archive.value("tables", json::object());
        import_reports_table_locked(db.db, tables.value("reports", json::array()));
        import_downloads_table_locked(db.db, tables.value("downloads", json::array()));
        import_fit_table_locked(db.db, tables.value("fit_recommendations", json::array()));
        import_configurations_table_locked(db.db, tables.value("configurations", json::array()));
        import_events_table_locked(db.db, tables.value("events", json::array()));
        exec_sql(db.db, "COMMIT;");
    } catch (...) {
        exec_sql(db.db, "ROLLBACK;");
        throw;
    }
    res->data = safe_json_to_str({
        {"success", true},
        {"database_path", database_path().string()},
        {"reports", scalar_count(db.db, "SELECT COUNT(*) FROM reports")},
        {"results", scalar_count(db.db, "SELECT COUNT(*) FROM results")},
        {"downloads", scalar_count(db.db, "SELECT COUNT(*) FROM downloads")},
        {"fit_recommendations", scalar_count(db.db, "SELECT COUNT(*) FROM fit_recommendations")},
    });
    return res;
}

} // namespace server_persistence
