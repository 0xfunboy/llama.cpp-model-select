# Local LLM Autopilot operator guide

## Production start

Build `llama-server` and the embedded UI together, then start router mode with a
JSON preset and separate user/admin credentials:

```sh
cmake -S . -B build -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TESTS=ON
cmake --build build --target llama-server -j
build/bin/llama-server \
  --models-preset /absolute/path/models.json \
  --models-max 1 \
  --models-autoload \
  --host 127.0.0.1 \
  --port 8080 \
  --api-key-file /run/secrets/llama-user.key \
  --admin-api-key-file /run/secrets/llama-admin.key
```

`aria2c` is optional unless managed Hugging Face downloads are required.
`llama-bench` must be installed beside `llama-server` for synthetic racing. Final
recommendations use isolated streaming `llama-server` processes.

## Runtime directories

The source and installation directories stay read-only. Defaults follow XDG:

- database: `$XDG_DATA_HOME/llama.cpp-model-select/platform.sqlite`;
- reports and operational state: `$XDG_STATE_HOME/llama.cpp-model-select/`;
- metadata/catalog cache: `$XDG_CACHE_HOME/llama.cpp-model-select/`.

JSON archive export is sanitized: credentials, commands and host-specific paths
are removed. Treat it as a portable backup, not as a live database.

## First run

1. Open **Doctor** and resolve missing binaries or an unwritable state database.
2. Open **Library**, refresh discovery and repair incomplete/corrupt shard sets.
3. On **Home**, choose the task, normal context and allowed test time.
4. Run the **Test Lab** plan. Synthetic rows only race candidates; finalists are
   profiled with streaming repetitions and the selected quality pack.
5. Apply the qualified recommendation, or address it through a `local-*` alias.

Doctor marks older reports legacy when they predate runtime fingerprints. Reports
become stale after the recorded llama.cpp commit or GPU driver changes and must be
rerun before they can be trusted for automatic routing.

## Migration

On first database access the server non-destructively copies the legacy
`data/llm-model-select.sqlite` database when no XDG database exists. Legacy static
Caliber/DS4 reports are imported idempotently. Existing files are preserved for
downgrade or manual recovery.

For an explicit move, set `LLAMA_MODEL_SELECT_LEGACY_DB` to the old SQLite path
before the first start. Use Settings archive export/import when moving to another
host; logical artifact identities survive while host paths do not.

## Release verification

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
cd tools/ui
npm run lint
npm run check
npm run test:ui -- --run
npm run test:client -- --run
npm run test:unit -- --run
npx playwright test
npm run build
```

Do not treat a release as production-ready unless a real local campaign produces
a streaming-measured, quality-qualified winner and a virtual alias either routes
to it or fails closed with an explanation.
