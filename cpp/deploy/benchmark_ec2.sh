#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/logs"
CONFIG_PATH="${1:-${ROOT_DIR}/config.json}"
DURATION_SECONDS="${2:-75}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_FILE="${LOG_DIR}/benchmark-${STAMP}.log"

if [[ ! -f "${CONFIG_PATH}" ]]; then
    echo "[bench] config not found: ${CONFIG_PATH}" >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "[bench] building (${BUILD_TYPE})"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
fi

if grep -Eq '"metrics_enabled"[[:space:]]*:[[:space:]]*false' "${CONFIG_PATH}"; then
    echo "[bench] warning: metrics_enabled=false in ${CONFIG_PATH}" >&2
    echo "[bench] warning: benchmarks will not print latency summaries" >&2
fi

if ulimit -l unlimited 2>/dev/null; then
    echo "[bench] memlock raised to unlimited"
else
    echo "[bench] memlock unchanged"
fi

echo "[bench] binary=${BUILD_DIR}/arb_detector"
echo "[bench] config=${CONFIG_PATH}"
echo "[bench] duration=${DURATION_SECONDS}s"
echo "[bench] output=${OUT_FILE}"

set +e
timeout "${DURATION_SECONDS}" \
    stdbuf -oL -eL "${BUILD_DIR}/arb_detector" --config "${CONFIG_PATH}" \
    2>&1 | tee "${OUT_FILE}"
status=${PIPESTATUS[0]}
set -e

if [[ ${status} -ne 0 && ${status} -ne 124 ]]; then
    echo "[bench] detector exited with status ${status}" >&2
    exit "${status}"
fi

echo "[bench] done"
echo "[bench] log saved to ${OUT_FILE}"
