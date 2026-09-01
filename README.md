# Local LLM Autopilot for llama.cpp

## Which local model should I run?

This fork answers that question on the hardware in front of you. Point it at one
or more GGUF directories and open **Local LLM Autopilot** in the embedded UI. It
discovers complete artifacts, detects broken shards and duplicates, normalizes
long model names into readable product labels, plans safe runtime settings,
races candidates, measures finalists through real streaming `llama-server`
requests, applies use-case quality evidence, and presents one answer with
inspectable alternatives.

The same control plane supports hardware-specific workers and presets. It keeps
NVIDIA CUDA planning intact and adds an AMD Strix Halo profile for full Vulkan
offload through unified memory.

The answer includes the exact context, KV cache, GPU offload/split, MoE, batch and
thread preset that was proved on this machine. Estimates, synthetic benchmarks,
streaming measurements and quality results are labelled separately; only a
context-verified, memory-observed, streaming-measured and quality-qualified row
can be applied automatically.

The product has two primary model-management destinations:

- **Models** - artifact discovery and health, hardware fit planning, persistent
  downloads and router configuration. Values derived from metadata are labelled
  Estimated; they are not presented as benchmarks.
- **Optimize** - the guided measurement workflow. Overview explains readiness,
  Run tests starts resumable campaigns, Evidence keeps performance and quality
  separate, and Archive combines saved Caliber and DS4 runs.

DS4-Eval and DS4-Bench remain available as advanced engines and through their
existing URLs, but they are not separate top-level products. Diagnostics is
available from Optimize. Least-cost routing is shown only after the machine has
qualified evidence for it.

Everything is offline-first. Runtime state lives under the platform XDG data and
state directories; sanitized JSON is an import/export format, not a second store.
See [the operator and migration guide](docs/local-llm-autopilot.md).

Development pushes use one fork-specific workflow for UI, CPU/server, Vulkan and CUDA compile checks.
See [Fork CI and safe pushes](docs/fork-ci.md) for the definition of a CI-clean commit and the local pre-push gate.

This fork tracks upstream `ggml-org/llama.cpp` and keeps inference on native
`llama-server` router mode. The upstream README continues below this fork
overview.

### Product capabilities

- **Native model selector UI** backed by router presets in `models.json`.
- **Admin router control API** for lifecycle-safe model switching:
    - `GET /admin/models`
    - `POST /admin/switch`
- **Models and Fit Planner** at `/fit-advisor` for discovery, downloads and
  estimated fit before a model is measured.
- **Optimize dashboard** at `/caliber-advisor`; Caliber and DS4 evidence are
  integrated into one guided workflow.
