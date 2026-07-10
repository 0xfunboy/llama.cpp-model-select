#include "server-persistence.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

int failures = 0;

void require(bool condition, const std::string & message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

std::string scalar_text(const std::string & sql) {
    sqlite3 * db = nullptr;
    sqlite3_open(server_persistence::database_path().string().c_str(), &db);
    sqlite3_stmt * stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char * value = sqlite3_column_text(stmt, 0);
        if (value) out = reinterpret_cast<const char *>(value);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

int64_t scalar_int(const std::string & sql) {
    const std::string value = scalar_text(sql);
    return value.empty() ? 0 : std::stoll(value);
}

server_http_req request_with_body(const std::string & body, const std::function<bool()> & stop) {
    return {{}, {}, "", "", body, {}, stop};
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "llama-persistence-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    setenv("XDG_DATA_HOME", (root / "data").string().c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "state").string().c_str(), 1);
    setenv("LLAMA_MODEL_SELECT_LEGACY_DB", (root / "missing.sqlite").string().c_str(), 1);

    require(server_persistence::database_path() == root / "data" / "llama.cpp-model-select" / "platform.sqlite", "XDG database path");
    const json secret = {
        {"api_key", "super-secret"},
        {"model_path", "/home/person/models/model.gguf"},
        {"paths", json::array({"/home/person/models/a.gguf", "/tmp/b.gguf"})},
    };
    const json redacted = server_persistence::redact_for_export(secret);
    require(redacted.at("api_key") == "[REDACTED]", "secret redaction");
    require(redacted.at("model_path") == "<local>/model.gguf", "model path redaction");
    require(redacted.at("paths").at(0) == "<local>/a.gguf", "path array redaction");

    const auto report = [&](const std::string & id, double score) {
        return json{
            {"id", id},
            {"status", "completed"},
            {"created_at", "2026-07-10T00:00:00Z"},
            {"api_key", "super-secret"},
            {"rows", json::array({{
                {"id", "preset"},
                {"artifact_id", "gguf-test"},
                {"workload_kind", "baseline"},
                {"ok", true},
                {"eval_tps", score},
                {"model_path", "/home/person/models/model.gguf"},
                {"runs", json::array({{{"run_index", 0}, {"eval_tps", score}}, {{"run_index", 1}, {"eval_tps", score + 1}}})},
            }})},
        };
    };
    server_persistence::record_report("caliber-advisor", "slow", "campaign", "completed", "slow", "/home/person/slow.json", report("slow", 10));
    server_persistence::record_report("caliber-advisor", "fast", "campaign", "completed", "fast", "/home/person/fast.json", report("fast", 20));
    require(scalar_text("SELECT report_id FROM best_results WHERE module='caliber-advisor'") == "fast", "best result selects fastest report");
    require(scalar_int("SELECT COUNT(*) FROM samples") == 4, "immutable run samples stored separately");
    server_persistence::delete_report("caliber-advisor", "fast");
    require(scalar_text("SELECT report_id FROM best_results WHERE module='caliber-advisor'") == "slow", "deleting winner promotes next result");

    server_persistence::record_configuration("caliber-advisor", "gguf-test", "preset", {{"model_path", "/home/person/models/model.gguf"}});
    const std::function<bool()> stop = []() { return false; };
    auto export_response = server_persistence::handle_archive_export(request_with_body("", stop));
    require(export_response->data.find("super-secret") == std::string::npos, "archive omits secrets");
    require(export_response->data.find("/home/person") == std::string::npos, "archive omits host paths");
    const json archive = json::parse(export_response->data);
    require(!archive.contains("database_path"), "archive omits database path");

    const int64_t configurations_before_import = scalar_int("SELECT COUNT(*) FROM configurations");
    auto first_import = server_persistence::handle_archive_import(request_with_body(archive.dump(), stop));
    const int64_t configurations_after_first = scalar_int("SELECT COUNT(*) FROM configurations");
    auto second_import = server_persistence::handle_archive_import(request_with_body(archive.dump(), stop));
    require(first_import->status == 200 && second_import->status == 200, "archive imports succeed");
    require(configurations_after_first == configurations_before_import, "round-trip preserves configuration identity");
    require(second_import->headers.at("X-Archive-Already-Imported") == "true", "repeated archive is detected");
    require(scalar_int("SELECT COUNT(*) FROM configurations") == configurations_after_first, "repeated import is idempotent");

    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " persistence test(s) failed\n";
        return 1;
    }
    std::cout << "server persistence tests passed\n";
    return 0;
}
