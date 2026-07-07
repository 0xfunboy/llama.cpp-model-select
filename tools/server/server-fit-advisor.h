#pragma once

#include "server-http.h"

#include <memory>

struct server_models_routes;

struct server_fit_advisor_routes {
    explicit server_fit_advisor_routes(server_models_routes & router);
    ~server_fit_advisor_routes();

    server_fit_advisor_routes(const server_fit_advisor_routes &) = delete;
    server_fit_advisor_routes & operator=(const server_fit_advisor_routes &) = delete;

    server_http_context::handler_t get_system;
    server_http_context::handler_t get_models;
    server_http_context::handler_t post_catalog_refresh;
    server_http_context::handler_t post_download;
    server_http_context::handler_t post_configure;

private:
    struct impl;
    std::shared_ptr<impl> pimpl;

    void init_routes();
};
