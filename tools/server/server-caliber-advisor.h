#pragma once

#include "server-http.h"

#include <memory>

struct server_models_routes;

struct server_caliber_advisor_routes {
    explicit server_caliber_advisor_routes(server_models_routes & router);
    ~server_caliber_advisor_routes();

    server_caliber_advisor_routes(const server_caliber_advisor_routes &) = delete;
    server_caliber_advisor_routes & operator=(const server_caliber_advisor_routes &) = delete;

    server_http_context::handler_t get_system;
    server_http_context::handler_t get_models;
    server_http_context::handler_t post_plan;
    server_http_context::handler_t post_sweep;
    server_http_context::handler_t post_sweep_stop;
    server_http_context::handler_t get_sweep_events;
    server_http_context::handler_t get_sweep_status;
    server_http_context::handler_t get_results;
    server_http_context::handler_t get_reports;
    server_http_context::handler_t get_report;
    server_http_context::handler_t delete_report;
    server_http_context::handler_t post_configure;

private:
    struct impl;
    std::shared_ptr<impl> pimpl;

    void init_routes();
};
