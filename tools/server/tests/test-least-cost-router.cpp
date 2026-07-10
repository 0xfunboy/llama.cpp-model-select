#include "least-cost-router.h"

#include <cstdlib>
#include <iostream>

using least_cost_router::json;

static void require(bool value, const char * message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }

static json row(const char * model, const char * artifact, double tps, double quality, int ctx, const char * pack = "overall") {
    return {{"id", model}, {"model", model}, {"artifact_id", artifact}, {"ok", true}, {"benchmark_backend", "llama-server-streaming"},
        {"context_target_met", true}, {"fit_eligible", true}, {"quality_gate_passed", true}, {"quality_policy", {{"pack", pack}}},
        {"quality_evidence", {{"score", quality}, {"samples", 8}}}, {"ctx_size", ctx}, {"eval_tps", tps}, {"e2e_ttft_ms", 200}, {"load_sec", 8}};
}

int main() {
    require(least_cost_router::is_virtual_alias("local-auto"), "known alias");
    const std::vector<json> candidates = {
        {{"model", "fast"}, {"artifact_id", "a"}, {"resident", false}, {"tags", {"chat"}}},
        {{"model", "resident"}, {"artifact_id", "b"}, {"resident", true}, {"tags", {"chat"}}},
        {{"model", "code"}, {"artifact_id", "c"}, {"resident", false}, {"tags", {"coding", "tools"}}},
    };
    const std::vector<json> reports = {json{
        {"status", "completed"},
        {"rows", json::array({
            row("fast", "a", 60, 0.90, 32768),
            row("resident", "b", 55, 0.88, 32768),
            row("code", "c", 40, 0.95, 32768, "coding"),
        })},
    }};
    const auto automatic = least_cost_router::select("local-auto", {{"messages", {{{"content", "hello"}}}}, {"max_tokens", 128}}, candidates, reports, json::array(), 10000);
    require(automatic.value("ok", false) && automatic.value("selected_model", std::string()) == "resident", "qualified resident preference");
    const auto coding = least_cost_router::select("local-code", {{"messages", {{{"content", "write code"}}}}}, candidates, reports, json::array(), 20000);
    require(coding.value("selected_model", std::string()) == "code", "coding pack filter");
    const auto too_long = least_cost_router::select("local-long", {{"prompt", std::string(200000, 'x')}, {"max_tokens", 1000}}, candidates, reports, json::array(), 30000);
    require(!too_long.value("ok", true), "context filter fails closed");
    std::cout << "least cost router tests passed\n";
}
