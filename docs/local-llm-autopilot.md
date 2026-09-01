# Local LLM product guide

This fork turns the llama.cpp router into a local model control plane. It keeps
model discovery, download, runtime selection, benchmarking, task evaluation and
chat in one offline-first product without hiding how a recommendation was made.

## Product map

The primary navigation has two model-management destinations:

- **Models** opens the Fit Planner. Use it to discover local GGUF artifacts,
  inspect shard health, search the downloadable catalog, estimate hardware fit,
  run persistent downloads and write a router preset.
- **Optimize** opens Local LLM Autopilot. Use it to measure installed models,
  compare evidence and apply a proved configuration.

Optimize is organized around four tasks:

- **Overview** shows readiness, current evidence and the next useful action.
- **Run tests** plans and starts a resumable campaign.
- **Evidence** keeps performance and task quality separate. Performance uses
  DS4-Bench and Caliber measurements; Quality uses DS4-Eval packs.
- **Archive** combines saved Caliber and DS4 runs without changing their source
  or metric semantics.

The old `#/fit-advisor`, `#/ds4-eval` and `#/ds4-bench` URLs remain valid for
bookmarks and advanced use. They are not separate primary products.

Diagnostics is available from Overview. Least-cost routing remains an advanced
feature and is shown only when qualified evidence exists.

## Evidence labels

Do not compare rows without first checking their evidence label:

- **Estimated** is a hardware and model-metadata prior. It is useful before a
  download, but it is not a benchmark.
- **Synthetic measured** comes from `llama-bench`. It is useful for fast races
  and configuration search, but excludes normal chat tokenization and sampling.
- **Streaming measured** comes from real isolated `llama-server` requests and
  includes observed response behavior.
- **Quality tested** comes from completed DS4 cases for a named pack. Coverage
  and sample count matter; one pack is not a universal intelligence score.

The Fit Planner's capability prior is derived from metadata such as parameter
count, active MoE parameters, model family, task tags and quantization. It must
not be presented as measured model quality.

## Recommended workflow

1. Open **Models** and verify that the target artifact is complete and loadable.
2. Use Fit Planner to choose a quantization and context that fit the detected
   hardware. Treat throughput and capability values as estimates until measured.
3. Download through the managed queue. Downloads are resumable and survive a
   browser disconnect.
4. Configure the downloaded artifact as a router model.
5. Open **Optimize**, select the real use case and start with the shortest useful
   campaign. Hardware shortlist size controls how many candidates are proposed;
   it is not a promised run-time limit.
6. Let synthetic measurements narrow the candidates. Use streaming measurements
   for finalists and the matching DS4 pack for task quality.
7. Review speed, memory, context and quality as separate dimensions. Apply a
   winner only when the required gates pass.
8. Keep the report in Archive so a later runtime or driver change can be compared
   against the same evidence.

A full multi-model quality run can take hours or days. Server-side jobs preserve
progress and can be resumed after the page or browser is closed.

Configured presets are evidence identities, not only file paths. A base preset,
a LoRA preset and a different runtime preset that share one GGUF are measured and
reported separately. This prevents evidence from one launch configuration from
being applied to another.

## Current evaluator coverage

The bundled core suite is strongest at objective reasoning, science, coding and
security questions. Product packs add smoke coverage for general chat, FIM,
RAG, tool use and long context. Narrative quality, personality, uncensored
instruction following and media-prompt reliability require dedicated packs or
human pairwise review. The UI must show coverage and sample count rather than
claiming a universal best model.

## Local image and video generation

The chat UI can expose local image and video tools when a compatible media
controller is available through `/api/media`. The language model receives a
provider-neutral scene schema; the local controller compiles it for the chosen
checkpoint and reports queue, model, phase, progress, elapsed time and ETA.

Generated media is attached to the conversation, can be restored with chat
history and is removed from the media store when its message or conversation is
deleted. Prompt text and generated assets are not sent to a remote generation
service. Model registries and download providers may still receive normal model
download requests.

## Production start

Build `llama-server` and the embedded UI together, then start router mode with a
JSON preset and separate user and administrator credentials:

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

`aria2c` is required only for managed downloads. `llama-bench` must be installed
beside `llama-server` for synthetic racing. Final decision-grade measurements
use isolated streaming `llama-server` processes.

## Hardware profiles

Fit Planner detects AMD DRM devices and NVIDIA GPUs independently. A Strix Halo
UMA system selects `strix_halo_vulkan`; an NVIDIA system selects `nvidia_cuda`.
If both backends are present, select the intended planner explicitly:

```sh
LLAMA_FIT_ADVISOR_PROFILE=strix_halo_vulkan build/bin/llama-server ...
LLAMA_FIT_ADVISOR_PROFILE=nvidia_cuda build/bin/llama-server ...
```

The Strix profile counts the amdgpu GTT pool once, prefers trusted Vulkan GGUF
quantizations and uses local single-stream decode calibration. The CUDA profile
keeps per-GPU and aggregate VRAM separate and supports single-GPU, MultiGPU, MoE
offload and hybrid planning.

Keep machine-specific presets outside the source checkout. Use
`LLAMA_SERVER_WORKER` for one worker per host, or trusted per-model `worker` and
`runtime` preset fields when models require different builds. This prevents a
source update from replacing the active Strix Halo or CUDA configuration.

## State, privacy and migration

Defaults follow XDG:

- database: `$XDG_DATA_HOME/llama.cpp-model-select/platform.sqlite`;
- reports and operational state: `$XDG_STATE_HOME/llama.cpp-model-select/`;
- metadata and catalog cache: `$XDG_CACHE_HOME/llama.cpp-model-select/`.

The SQLite archive is canonical. JSON import and export is sanitized and omits
credentials, commands and host-specific paths. Treat it as a portable backup,
not as a second live database.

On first database access the server copies a legacy
`data/llm-model-select.sqlite` database only when no XDG database exists. Legacy
Caliber and DS4 reports are imported idempotently. Existing files remain in
place for downgrade or manual recovery.

Use Settings archive export and import when moving to another host. Logical
artifact identities survive; machine paths and runtime measurements do not.

## Failure behavior

Models, Caliber results, system diagnostics and DS4 evidence load independently.
A failed evidence endpoint must not blank the rest of Optimize. Long-running
jobs report their own status and remain resumable.

Reports become stale after the recorded llama.cpp build, worker runtime or GPU
driver changes. Stale and legacy reports remain visible in Archive with their
incompatibility reasons, but they cannot win a current comparison or be applied.
Rerun the relevant evidence before automatic FIT or routing. Fatal GPU allocation
errors stop the current campaign instead of retrying into a degraded device
state.

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
npx playwright install chromium
npx playwright test
npm run build
```

A release is not decision-ready until a real local campaign produces a
streaming-measured row with the required context, memory and quality evidence.
