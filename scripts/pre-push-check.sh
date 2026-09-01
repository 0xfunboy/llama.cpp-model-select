#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ui_dir="${repo_root}/tools/ui"
cpu_build="${repo_root}/build-pre-push-cpu"
vulkan_build="${repo_root}/build-pre-push-vulkan"
jobs="${LLAMA_CI_JOBS:-$(nproc)}"
validated_head="$(git -C "${repo_root}" rev-parse HEAD)"
sqlite_prefix="${LLAMA_SQLITE_PREFIX:-${repo_root}/../../deps/sqlite3/usr}"
sqlite_library="${LLAMA_SQLITE_LIBRARY:-/usr/lib/x86_64-linux-gnu/libsqlite3.so.0}"
spirv_headers_dir="${LLAMA_SPIRV_HEADERS_DIR:-${repo_root}/../../deps/spirv-headers/root/usr/share/cmake/SPIRV-Headers}"
sqlite_args=()
vulkan_args=()

if [[ -f "${sqlite_prefix}/include/sqlite3.h" && -f "${sqlite_library}" ]]; then
    sqlite_args=(
        "-DSQLite3_INCLUDE_DIR=${sqlite_prefix}/include"
        "-DSQLite3_LIBRARY=${sqlite_library}"
    )
fi

if [[ -f "${spirv_headers_dir}/SPIRV-HeadersConfig.cmake" ]]; then
    vulkan_args=("-DSPIRV-Headers_DIR=${spirv_headers_dir}")
fi

run_ui() {
    local os_version

    cd "${ui_dir}"
    export CI=1
    if [[ "$(uname -s)" == "Linux" && -r /etc/os-release ]]; then
        os_version="$(. /etc/os-release; printf '%s' "${VERSION_ID:-}")"
        if [[ "${os_version}" == "26.04" && -z "${PLAYWRIGHT_HOST_PLATFORM_OVERRIDE:-}" ]]; then
            export PLAYWRIGHT_HOST_PLATFORM_OVERRIDE=ubuntu24.04-x64
        fi
    fi
    npm ci --prefer-offline --no-audit
    npm run check
    npm run lint
    npm run test:unit -- --run
    npm run build
    npx playwright install chromium
    npm run test:client -- --run
    npm run build-storybook
    npm run test:ui -- --run --testTimeout=60000
    npm run test:e2e
}

run_cpu() {
    cmake -S "${repo_root}" -B "${cpu_build}" -G Ninja \
        "${sqlite_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_NATIVE=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_BUILD_SERVER=ON \
        -DLLAMA_BUILD_TESTS=ON
    cmake --build "${cpu_build}" --target \
        llama-server \
        test-arg-parser \
        test-caliber-scoring \
        test-caliber-plan \
        test-model-registry \
        test-server-persistence \
        test-model-operations \
        test-streaming-profiler \
        test-quality-evidence \
        test-ds4-output-guard \
        test-least-cost-router \
        test-llama-archs \
        -j "${jobs}"
    ctest --test-dir "${cpu_build}" --output-on-failure \
        -R '^(test-arg-parser|caliber-scoring|caliber-plan|model-registry|server-persistence|model-operations|streaming-profiler|quality-evidence|ds4-output-guard|least-cost-router)$'
    "${cpu_build}/bin/test-llama-archs" -a qwen4exp -v
}

run_vulkan() {
    cmake -S "${repo_root}" -B "${vulkan_build}" -G Ninja \
        "${sqlite_args[@]}" \
        "${vulkan_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_NATIVE=OFF \
        -DGGML_VULKAN=ON \
        -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_BUILD_SERVER=ON \
        -DLLAMA_BUILD_TESTS=ON
    cmake --build "${vulkan_build}" --target llama-server test-llama-archs -j "${jobs}"
    "${vulkan_build}/bin/llama-server" --version
    "${vulkan_build}/bin/test-llama-archs" -a qwen4exp -v
}

run_all_gates() {
    git -C "${repo_root}" diff --check HEAD
    run_ui
    run_cpu
    run_vulkan
    if ! git -C "${repo_root}" diff --quiet || ! git -C "${repo_root}" diff --cached --quiet; then
        echo "pre-push check: a gate changed tracked files in the clean worktree" >&2
        return 1
    fi
}

run_clean_worktree() {
    local clean_parent
    local clean_root
    local status=0

    clean_parent="$(mktemp -d "${TMPDIR:-/tmp}/llama-pre-push.XXXXXX")"
    clean_root="${clean_parent}/tree"

    cleanup_clean_worktree() {
        git -C "${repo_root}" worktree remove --force "${clean_root}" >/dev/null 2>&1 || true
        rmdir "${clean_parent}" >/dev/null 2>&1 || true
    }
    git -C "${repo_root}" worktree add --detach "${clean_root}" "${validated_head}" || status=$?
    if (( status == 0 )); then
        LLAMA_CI_JOBS="${jobs}" \
            LLAMA_SQLITE_PREFIX="${sqlite_prefix}" \
            LLAMA_SQLITE_LIBRARY="${sqlite_library}" \
            LLAMA_SPIRV_HEADERS_DIR="${spirv_headers_dir}" \
            "${clean_root}/scripts/pre-push-check.sh" _all_clean || status=$?
    fi
    cleanup_clean_worktree
    return "${status}"
}

case "${1:-all}" in
    all)
        if ! git -C "${repo_root}" diff --quiet || ! git -C "${repo_root}" diff --cached --quiet; then
            echo "pre-push check: tracked files differ from HEAD; commit or stash them first" >&2
            exit 1
        fi
        run_clean_worktree
        if [[ "$(git -C "${repo_root}" rev-parse HEAD)" != "${validated_head}" ]] ||
           ! git -C "${repo_root}" diff --quiet ||
           ! git -C "${repo_root}" diff --cached --quiet; then
            echo "pre-push check: HEAD or tracked files changed during validation" >&2
            exit 1
        fi
        printf '%s\n' "${validated_head}" > "$(git -C "${repo_root}" rev-parse --git-path llama-ci-clean)"
        ;;
    _all_clean)
        run_all_gates
        ;;
    ui)
        run_ui
        ;;
    cpu)
        run_cpu
        ;;
    vulkan)
        run_vulkan
        ;;
    *)
        echo "usage: $0 [all|ui|cpu|vulkan]" >&2
        exit 2
        ;;
esac