- **Fit planning engine**, backed by a native C++ advisor
  inspired by [`AlexsJones/llmfit`](https://github.com/AlexsJones/llmfit).
- **Caliber engine**, a Svelte/native-router
  port of core planning, scoring, guided-run, benchmark and reporting ideas from
  [`SpeederX/calibr`](https://github.com/SpeederX/calibr).
- **Evidence Lab** for resumable DwarfStar-style quality runs plus
  built-in general, chat, coding, reasoning, FIM, RAG, tools and long-context
  packs, inspired by [`antirez/ds4`](https://github.com/antirez/ds4).
- **Model report and comparison views** with sector summaries, winner selection,
  per-model drilldowns, and historical comparison across completed runs.
- **aria2c Hugging Face download manager** for resumable GGUF downloads.
- **Router preset writer** that can add downloaded or selected models directly to
  the active `--models-preset` JSON file.
- **Local SQLite archive** for benchmark reports, download state, Fit Planner
  recommendations and configured presets, with import/export from Settings.
- **Least-cost virtual models**: `local-auto`, `local-fast`, `local-best`,
  `local-code`, `local-long`, and `local-vision`.
- **External worker dispatch** through `LLAMA_SERVER_WORKER`, allowing a control
  plane build to launch a separate CUDA or Vulkan `llama-server` runtime.
- **Per-model runtime selection** through trusted preset `worker` paths, with the
  resolved runtime and worker shown in the model selector and router API.
- **Local media tools** that attach generated images and videos to chat history,
  expose backend progress and remove owned assets when a message or conversation
  is deleted.

### Credits And Imported Work

This fork keeps upstream `llama.cpp` as the runtime foundation and integrates
local-model operations ideas from several focused projects:

- Thanks to **SpeederX** for [`calibr`](https://github.com/SpeederX/calibr). The
  Caliber Advisor module adapts its guided workflow, fit planning, winner
  profiles, benchmark result model and rich report layout into the native
  `llama-server` router UI.
- Thanks to **Salvatore Sanfilippo / antirez** for
  [`ds4`](https://github.com/antirez/ds4). The DS4-Eval workflow and
  DwarfStar-style quality checks in this fork build on that project family and
  its model-evaluation focus.
- Thanks to **Alex Jones** for [`llmfit`](https://github.com/AlexsJones/llmfit).
  Fit Planner uses the same practical premise: rank GGUF candidates against the
  actual machine before downloading or configuring them.

### Router And Model Lifecycle Features

- Runs best in `llama-server` router mode with `--models-preset`.
- Supports admin authentication separately from normal API authentication.
- Enforces exclusive model switching: when a model is selected, other running or
  downloading model instances are stopped before the new model is loaded.
- Serializes automatic router autoload operations so separate UI panels cannot
  trigger competing model loads at the same time.
- Uses llama.cpp child-process isolation so unloading a model clears that child
  instance's slots, KV cache, sampler state, and CUDA allocations.
- Keeps the OpenAI-compatible request path native to `llama-server`; the model is
  selected through router mode rather than a separate proxy stack.

### Models And Fit Planner Features

- Scores GGUF catalog models against the host's CPU, total RAM, single-GPU VRAM,
  and aggregate multi-GPU VRAM.
- Uses total RAM capacity for fit planning while still displaying currently free
  RAM as an operational status value.
- Detects AMD Vulkan UMA capacity from amdgpu sysfs and NVIDIA VRAM from
  `nvidia-smi`, exposing both per-GPU and aggregate memory estimates.
- Treats Strix Halo GTT as one shared CPU/GPU memory pool, avoiding double-counted
  capacity and inappropriate CPU-offload recommendations.
- Supports strategy modes:
    - `Balanced`
    - `MultiGPU`
    - `MoE offload`
    - `Hybrid offload`
- Estimates weight memory, KV cache memory, overhead, runtime mode, fit level,
  rough tokens/sec, and score components.
- Runtime modes include:
    - `gpu_single`
    - `layer_split`
    - `moe_offload`
    - `cpu_offload`
    - `cpu_only`
- Pulls and caches the llmfit Hugging Face GGUF catalog.
- Filters by use case, minimum fit, quantization, search text, result limit, and
  selectable context preset.
- Context selection is a dropdown with common tiers; the guided product defaults
  to a realistic `32k` and requires explicit selection for extreme contexts.
- Score output is decomposed into a capability prior, speed, fit, context and
  capacity components. The capability prior is an Estimated metadata heuristic,
  not measured intelligence.
- High-capacity hosts weight long-context, larger coding/reasoning models more
  heavily than tiny fast models.
- Generates recommended command args and router preset JSON, including tensor
  split, MoE offload, KV cache quantization, flash attention, and reasoning flags
  when appropriate.
- Avoids absolute user paths in generated presets when a relative path can be
  written safely.
- Persists recommendations, download state and FIT configurations into the local
  archive so they can be backed up or compared later.

### Optimize And Caliber Features

- Provides a guided workflow for selecting the target use case, benchmark depth,
  candidate models and context target.
- Lists installed router models and can select every available model for a
  campaign while skipping models that already have successful historical
  measurements.
- Treats every configured launch preset as a separate evidence identity, even
  when base, LoRA or runtime variants share the same GGUF artifact.
- Uses hardware shortlist size only to bound candidate selection; it does not
  present an unimplemented wall-clock limit as a guarantee.
- Reuses Models downloads and Fit Planner configuration, so a catalog model can be
  downloaded, configured and then measured without leaving the UI.
- Plans benchmark sweeps from GGUF metadata, target context, KV-cache settings,
  offload mode and host hardware.
- Runs campaigns server-side, preserving progress if the browser is closed.
- Stores canonical reports in SQLite under the XDG data directory and writes JSON
  only as a derived view/export. Legacy static reports are imported once.
- Aggregates all completed reports into a historical comparison view; later runs
  remain comparable with earlier runs without repeating benchmarks for the same
  model.
- Keeps stale and legacy reports visible in Archive with compatibility reasons,
  while excluding them from current winners and automatic configuration.
- Offers winner profiles for:
    - daily-driver balance
    - fastest response
    - speed per watt/GB
    - safest memory fit
- Renders a Calibr-style report with hardware strip, data-scope controls,
  winner criteria, memory-vs-latency scatter, per-model expandable rows,
  measured config tables and throughput/memory bars.
- Keeps `FIT winner` actions on recommended winners, writing known-good launch
  settings back to the active router preset.
- Marks synthetic `llama-bench` rows explicitly so benchmark output is not
  confused with full streaming chat telemetry.

### Evidence Lab And DS4 Features

- Runs the test suite from `tools/server/ds4-eval-cases.json`.
- Uses the same model registry, compact labels, configured IDs and aliases as
  Models, Run tests and Optimize results.
- Shows newly discovered GGUF artifacts immediately while keeping unconfigured
  artifacts disabled until Models writes a router preset that DS4 can load.
- Covers multiple subject sectors and product packs, including general,
  chat, coding, reasoning, FIM, RAG, tools and long context where present in the
  suite.
- Supports model subsets and an `All new` action that selects configured models
  without a completed DS4 score. Previously evaluated models require deliberate
  manual selection for a retest.
- Uses bounded per-case generation budgets, a repetition detector and a
  per-case safety limit. Degenerate output is recorded as failed evidence and
  the campaign advances instead of stalling or discarding prior work.
- Continues after an isolated generation failure while preserving the error and
  stop reason in the report for diagnosis.
- Skips a model that cannot load, records the load error and continues with the
  remaining selection instead of aborting a multi-model campaign.
- Supports direct launch from the Run tests quality scope.
- Streams long-running job progress to the UI.
- Keeps job state visible after page changes or browser reconnects.
- Saves partial reports on stop and supports resuming compatible reports.
- Stores canonical reports through the shared report archive and exposes JSON as
  a derived view/export.
- Provides a model report panel below test logs with sector-level percentages.
- Supports single-model summaries and race-style comparison between models from
  completed evaluation runs.
- Feeds Optimize with weighted capability rankings so sector fit is based
  on a mix of measured skills, evidence coverage and sample confidence rather
  than a single raw score.
- Retains completed model results as historical benchmark data while allowing
  interrupted or pending reports to be deleted.

### Archive And Backup Features

- Stores operational data in
  `$XDG_DATA_HOME/llama.cpp-model-select/platform.sqlite` (normally
  `~/.local/share/llama.cpp-model-select/platform.sqlite`).
- Provides Settings import/export for portable backups of:
    - Fit Planner recommendations
    - download states
    - FIT configurations
    - Caliber reports and measured rows
    - DS4-Eval reports and best results
- Keeps completed benchmark data available for future comparison until a better
  result replaces it or the report is explicitly removed.

### Download Manager Features

- Uses background `aria2c` jobs for high-throughput resumable Hugging Face GGUF
  downloads.
- Downloads into the router models directory, defaulting to
  `$HOME/models/<model-name>/` when no `--models-dir` is configured.
- Reads Hugging Face auth from `HF_TOKEN` or `~/.cache/huggingface/token`.
- Resolves target GGUF files from selected quantization, including sharded GGUF
  sets.
- Exposes download states:
    - `available`
    - `queued`
    - `resolving`
    - `downloading`
    - `downloaded`
    - `configured`
    - `failed`
- Exposes download monitor endpoints:
    - `GET /api/fit-advisor/downloads`
    - `GET /api/fit-advisor/downloads/sse`
- The UI shows downloaded bytes, total size, percent, speed, target directory,
  errors, and enables `FIT` after the model reaches `downloaded`.

### Compatibility And Bug Fixes

- Normalizes incompatible assistant history when switching between model families
  with different chat templates, avoiding Jinja failures such as assistant
  non-text chunk errors.
- Flattens non-portable historical reasoning chunks when a target template only
  accepts text assistant content.
- Prevents automatic model load races across independent UI sections.
- Keeps Run tests checkbox controls visually and behaviorally independent.
- Aligns use-case choice cards and normalizes model labels across Models,
  Optimize and Evidence.
- Deprioritizes creative or uncensored finetunes for DS4-style correctness and
  disables generated reasoning flags for those presets when appropriate.
- Fixes advisor ranking issues caused by using transient free RAM instead of total
  host RAM capacity.
- Fixes advisor ranking issues caused by an 8k context default that overvalued
  small short-context models.

### Local Operator Notes

- This fork is intended to run primarily in router mode:

```sh
llama-server \
  --models-preset ./models.json \
  --models-max 1 \
  --models-autoload \
  --admin-api-key-file /path/to/admin-api-key
```

- On the Strix Halo host, use `/home/funboy/ai/bin/llama-strix-console`. Its
  machine preset is `/home/funboy/ai/config/strix-halo-models.json`, and all
  managed models remain rooted at `/home/funboy/models`.
- The multi-node Thunderbolt architecture and qualification gates are documented
  in [`docs/strix-halo-cluster.md`](docs/strix-halo-cluster.md).

- `aria2c` is required for Fit Planner managed downloads.
- `models.json` in this fork is a local preset file and may contain machine-specific
  paths; adjust it when reinstalling on a different host.

![llama](https://raw.githubusercontent.com/ggml-org/llama.brand/refs/heads/master/cover/llama-cpp/cover-llama-cpp-dark.svg)

<div align="center">

<b>LLM inference in C/C++</b>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/ggml-org/llama.cpp?filter=v*&color=brightgreen)](https://github.com/ggml-org/llama.cpp/releases?q=tag:v0)
[![Nightly](https://img.shields.io/github/v/release/ggml-org/llama.cpp?label=nightly&filter=b*&color=orange)](https://github.com/ggml-org/llama.cpp/releases?q=b)
[![Server](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/server.yml?label=Server)](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml)
[![Docker](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/docker.yml?label=Docker)](https://github.com/ggml-org/llama.cpp/actions/workflows/docker.yml)
[![Winget](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/winget.yml?label=Winget)](https://github.com/ggml-org/llama.cpp/actions/workflows/winget.yml)

[ggml](https://github.com/ggml-org/ggml) / [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md) / [maintainer PRs](https://github.com/ggml-org/llama.cpp/issues?q=is%3Apr%20is%3Aopen%20draft%3AFalse%20(author%3Argerganov%20OR%20author%3AKitaitiMakoto%20OR%20author%3Adanbev%20OR%20author%3Aaldehir%20OR%20author%3Amax-krasnyansky%20OR%20author%3ACISC%20OR%20author%3Aggerganov%20OR%20author%3Aam17an%20OR%20author%3Abartowski1182%20OR%20author%3Anikwen%20OR%20author%3Ahipudding%20OR%20author%3AServeurpersoCom%20OR%20author%3Apwilkin%20OR%20author%3Areeselevine%20OR%20author%3Angxson%20OR%20author%3Ajeffbolznv%20OR%20author%3Amarty1885%20OR%20author%3A0cc4m%20OR%20author%3ATitaniumtown%20OR%20author%3Aangt%20OR%20author%3AIMbackK%20OR%20author%3Aarthw%20OR%20author%3AJohannesGaessler%20OR%20author%3AORippler%20OR%20author%3Aruixiang63%20OR%20author%3Axctan%20OR%20author%3Aallozaur%20OR%20author%3Ayomaytk%20OR%20author%3Aaendk%20OR%20author%3Agaugarg-nv%20OR%20author%3Ataronaeo%20OR%20author%3Aforforever73%20OR%20author%3Alhez%20OR%20author%3Anetrunnereve%20OR%20author%3Afairydreaming)%20sort%3Aupdated-desc) / [dev stats](https://github.com/ggml-org/llama.cpp-dev) / [lib llama API](https://github.com/ggml-org/llama.cpp/issues/9289) / [llama-server REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

</div>

## Quick start

A few options to get `llama.cpp` installed on your machine:

- Visit https://llama.app and follow the instructions
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed:

```sh
# Download and run a model directly from Hugging Face
llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF

# Launch OpenAI-compatible API server
llama serve -hf ggml-org/Qwen3.5-0.8B-GGUF
```

<table align="center">
    <tr>
        <td align="center" width=50%>
            <img width="1310" height="888" alt="VLM session with `llama cli`" src="https://github.com/user-attachments/assets/88726b48-1713-48aa-a525-95a02e78afc4" />
            <i>VLM session with <b>llama cli</b></i>
        </td>
        <td align="center">
            <img width="1392" height="958" alt="Built-in web UI against `llama serve` running Qwen 3.6" src="https://github.com/user-attachments/assets/b402f972-2e32-4def-8771-8d849f08cf2e" />
            <i>Built-in web UI against <b>llama serve</b></i>
        </td>
    </tr>
<table>

## Description

The main goal of `llama.cpp` is to enable LLM (and VLM) inference with minimal setup and state-of-the-art performance on
a wide range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity

The `llama.cpp` project is build on top of the [ggml](https://github.com/ggml-org/ggml) library.

## Supported backends

| Backend | Target devices |
| --- | --- |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Hexagon [In Progress]](docs/backend/snapdragon/README.md) | Snapdragon |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [WebGPU](docs/build.md#webgpu) | All |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |

## Documentation

#### Tools

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)
- [XCFramework](docs/xcframework.md)
- [Completions](docs/completions.md)
- [Models](docs/models.md)
- [Release process](docs/release.md)

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information

## Acknowledgements

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [nothings/stb](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [sheredom/subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
