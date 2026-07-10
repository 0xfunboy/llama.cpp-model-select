#pragma once

#include "server-http.h"
#include "server-common.h"

#include <filesystem>
#include <string>

namespace server_persistence {

std::filesystem::path database_path();
std::filesystem::path data_dir();
std::filesystem::path state_dir();
json redact_for_export(const json & value);

void import_existing_reports_once();

void record_report(
        const std::string & module,
        const std::string & id,
        const std::string & kind,
        const std::string & status,
        const std::string & title,
        const std::filesystem::path & path,
        const json & payload);

void delete_report(const std::string & module, const std::string & id);
json load_report(const std::string & module, const std::string & id);
json load_reports(const std::string & module);

void record_download(const json & snapshot);
void record_fit_recommendations(const json & response);
void record_configuration(const std::string & module, const std::string & model_id, const std::string & preset_id, const json & payload);

server_http_res_ptr handle_archive_status(const server_http_req & req);
server_http_res_ptr handle_archive_export(const server_http_req & req);
server_http_res_ptr handle_archive_import(const server_http_req & req);

} // namespace server_persistence
