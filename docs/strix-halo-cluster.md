# Strix Halo Cluster Design

## Current topology

The first milestone uses one host and two cooperating binaries:

- The `llama.cpp-strix-halo` server is the control plane. It owns the Web UI,
  model selector, lifecycle API, Fit Advisor, downloads, and benchmarks.
- The optimized Strix Halo Vulkan `llama-server` is the inference worker. The
  control plane starts it through `LLAMA_SERVER_WORKER`.
- `/home/funboy/models` is the only managed model root.
- One model worker is resident at a time. This leaves model switching explicit
  and makes unload reclaim the whole child process.

This layout keeps the custom UI independent from the optimized runtime and makes
runtime upgrades reversible.

## Thunderbolt network

Linux exposes host-to-host Thunderbolt or USB4 networking through the
`thunderbolt-net` driver. The link appears as a normal network interface, such as
`thunderbolt0`, so the cluster should use ordinary IP networking first. A simple
two-host address plan is `10.55.0.1/30` and `10.55.0.2/30` on a dedicated link.

Do not automatically authorize every Thunderbolt device with a permissive udev
rule. Keep IOMMU DMA protection enabled and authorize known devices only.

## Two different multi-node modes

### Independent workers

Each Strix Halo host runs its own Vulkan `llama-server` and keeps a complete model
in its local UMA pool. The control plane routes whole requests to a healthy node.

Use this mode for:

- higher concurrent throughput;
- model replicas and failover;
- running different models on different hosts;
- the 27B dense model, which already fits comfortably on one node.

This should be the default cluster mode because token generation has no
cross-host dependency for an individual request.

### Distributed model with ggml RPC

The llama.cpp RPC backend can expose a remote Vulkan device to one coordinator.
Both coordinator and workers must be built with `GGML_RPC=ON`, and the worker also
needs `GGML_VULKAN=ON`. The coordinator receives one or more `--rpc host:port`
arguments and can distribute model weights and KV cache across local and remote
devices.

Use this mode only when one model cannot fit in a single 120 GiB UMA pool, or when
a benchmark demonstrates a useful speedup. The upstream RPC backend is still
documented as proof-of-concept, fragile, and insecure. It has no authentication
or encryption, so bind it only to the dedicated Thunderbolt IP and never expose
it to an untrusted network.

RPC protocol and backend compatibility require the same tested llama.cpp build on
all nodes. A runtime upgrade is accepted only after load, generation, unload, and
failure-recovery tests pass on the complete cluster.

## Control-plane node registry

The future UI should maintain a registry with these fields per node:

- stable node ID and display name;
- Thunderbolt IP and API endpoint;
- llama.cpp build and RPC protocol versions;
- backend, device name, UMA capacity, and currently available memory;
- health, active model, load state, and measured prompt/generation throughput;
- model files available locally, identified by size and checksum.

The scheduler can then choose one of three explicit policies:

1. `single`: serve on one selected node;
2. `replica`: place complete requests on independent nodes;
3. `rpc-split`: start one distributed model through a coordinator.

Model transfer is never implicit. The UI must show source, destination, size,
checksum, free space, and resume behavior before the operator starts a copy.

## Qualification gates

Before enabling a second host in production:

1. Measure Thunderbolt throughput and latency with `iperf3` and `ping`.
2. Record a local Vulkan baseline for prompt processing and generation.
3. Repeat the same model, context, quantization, and prompt through RPC.
4. Test coordinator and worker loss; a failed RPC job must not wedge the control
   plane or leave stale model state.
5. Verify that stopping a worker reclaims UMA and that reconnecting never starts
   a model download automatically.

The cluster feature is complete only when its benchmark and recovery behavior are
visible in the same Web UI as the existing DS4 reports.

## Primary references

- [llama.cpp RPC backend](https://github.com/ggml-org/llama.cpp/blob/master/tools/rpc/README.md)
- [llama.cpp multi-GPU guide](https://github.com/ggml-org/llama.cpp/blob/master/docs/multi-gpu.md)
- [Linux USB4 and Thunderbolt guide](https://docs.kernel.org/admin-guide/thunderbolt.html)
