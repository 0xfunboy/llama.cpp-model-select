# Fork CI and safe pushes

This fork has a smaller automatic test matrix than upstream llama.cpp. A commit is CI clean when every job in `Fork CI` succeeds for the exact commit that will reach `master`.

`Fork CI` runs these gates in one workflow:

- UI type checks, lint, unit, client, Storybook accessibility, end-to-end tests and production build;
- CPU server build, the fork-specific server tests and a Qwen4Exp architecture smoke test;
- Vulkan server build, loader smoke test and the same Qwen4Exp architecture regression;
- CUDA `sm_86` server compile without requiring a GPU.

The automatic workflow installs SQLite development files explicitly because the fork's server archive depends on SQLite. It does not run the upstream cache cleanup step, which requires permissions that normal forks do not have.

Upstream platform matrices remain preserved behind `workflow_dispatch` and do not run on every branch push. Some need upstream-only runners, labels or secrets and will not execute successfully in this fork until those resources are provisioned. The CANN workflow is an explicit no-op while its dedicated runners are unavailable.

## Local gate

Run the same local gates before publishing a branch:

```sh
scripts/pre-push-check.sh all
```

Individual gates are available while iterating:

```sh
scripts/pre-push-check.sh ui
scripts/pre-push-check.sh cpu
scripts/pre-push-check.sh vulkan
```

The local machine cannot reproduce the hosted CUDA container unless it has the CUDA toolkit. The CUDA compile job in `Fork CI` remains the source of truth for that backend.

Enable the versioned pre-push hook once per checkout:

```sh
git config core.hooksPath .githooks
```

The full gate checks out the exact `HEAD` commit in a temporary clean worktree, so local untracked artifacts cannot leak into its build or tests. It records the tested commit under `.git`. The hook rejects a push when tracked files differ from `HEAD`, the outgoing range has whitespace errors or the outgoing commit does not match that record. It does not keep a remote connection open while a long test suite runs. Git supports `--no-verify`, but bypassing the hook also bypasses the local CI evidence.

## Branch workflow

1. Create a branch from the current green `master`.
2. Commit one coherent change at a time.
3. Run an individual gate while developing.
4. Run `scripts/pre-push-check.sh all` on the final commit.
5. Push the branch. `Fork CI` runs automatically on that branch.
6. Move the commit to `master` only after every job is green.

A branch or pull request inside `0xfunboy/llama.cpp-model-select` does not notify or submit code to `ggml-org/llama.cpp`.

## Notifications

GitHub sends workflow notifications per workflow, not per push. This fork therefore uses one automatic workflow instead of the upstream collection of independent workflows. Notification preferences can still be changed in GitHub account settings, but disabling mail does not make a failing commit CI clean.
