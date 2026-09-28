#!/usr/bin/env bash
# build.sh — build ubersdr_ndb locally (no Docker)
#
# Requires: build-essential, cmake, libcurl4-openssl-dev,
#           libssl-dev, zlib1g-dev, pkg-config, and IXWebSocket (cloned
#           automatically). Also fetches the OurAirports navaid list.
#
# Usage:
#   ./build.sh [--clean|-c]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
IXWS_DIR="${SCRIPT_DIR}/IXWebSocket"

CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean|-c) CLEAN=1 ;;
    *) echo "Unknown option: $arg"; echo "Usage: $0 [--clean|-c]"; exit 1 ;;
  esac
done

echo "=== ubersdr_ndb Build ==="
echo "Source: ${SCRIPT_DIR}/src"
echo "Build:  ${BUILD_DIR}"
echo ""

if [[ "${CLEAN}" -eq 1 ]]; then
  echo "--- Cleaning build directory ---"
  rm -rf "${BUILD_DIR}"
  echo ""
fi

# Clone IXWebSocket if not present
if [[ ! -f "${IXWS_DIR}/ixwebsocket/IXWebSocket.h" ]]; then
  echo "--- Cloning IXWebSocket ---"
  git clone --depth 1 https://github.com/machinezone/IXWebSocket.git "${IXWS_DIR}"
  echo ""
fi

# Beacon database, next to the build (the binary looks beside itself first).
if [[ ! -f "${BUILD_DIR}/navaids.csv" ]]; then
  echo "--- Fetching navaids.csv (OurAirports) ---"
  mkdir -p "${BUILD_DIR}"
  curl -fsSL -o "${BUILD_DIR}/navaids.csv" https://davidmegginson.github.io/ourairports-data/navaids.csv \
    || echo "warning: could not fetch navaids.csv — beacons will not be named"
  echo ""
fi

mkdir -p "${BUILD_DIR}"

echo "--- Running cmake ---"
cmake -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DIXWS_ROOT="${IXWS_DIR}" \
      "${SCRIPT_DIR}"

echo ""
echo "--- Running make ---"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)" --target ubersdr_ndb

echo ""
echo "=== Build complete ==="
echo "Binary: ${BUILD_DIR}/ubersdr_ndb"
echo ""
echo "Usage example:"
echo "  ${BUILD_DIR}/ubersdr_ndb --url http://192.168.1.10:8080 --stream 356000:iq192 --web-static ${SCRIPT_DIR}/static"
