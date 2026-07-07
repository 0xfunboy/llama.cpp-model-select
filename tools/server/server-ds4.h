#pragma once

#include "server-http.h"

#include <memory>

struct server_models_routes;

struct server_ds4_routes {
    explicit server_ds4_routes(server_models_routes & router);
    ~server_ds4_routes();

    server_ds4_routes(const server_ds4_routes &) = delete;
    server_ds4_routes & operator=(const server_ds4_routes &) = delete;

    server_http_context::handler_t get_models;
    server_http_context::handler_t post_run_eval;
    server_http_context::handler_t post_run_bench;
    server_http_context::handler_t get_job;
    server_http_context::handler_t get_job_events;
    server_http_context::handler_t get_reports;
    server_http_context::handler_t get_report;

private:
    struct impl;
    std::shared_ptr<impl> pimpl;

    void init_routes();
};
