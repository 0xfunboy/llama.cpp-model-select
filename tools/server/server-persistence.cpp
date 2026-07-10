#include "server-persistence.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cstdlib>

namespace server_persistence {
namespace {

static std::mutex g_mutex;
static bool g_schema_ready = false;
static bool g_imported_reports = false;

static std::filesystem::path xdg_dir(const char * env_name, const std::filesystem::path & home_suffix) {
    if (const char * configured = std::getenv(env_name); configured && configured[0] != '\0') {
        return std::filesystem::path(configured) / "llama.cpp-model-select";
    }
    if (const char * home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::filesystem::path(home) / home_suffix / "llama.cpp-model-select";
    }
    return std::filesystem::temp_directory_path() / "llama.cpp-model-select";
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

static std::string stable_fingerprint(const json & value) {
    uint64_t hash = 1469598103934665603ULL;
    const std::string text = json_dump(value);
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
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

static void migrate_legacy_database(const std::filesystem::path & target) {
    if (std::filesystem::exists(target)) return;
    std::filesystem::path legacy;
    if (const char * configured = std::getenv("LLAMA_MODEL_SELECT_LEGACY_DB"); configured && configured[0] != '\0') {
        legacy = configured;
    } else {
        legacy = std::filesystem::current_path() / "data" / "llm-model-select.sqlite";
    }
    std::error_code ec;
    if (!std::filesystem::exists(legacy, ec) || std::filesystem::equivalent(legacy, target, ec)) return;
    std::filesystem::create_directories(target.parent_path());
    sqlite3 * source = nullptr;
    sqlite3 * destination = nullptr;
    if (sqlite3_open_v2(legacy.string().c_str(), &source, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
        sqlite3_open_v2(target.string().c_str(), &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (source) sqlite3_close(source);
        if (destination) sqlite3_close(destination);
        std::filesystem::remove(target, ec);
        return;
    }
    sqlite3_backup * backup = sqlite3_backup_init(destination, "main", source, "main");
    const bool copied = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
    if (backup) sqlite3_backup_finish(backup);
    sqlite3_close(source);
    sqlite3_close(destination);
    if (!copied) std::filesystem::remove(target, ec);
}

static db_handle open_database_locked() {
    const auto path = database_path();
    migrate_legacy_database(path);
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
            "CREATE TABLE IF NOT EXISTS samples ("
            "  module TEXT NOT NULL,"
            "  report_id TEXT NOT NULL,"
            "  result_id TEXT NOT NULL,"
            "  sample_id TEXT NOT NULL,"
            "  payload_json TEXT NOT NULL,"
            "  created_at TEXT,"
            "  PRIMARY KEY(module, report_id, result_id, sample_id)"
            ");"
            "CREATE TABLE IF NOT EXISTS archive_imports ("
            "  fingerprint TEXT PRIMARY KEY,"
            "  imported_at TEXT NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS imported_rows ("
            "  kind TEXT NOT NULL,"
            "  fingerprint TEXT NOT NULL,"
            "  imported_at TEXT NOT NULL,"
            "  PRIMARY KEY(kind, fingerprint)"
            ");"
            "CREATE TABLE IF NOT EXISTS jobs ("
            "  module TEXT NOT NULL,"
            "  id TEXT NOT NULL,"
            "  status TEXT NOT NULL,"
            "  created_at TEXT,"
            "  updated_at TEXT NOT NULL,"
            "  payload_json TEXT NOT NULL,"
            "  PRIMARY KEY(module, id)"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_results_model ON results(module, model);"
            "CREATE INDEX IF NOT EXISTS idx_samples_result ON samples(module, report_id, result_id);"
            "CREATE INDEX IF NOT EXISTS idx_downloads_status ON downloads(status);"
            "CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(module, status);");
        exec_sql(handle.db,
            "INSERT INTO metadata(key, value) VALUES('schema_version', '3') "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
        exec_sql(handle.db,
            "UPDATE jobs SET status='interrupted', updated_at=datetime('now') WHERE status IN ('queued','running','stopping','cancelling','resolving','downloading');"
            "UPDATE downloads SET status='interrupted', updated_at=datetime('now') WHERE status IN ('queued','resolving','downloading');");
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
    std::string out = first_string(row, {"artifact_id", "model_id", "model", "name"});
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

static void insert_samples_locked(sqlite3 * db, const std::string & module, const std::string & report_id, const std::string & result_id, const json & row) {
    if (!row.contains("runs") || !row["runs"].is_array()) return;
    size_t index = 0;
    for (const auto & sample : row["runs"]) {
        if (!sample.is_object()) {
            ++index;
            continue;
        }
        std::string sample_id = first_string(sample, {"sample_id", "run_id", "run_index"});
        if (sample_id.empty()) sample_id = std::to_string(index);
        auto stmt = prepare(db,
            "INSERT OR IGNORE INTO samples(module, report_id, result_id, sample_id, payload_json, created_at) "
            "VALUES(?, ?, ?, ?, ?, ?);");
        bind_text(stmt.stmt, 1, module);
        bind_text(stmt.stmt, 2, report_id);
        bind_text(stmt.stmt, 3, result_id);
        bind_text(stmt.stmt, 4, sample_id);
        bind_text(stmt.stmt, 5, json_dump(sample));
        bind_text(stmt.stmt, 6, first_string(sample, {"timestamp", "created_at"}));
        step_done(db, stmt.stmt);
        ++index;
    }
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
    insert_samples_locked(db, module, report_id, rid, row);

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

static void rebuild_best_results_locked(sqlite3 * db) {
    exec_sql(db, "DELETE FROM best_results;");
    auto stmt = prepare(db,
        "INSERT INTO best_results(module, model, domain, score, report_id, result_id, payload_json, updated_at) "
        "SELECT r.module, r.model, r.domain, r.score, r.report_id, r.result_id, r.payload_json, ? "
        "FROM results r JOIN reports p ON p.module = r.module AND p.id = r.report_id "
        "WHERE p.status = 'completed' AND r.status IN ('ok', 'pass') AND r.score > 0 AND r.model <> 'unknown' "
        "AND NOT EXISTS ("
        "  SELECT 1 FROM results better JOIN reports bp ON bp.module = better.module AND bp.id = better.report_id "
        "  WHERE better.module = r.module AND better.model = r.model AND better.domain = r.domain "
        "    AND bp.status = 'completed' AND better.status IN ('ok', 'pass') AND better.score > 0 "
        "    AND (better.score > r.score OR (better.score = r.score AND (better.result_id < r.result_id OR "
        "         (better.result_id = r.result_id AND better.report_id < r.report_id))))"
        ");");
    bind_text(stmt.stmt, 1, isoish_timestamp());
    step_done(db, stmt.stmt);
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
        const json payload = redact_for_export(row.value("payload_json", json::object()));
        record_report_locked(db,
            json_string_value(row, "module"),
            json_string_value(row, "id"),
            json_string_value(row, "kind"),
            json_string_value(row, "status"),
            json_string_value(row, "title"),
            std::filesystem::path(),
            payload);
    }
}

static void import_downloads_table_locked(sqlite3 * db, const json & rows) {
    if (!rows.is_array()) return;
    for (const auto & row : rows) {
        if (!row.is_object()) continue;
        const json payload = redact_for_export(row.value("payload_json", row));
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
        bind_text(stmt.stmt, 7, "");
        bind_text(stmt.stmt, 8, "");
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
        const json payload = redact_for_export(row.value("payload_json", row));
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
        const std::string fingerprint = stable_fingerprint(row);
        auto imported = prepare(db, "INSERT OR IGNORE INTO imported_rows(kind, fingerprint, imported_at) VALUES('configuration', ?, ?);");
        bind_text(imported.stmt, 1, fingerprint);
        bind_text(imported.stmt, 2, isoish_timestamp());
        step_done(db, imported.stmt);
        if (sqlite3_changes(db) == 0) continue;
        const json payload = redact_for_export(row.value("payload_json", row));
        auto existing = prepare(db,
            "SELECT 1 FROM configurations WHERE module = ? AND model_id = ? AND preset_id = ? AND created_at = ? LIMIT 1;");
        bind_text(existing.stmt, 1, json_string_value(row, "module"));
        bind_text(existing.stmt, 2, json_string_value(row, "model_id"));
        bind_text(existing.stmt, 3, json_string_value(row, "preset_id"));
        bind_text(existing.stmt, 4, json_string_value(row, "created_at"));
        if (sqlite3_step(existing.stmt) == SQLITE_ROW) continue;
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
        const std::string fingerprint = stable_fingerprint(row);
        auto imported = prepare(db, "INSERT OR IGNORE INTO imported_rows(kind, fingerprint, imported_at) VALUES('event', ?, ?);");
        bind_text(imported.stmt, 1, fingerprint);
        bind_text(imported.stmt, 2, isoish_timestamp());
        step_done(db, imported.stmt);
        if (sqlite3_changes(db) == 0) continue;
        const json payload = redact_for_export(row.value("payload_json", row));
        auto existing = prepare(db,
            "SELECT 1 FROM events WHERE module = ? AND event_type = ? AND object_id = ? AND created_at = ? LIMIT 1;");
        bind_text(existing.stmt, 1, json_string_value(row, "module"));
        bind_text(existing.stmt, 2, json_string_value(row, "event_type"));
        bind_text(existing.stmt, 3, json_string_value(row, "object_id"));
        bind_text(existing.stmt, 4, json_string_value(row, "created_at"));
        if (sqlite3_step(existing.stmt) == SQLITE_ROW) continue;
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

std::filesystem::path data_dir() {
    return xdg_dir("XDG_DATA_HOME", ".local/share");
}

std::filesystem::path state_dir() {
    return xdg_dir("XDG_STATE_HOME", ".local/state");
}

std::filesystem::path database_path() {
    if (const char * configured = std::getenv("LLAMA_MODEL_SELECT_DB_PATH"); configured && configured[0] != '\0') {
        return configured;
    }
    return data_dir() / "platform.sqlite";
}

json redact_for_export(const json & value) {
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        const bool windows_path = text.size() > 3 && std::isalpha((unsigned char) text[0]) && text[1] == ':' && (text[2] == '\\' || text[2] == '/');
        const bool absolute_path = !text.empty() && text[0] == '/';
        if (windows_path || absolute_path) {
            const std::filesystem::path source(text);
            return source.filename().empty() ? json("<local>") : json("<local>/" + source.filename().string());
        }
        return value;
    }
    if (value.is_array()) {
        json out = json::array();
        bool redact_next = false;
        for (const auto & item : value) {
            if (redact_next) {
                out.push_back("[REDACTED]");
                redact_next = false;
                continue;
            }
            if (item.is_string()) {
                const std::string token = item.get<std::string>();
                if (token == "--api-key" || token == "--admin-api-key" || token == "--api-key-file" || token == "--admin-api-key-file") {
                    out.push_back(token);
                    redact_next = true;
                    continue;
                }
            }
            out.push_back(redact_for_export(item));
        }
        return out;
    }
    if (!value.is_object()) return value;
    json out = json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        std::string key = it.key();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        const bool secret = key.find("api_key") != std::string::npos || key.find("authorization") != std::string::npos ||
            key.find("password") != std::string::npos || key.find("secret") != std::string::npos || key == "token" || key.find("access_token") != std::string::npos;
        const bool command = key.find("command") != std::string::npos;
        const bool path = key == "path" || key == "paths" || key.find("_path") != std::string::npos || key.find("_dir") != std::string::npos || key == "database_path";
        if (secret) {
            out[it.key()] = "[REDACTED]";
        } else if (command && it.value().is_string()) {
            out[it.key()] = "[REDACTED_COMMAND]";
        } else if (path && it.value().is_string()) {
            const std::filesystem::path source(it.value().get<std::string>());
            out[it.key()] = source.empty() ? "" : (source.filename().empty() ? "<local>" : "<local>/" + source.filename().string());
        } else if (path && it.value().is_array()) {
            out[it.key()] = json::array();
            for (const auto & item : it.value()) {
                if (!item.is_string()) {
                    out[it.key()].push_back(redact_for_export(item));
                    continue;
                }
                const std::filesystem::path source(item.get<std::string>());
                out[it.key()].push_back(source.empty() ? "" : (source.filename().empty() ? "<local>" : "<local>/" + source.filename().string()));
            }
        } else {
            out[it.key()] = redact_for_export(it.value());
        }
    }
    return out;
}

void import_existing_reports_once() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_imported_reports) {
        return;
    }
    try {
        auto db = open_database_locked();
        exec_sql(db.db, "BEGIN IMMEDIATE;");
        std::error_code ec;
        const std::vector<std::pair<std::filesystem::path, std::string>> report_dirs = {
            {state_dir() / "reports" / "ds4", "ds4"},
            {state_dir() / "reports" / "caliber", "caliber-advisor"},
            {std::filesystem::current_path() / "tools" / "ui" / "static" / "reports", "ds4"},
            {std::filesystem::current_path() / "tools" / "ui" / "static" / "reports" / "caliber", "caliber-advisor"},
        };
        for (const auto & [directory, module] : report_dirs) {
            if (!std::filesystem::exists(directory, ec)) continue;
            for (const auto & entry : std::filesystem::directory_iterator(directory, ec)) {
                if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
                try {
                    import_report_file_locked(db.db, entry.path(), module);
                } catch (const std::exception & e) {
                    SRV_WRN("archive import skipped %s: %s\n", entry.path().string().c_str(), e.what());
                }
            }
        }
        rebuild_best_results_locked(db.db);
        exec_sql(db.db, "COMMIT;");
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
        exec_sql(db.db, "BEGIN IMMEDIATE;");
        record_report_locked(db.db, module, id, kind, status, title, path, payload);
        insert_event_locked(db.db, module, "report_saved", id, {{"status", status}, {"path", path.string()}});
        rebuild_best_results_locked(db.db);
        exec_sql(db.db, "COMMIT;");
    } catch (const std::exception & e) {
        SRV_WRN("archive report write failed: %s\n", e.what());
    }
}

void delete_report(const std::string & module, const std::string & id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        exec_sql(db.db, "BEGIN IMMEDIATE;");
        auto del_samples = prepare(db.db, "DELETE FROM samples WHERE module = ? AND report_id = ?;");
        bind_text(del_samples.stmt, 1, module);
        bind_text(del_samples.stmt, 2, id);
        step_done(db.db, del_samples.stmt);
        auto del_results = prepare(db.db, "DELETE FROM results WHERE module = ? AND report_id = ?;");
        bind_text(del_results.stmt, 1, module);
        bind_text(del_results.stmt, 2, id);
        step_done(db.db, del_results.stmt);
        auto del_report = prepare(db.db, "DELETE FROM reports WHERE module = ? AND id = ?;");
        bind_text(del_report.stmt, 1, module);
        bind_text(del_report.stmt, 2, id);
        step_done(db.db, del_report.stmt);
        rebuild_best_results_locked(db.db);
        insert_event_locked(db.db, module, "report_deleted", id, json::object());
        exec_sql(db.db, "COMMIT;");
    } catch (const std::exception & e) {
        SRV_WRN("archive report delete failed: %s\n", e.what());
    }
}

json load_report(const std::string & module, const std::string & id) {
    import_existing_reports_once();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto db = open_database_locked();
    auto stmt = prepare(db.db, "SELECT payload_json FROM reports WHERE module = ? AND id = ?;");
    bind_text(stmt.stmt, 1, module);
    bind_text(stmt.stmt, 2, id);
    if (sqlite3_step(stmt.stmt) != SQLITE_ROW) return nullptr;
    return col_json(stmt.stmt, 0);
}

json load_reports(const std::string & module) {
    import_existing_reports_once();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto db = open_database_locked();
    auto stmt = prepare(db.db,
        "SELECT payload_json FROM reports WHERE module = ? ORDER BY updated_at DESC, created_at DESC, id DESC;");
    bind_text(stmt.stmt, 1, module);
    json out = json::array();
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) out.push_back(col_json(stmt.stmt, 0));
    return out;
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

void record_job(const std::string & module, const std::string & id, const std::string & status, const json & payload) {
    if (module.empty() || id.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto db = open_database_locked();
        auto stmt = prepare(db.db,
            "INSERT INTO jobs(module, id, status, created_at, updated_at, payload_json) VALUES(?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(module, id) DO UPDATE SET status=excluded.status, updated_at=excluded.updated_at, payload_json=excluded.payload_json;");
        const std::string now = isoish_timestamp();
        bind_text(stmt.stmt, 1, module);
        bind_text(stmt.stmt, 2, id);
        bind_text(stmt.stmt, 3, status);
        bind_text(stmt.stmt, 4, first_string(payload, {"created_at", "started_at"}).empty() ? now : first_string(payload, {"created_at", "started_at"}));
        bind_text(stmt.stmt, 5, now);
        bind_text(stmt.stmt, 6, json_dump(payload));
        step_done(db.db, stmt.stmt);
    } catch (const std::exception & e) {
        SRV_WRN("job checkpoint write failed: %s\n", e.what());
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
        {"samples", scalar_count(db.db, "SELECT COUNT(*) FROM samples")},
        {"best_results", scalar_count(db.db, "SELECT COUNT(*) FROM best_results")},
        {"downloads", scalar_count(db.db, "SELECT COUNT(*) FROM downloads")},
        {"fit_recommendations", scalar_count(db.db, "SELECT COUNT(*) FROM fit_recommendations")},
        {"configurations", scalar_count(db.db, "SELECT COUNT(*) FROM configurations")},
        {"events", scalar_count(db.db, "SELECT COUNT(*) FROM events")},
        {"jobs", scalar_count(db.db, "SELECT COUNT(*) FROM jobs")},
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
        {"version", 2},
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
            {"samples", select_rows(db.db,
                "SELECT module, report_id, result_id, sample_id, payload_json, created_at FROM samples ORDER BY module, report_id, result_id, sample_id",
                {"module", "report_id", "result_id", "sample_id", "payload_json", "created_at"},
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
            {"jobs", select_rows(db.db,
                "SELECT module, id, status, created_at, updated_at, payload_json FROM jobs ORDER BY updated_at DESC",
                {"module", "id", "status", "created_at", "updated_at", "payload_json"},
                {"payload_json"})},
        }},
    };
    archive.erase("database_path");
    archive = redact_for_export(archive);
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
        const std::string archive_fingerprint = stable_fingerprint(archive);
        auto imported = prepare(db.db, "INSERT OR IGNORE INTO archive_imports(fingerprint, imported_at) VALUES(?, ?);");
        bind_text(imported.stmt, 1, archive_fingerprint);
        bind_text(imported.stmt, 2, isoish_timestamp());
        step_done(db.db, imported.stmt);
        const bool already_imported = sqlite3_changes(db.db) == 0;
        const json tables = archive.value("tables", json::object());
        if (!already_imported) {
            import_reports_table_locked(db.db, tables.value("reports", json::array()));
            import_downloads_table_locked(db.db, tables.value("downloads", json::array()));
            import_fit_table_locked(db.db, tables.value("fit_recommendations", json::array()));
            import_configurations_table_locked(db.db, tables.value("configurations", json::array()));
            import_events_table_locked(db.db, tables.value("events", json::array()));
            rebuild_best_results_locked(db.db);
        }
        exec_sql(db.db, "COMMIT;");
        res->headers["X-Archive-Already-Imported"] = already_imported ? "true" : "false";
    } catch (...) {
        exec_sql(db.db, "ROLLBACK;");
        throw;
    }
    res->data = safe_json_to_str({
        {"success", true},
        {"database_path", database_path().string()},
        {"reports", scalar_count(db.db, "SELECT COUNT(*) FROM reports")},
        {"results", scalar_count(db.db, "SELECT COUNT(*) FROM results")},
        {"samples", scalar_count(db.db, "SELECT COUNT(*) FROM samples")},
        {"downloads", scalar_count(db.db, "SELECT COUNT(*) FROM downloads")},
        {"fit_recommendations", scalar_count(db.db, "SELECT COUNT(*) FROM fit_recommendations")},
    });
    return res;
}

} // namespace server_persistence
