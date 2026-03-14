#!/usr/bin/env bash
set -euo pipefail

# One-command build helper for this CMake project.
# - Ensures build directory exists
# - Re-runs CMake configure to refresh file(GLOB ...) source lists
# - Performs parallel build

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
JOBS="${JOBS:-$(nproc)}"

usage() {
  cat <<'EOF'
Usage:
  ./build.sh [--clean] [--release]

Options:
  --clean    Remove build directory before configuring
  --release  Build with Release type (default: Debug)

Environment variables:
  JOBS        Number of parallel jobs (default: nproc)
  BUILD_TYPE  CMake build type (overridden by --release)
EOF
}

CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN=1
      shift
      ;;
    --release)
      BUILD_TYPE="Release"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "${CLEAN}" -eq 1 ]]; then
  echo "[build] cleaning ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

echo "[build] configuring (type=${BUILD_TYPE})"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "[build] building with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

echo "[build] done"
