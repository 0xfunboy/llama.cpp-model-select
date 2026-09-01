#include "server-caliber-advisor.h"
#include "server-fit-advisor.h"
#include "server-models.h"
#include "server-persistence.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

void write_file(const std::filesystem::path & path, const std::string & value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << value;
}

json find_by_id(const json & items, const std::string & id) {
    if (!items.is_array()) return nullptr;
    for (const auto & item : items) {
        if (item.is_object() && item.value("id", std::string()) == id) return item;
    }
    return nullptr;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "llama-persistence-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    setenv("XDG_DATA_HOME", (root / "data").string().c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "state").string().c_str(), 1);
    setenv("XDG_CACHE_HOME", (root / "cache").string().c_str(), 1);
    setenv("LLAMA_CACHE", (root / "cache").string().c_str(), 1);
    setenv("LLAMA_MODEL_SELECT_LEGACY_DB", (root / "missing.sqlite").string().c_str(), 1);

    const auto original_cwd = std::filesystem::current_path();
    std::filesystem::current_path(root);
    server_persistence::import_existing_reports_once();
    std::filesystem::current_path(original_cwd);

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

    server_persistence::record_route_decision({{"id", "route-1"}, {"alias", "local-auto"}, {"selected_model", "gguf-test"}, {"api_key", "must-not-export"}});
    server_persistence::record_route_feedback("route-1", {{"rating", 1}, {"note", "worked"}});
    const json route_events = server_persistence::load_route_events();
    require(route_events.size() == 2 && route_events.at(0).at("event_type") == "feedback", "route decisions and feedback persist locally");

    server_persistence::record_configuration("caliber-advisor", "gguf-test", "preset", {{"model_path", "/home/person/models/model.gguf"}});
    const std::function<bool()> stop = []() { return false; };
    auto export_response = server_persistence::handle_archive_export(request_with_body("", stop));
    require(export_response->data.find("super-secret") == std::string::npos, "archive omits secrets");
    require(export_response->data.find("must-not-export") == std::string::npos, "route event secrets are redacted");
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

    const auto text_model = root / "models" / "text" / "measured-7b-Q4_K_M.gguf";
    const auto image_model = root / "models" / "media" / "image" / "zimage" / "qwen-4b-zimage-hereticv2-Q8_0.gguf";
    const auto video_model = root / "models" / "media" / "video" / "wan2.2-ti2v-5b-Q8_0.gguf";
    write_file(text_model, "test model");
    write_file(image_model, "test image encoder");
    write_file(video_model, "test video model");

    const json preset = {
        {"version", 1},
        {"models", json::array({
            {{"id", "measured-7b"}, {"model", text_model.string()}, {"ctx_size", 32768}},
            {{"id", "measured-7b-lora"}, {"model", text_model.string()}, {"ctx_size", 32768}},
            {{"id", "qwen-4b-zimage-hereticv2"}, {"model", image_model.string()}, {"ctx_size", 32768}},
            {{"id", "wan2.2-ti2v-5b"}, {"model", video_model.string()}, {"ctx_size", 32768}},
        })},
    };
    const auto preset_path = root / "models.json";
    write_file(preset_path, preset.dump(2));

    const json remote_without_repo = {
        {"name", "Qwen3-7B-GGUF"},
        {"provider", "unsloth"},
        {"parameter_count", "7B"},
        {"parameters_raw", 7000000000.0},
        {"quantization", "Q4_K_M"},
        {"format", "gguf"},
        {"context_length", 32768},
        {"pipeline_tag", "text-generation"},
        {"architecture", "qwen3"},
    };
    write_file(root / "cache" / "fit-advisor" / "hf_models.json", json::array({remote_without_repo}).dump());

    const json bench_report = {
        {"id", "canonical-ds4-bench"},
        {"kind", "bench"},
        {"status", "completed"},
        {"created_at", "2026-07-11T00:00:00Z"},
        {"updated_at", "2026-07-11T00:00:01Z"},
        {"results", json::array({
            {{"model", "measured-7b"}, {"ctx", 2048}, {"decode_tokens_per_second", 42.5}},
            {{"model", "measured-7b"}, {"ctx", 4096}, {"decode_tokens_per_second", 39.0}},
        })},
    };
    server_persistence::record_report("ds4-bench", "canonical-ds4-bench", "bench", "completed", "measured-7b", root / "canonical-ds4-bench.json", bench_report);

    common_params params;
    params.models_dir = (root / "registry-root").string();
    params.models_preset = preset_path.string();
    std::filesystem::create_directories(params.models_dir);
    char executable[] = "test-server-persistence";
    char * argv[] = {executable};
    server_models_routes models(params, 1, argv);
    server_fit_advisor_routes fit_advisor(models);
    server_http_req fit_request = {
        {{"include_too_tight", "true"}, {"min_tps", "0"}, {"context", "2048"}, {"limit", "100"}},
        {}, "", "", "", {}, stop,
    };
    const auto fit_response = fit_advisor.get_models(fit_request);
    require(fit_response->status == 200, "Fit Planner regression request succeeds");
    const json fit_payload = json::parse(fit_response->data);

    const json remote_row = find_by_id(fit_payload.value("models", json::array()), "Qwen3-7B-GGUF");
    require(remote_row.is_object(), "catalog model without a repo remains visible");
    require(remote_row.is_object() && !remote_row.value("configured", true), "empty repositories never match an installed model");
    require(remote_row.is_object() && remote_row.value("installed_model_id", std::string()).empty(), "empty repository match has no installed identity");

    const json measured_row = find_by_id(fit_payload.value("models", json::array()), "measured-7b");
    require(measured_row.is_object(), "local text-generation model remains recommended");
    require(measured_row.is_object() && measured_row.value("throughput_measured", false), "canonical DS4-Bench report supplies measured throughput");
    require(measured_row.is_object() && std::fabs(measured_row.value("estimated_tps", 0.0) - 42.5) < 0.001, "nearest canonical DS4-Bench context is selected");

    require(find_by_id(fit_payload.value("models", json::array()), "qwen-4b-zimage-hereticv2").is_null(), "image text encoder is excluded from LLM recommendations");
    require(find_by_id(fit_payload.value("models", json::array()), "wan2.2-ti2v-5b").is_null(), "video GGUF is excluded from LLM recommendations");
    require(find_by_id(fit_payload.value("installed", json::array()), "qwen-4b-zimage-hereticv2").is_object(), "image text encoder remains in the installed inventory");
    require(find_by_id(fit_payload.value("installed", json::array()), "wan2.2-ti2v-5b").is_object(), "video GGUF remains in the installed inventory");

    server_caliber_advisor_routes caliber_advisor(models);
    const server_http_req caliber_request = {{}, {}, "", "", "", {}, stop};
    const auto caliber_response = caliber_advisor.get_models(caliber_request);
    require(caliber_response->status == 200, "Optimize model inventory request succeeds");
    const json caliber_payload = json::parse(caliber_response->data);
    const json measured_base = find_by_id(caliber_payload.value("data", json::array()), "measured-7b");
    const json measured_lora = find_by_id(caliber_payload.value("data", json::array()), "measured-7b-lora");
    require(measured_base.is_object() && measured_lora.is_object(), "presets sharing one GGUF remain separate Optimize candidates");
    require(measured_base.is_object() && measured_lora.is_object() &&
            measured_base.value("artifact_id", std::string()) == measured_lora.value("artifact_id", std::string()),
            "separate preset candidates retain their shared artifact identity");
    require(measured_base.is_object() && measured_base.value("configured_ids", json::array()) == json::array({"measured-7b"}),
            "base Optimize candidate exposes only its configured identity");
    require(measured_lora.is_object() && measured_lora.value("configured_ids", json::array()) == json::array({"measured-7b-lora"}),
            "LoRA Optimize candidate exposes only its configured identity");
    const json caliber_image = find_by_id(caliber_payload.value("data", json::array()), "qwen-4b-zimage-hereticv2");
    const json caliber_video = find_by_id(caliber_payload.value("data", json::array()), "wan2.2-ti2v-5b");
    require(caliber_image.is_object() && !caliber_image.value("benchmark_eligible", true), "image artifact is not Caliber benchmark eligible");
    require(caliber_video.is_object() && !caliber_video.value("benchmark_eligible", true), "video artifact is not Caliber benchmark eligible");

    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " persistence test(s) failed\n";
        return 1;
    }
    std::cout << "server persistence tests passed\n";
    return 0;
}
