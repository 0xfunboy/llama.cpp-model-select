#!/usr/bin/env python3
"""Run a DS4-style frontier benchmark against llama-server router models.

The original ds4-bench walks one fixed token sequence to increasing context
frontiers, measures only the newly-prefilled suffix, probes generation speed,
then restores the frontier state before continuing.

llama-server gives us the same useful shape through:
  - /tokenize for model-specific token frontiers
  - /completion with token-array prompts, n_predict=0, and cache_prompt=true
  - /admin/switch for exclusive one-model-at-a-time measurements

Generation is measured as a separate probe after prefill. The runner then sends
the same frontier again with n_predict=0 so the server slot is returned to the
frontier before the next incremental prefill, matching ds4-bench's restore step
without writing slot snapshots to disk.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_BASE_URL = "http://127.0.0.1:8080"
DEFAULT_PROMPT_FILE = "/home/cooper/ds4/speed-bench/promessi_sposi.txt"
DEFAULT_KEY_FILE = "/home/cooper/.config/llama-server/qwen35-122b.api-key"


def read_api_key(path: str | None) -> str | None:
    if not path:
        return None
    p = Path(path).expanduser()
    if not p.exists():
        return None
    return p.read_text(encoding="utf-8").strip()


class RouterClient:
    def __init__(self, base_url: str, api_key: str | None):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key

    def request(self, method: str, path: str, payload: dict[str, Any] | None = None, timeout: int = 600) -> Any:
        data = None
        headers = {}
        if payload is not None:
            data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"

        req = urllib.request.Request(
            self.base_url + path,
            data=data,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as res:
                body = res.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"{method} {path} failed: HTTP {e.code}: {body}") from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"{method} {path} failed: {e}") from e

        if not body:
            return {}
        try:
            return json.loads(body)
        except json.JSONDecodeError as e:
            raise RuntimeError(f"{method} {path} returned non-JSON: {body[:500]}") from e

    def admin_models(self) -> list[dict[str, Any]]:
        data = self.request("GET", "/admin/models")
        return data.get("data", [])

    def switch(self, model: str) -> Any:
        return self.request("POST", "/admin/switch", {"model": model}, timeout=900)

    def tokenize(self, model: str, content: str) -> list[int]:
        data = self.request("POST", "/tokenize", {
            "model": model,
            "content": content,
            "add_special": False,
            "parse_special": True,
        }, timeout=900)
        return data["tokens"]

    def completion(self, model: str, prompt_tokens: list[int], gen_tokens: int) -> dict[str, Any]:
        return self.request("POST", "/completion", {
            "model": model,
            "prompt": prompt_tokens,
            "n_predict": gen_tokens,
            "temperature": 0,
            "top_k": 1,
            "top_p": 1,
            "repeat_penalty": 1.0,
            "cache_prompt": True,
            "id_slot": 0,
            "ignore_eos": True,
            "stream": False,
        }, timeout=1800)


def status_value(model: dict[str, Any]) -> str:
    status = model.get("status", "")
    if isinstance(status, dict):
        return str(status.get("value", ""))
    return str(status)


def model_ids_from_admin(models: list[dict[str, Any]]) -> list[str]:
    return [str(m["id"]) for m in models if "id" in m]


def loaded_models(models: list[dict[str, Any]]) -> list[str]:
    return [str(m["id"]) for m in models if status_value(m) == "loaded"]


def parse_model_list(value: str, available: list[str]) -> list[str]:
    if value == "all":
        return available
    requested = [item.strip() for item in value.split(",") if item.strip()]
    missing = [item for item in requested if item not in available]
    if missing:
        raise SystemExit(f"Unknown model(s): {', '.join(missing)}. Available: {', '.join(available)}")
    return requested


def frontiers(ctx_start: int, ctx_max: int, step_incr: int, step_mul: float) -> list[int]:
    if ctx_start <= 0 or ctx_max <= 0:
        raise SystemExit("ctx-start and ctx-max must be positive")
    if ctx_start > ctx_max:
        raise SystemExit("ctx-start must be <= ctx-max")
    if step_mul < 1:
        raise SystemExit("step-mul must be >= 1")
    if step_mul == 1 and step_incr <= 0:
        raise SystemExit("step-incr must be positive when step-mul is 1")

    out = []
    cur = ctx_start
    while True:
        out.append(cur)
        if cur >= ctx_max:
            return out
        if step_mul > 1:
            nxt = int(cur * step_mul)
        else:
            nxt = cur + step_incr
        if nxt <= cur:
            nxt = cur + 1
        cur = min(nxt, ctx_max)


def output_path(value: str | None) -> Any:
    if not value or value == "-":
        return sys.stdout
    p = Path(value).expanduser()
    p.parent.mkdir(parents=True, exist_ok=True)
    return p.open("w", newline="", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="DS4-style benchmark for llama-server router models")
    parser.add_argument("--base-url", default=os.environ.get("LLAMA_SERVER_URL", DEFAULT_BASE_URL))
    parser.add_argument("--api-key-file", default=os.environ.get("LLAMA_API_KEY_FILE", DEFAULT_KEY_FILE))
    parser.add_argument("--prompt-file", default=DEFAULT_PROMPT_FILE)
    parser.add_argument("--models", default="all", help="Comma-separated model ids, or 'all'")
    parser.add_argument("--ctx-start", type=int, default=2048)
    parser.add_argument("--ctx-max", type=int, default=8192)
    parser.add_argument("--step-incr", type=int, default=2048)
    parser.add_argument("--step-mul", type=float, default=1.0)
    parser.add_argument("--gen-tokens", type=int, default=64)
    parser.add_argument("--csv", default="-", help="Output CSV path, or '-' for stdout")
    parser.add_argument("--no-switch", action="store_true", help="Do not call /admin/switch before each model")
    parser.add_argument("--no-restore", action="store_true", help="Do not switch back to the initially loaded model")
    args = parser.parse_args()

    prompt_path = Path(args.prompt_file).expanduser()
    if not prompt_path.exists():
        raise SystemExit(f"Prompt file not found: {prompt_path}")
    prompt_text = prompt_path.read_text(encoding="utf-8")

    client = RouterClient(args.base_url, read_api_key(args.api_key_file))
    admin_before = client.admin_models()
    available = model_ids_from_admin(admin_before)
    if not available:
        raise SystemExit("No router models reported by /admin/models")
    targets = parse_model_list(args.models, available)
    initially_loaded = loaded_models(admin_before)
    restore_model = initially_loaded[0] if initially_loaded else None
    frontier_list = frontiers(args.ctx_start, args.ctx_max, args.step_incr, args.step_mul)

    out_file = output_path(args.csv)
    close_out = out_file is not sys.stdout
    writer = csv.DictWriter(out_file, fieldnames=[
        "model",
        "ctx_tokens",
        "target_prefill_tokens",
        "actual_prefill_tokens",
        "prefill_tps",
        "gen_tokens",
        "actual_gen_tokens",
        "gen_tps",
        "kvcache_bytes",
        "cache_tokens",
        "prompt_ms",
        "gen_ms",
        "total_ms",
    ])
    writer.writeheader()
    out_file.flush()

    last_model = None
    try:
        for model in targets:
            print(f"# model={model}: switching/loading", file=sys.stderr, flush=True)
            if not args.no_switch:
                client.switch(model)
            last_model = model

            print(f"# model={model}: tokenizing prompt", file=sys.stderr, flush=True)
            tokens = client.tokenize(model, prompt_text)
            if len(tokens) < args.ctx_max:
                print(
                    f"# model={model}: skipped, prompt has {len(tokens)} tokens < ctx-max {args.ctx_max}",
                    file=sys.stderr,
                    flush=True,
                )
                continue

            previous = 0
            for frontier in frontier_list:
                t0 = time.monotonic()
                prefix = tokens[:frontier]
                prefill_response = client.completion(model, prefix, 0)
                gen_response = prefill_response
                if args.gen_tokens > 0:
                    gen_response = client.completion(model, prefix, args.gen_tokens)
                    client.completion(model, prefix, 0)
                elapsed_ms = (time.monotonic() - t0) * 1000.0
                prefill_timings = prefill_response.get("timings", {})
                gen_timings = gen_response.get("timings", {})
                row = {
                    "model": model,
                    "ctx_tokens": frontier,
                    "target_prefill_tokens": frontier - previous,
                    "actual_prefill_tokens": prefill_timings.get("prompt_n", ""),
                    "prefill_tps": f"{float(prefill_timings.get('prompt_per_second', 0.0)):.2f}",
                    "gen_tokens": args.gen_tokens,
                    "actual_gen_tokens": gen_timings.get("predicted_n", ""),
                    "gen_tps": f"{float(gen_timings.get('predicted_per_second', 0.0)):.2f}",
                    "kvcache_bytes": 0,
                    "cache_tokens": prefill_timings.get("cache_n", ""),
                    "prompt_ms": f"{float(prefill_timings.get('prompt_ms', 0.0)):.2f}",
                    "gen_ms": f"{float(gen_timings.get('predicted_ms', 0.0)):.2f}",
                    "total_ms": f"{elapsed_ms:.2f}",
                }
                writer.writerow(row)
                out_file.flush()
                previous = frontier
    finally:
        if restore_model and not args.no_restore and not args.no_switch and last_model != restore_model:
            print(f"# restoring initially loaded model={restore_model}", file=sys.stderr, flush=True)
            try:
                client.switch(restore_model)
            except Exception as e:  # noqa: BLE001 - best-effort restore path
                print(f"# restore failed: {e}", file=sys.stderr, flush=True)
        if close_out:
            out_file.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
