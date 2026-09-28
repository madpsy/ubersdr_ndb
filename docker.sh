#!/usr/bin/env bash
# docker.sh — build the ubersdr_ndb Docker image
#
# The ubersdr_ndb binary is built from source inside
# the Docker image.  No host binaries are required.
#
# Usage:
#   ./docker.sh [build|push|run|arm64]
#
#   build  — build the image for linux/amd64 (default, loaded into local daemon)
#   arm64  — build the image for linux/arm64 (Raspberry Pi, Apple Silicon, etc.)
#   push   — build multi-arch (amd64 + arm64) with buildx and push manifest to registry
#   run    — run the image (set env vars below)
#
# Environment variables (build):
#   IMAGE      Docker image name/tag   (default: madpsy/ubersdr_ndb:latest)
#   PLATFORM   Docker --platform flag  (default: linux/amd64)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="${IMAGE:-madpsy/ubersdr_ndb:latest}"
PLATFORM="${PLATFORM:-linux/amd64}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

die() { echo "error: $*" >&2; exit 1; }

check_deps() {
    command -v docker >/dev/null || die "docker not found in PATH"
}

# Ensure a buildx builder capable of multi-platform builds exists and is active.
# Uses the existing "multiarch" builder if present, otherwise creates it.
ensure_buildx_builder() {
    local builder_name="multiarch"

    if docker buildx inspect "$builder_name" &>/dev/null; then
        echo "Using existing buildx builder: $builder_name"
    else
        echo "Creating buildx builder: $builder_name"
        docker buildx create \
            --name "$builder_name" \
            --driver docker-container \
            --bootstrap
    fi

    docker buildx use "$builder_name"
}

# Stage the build context into a temp directory (excludes build artefacts / git).
stage_context() {
    TMPCTX="$(mktemp -d)"
    trap 'rm -rf "$TMPCTX"' EXIT

    echo "Staging build context in $TMPCTX..."
    rsync -a --exclude='/build' \
              --exclude='/build-*' \
              --exclude='/IXWebSocket' \
              --exclude='/navaids.csv' \
              --exclude='.git' \
              "$SCRIPT_DIR/" "$TMPCTX/"
}

build() {
    check_deps
    stage_context

    echo "Building image $IMAGE (platform=$PLATFORM)..."
    # Single-arch build loaded into the local Docker daemon (no push).
    docker buildx build \
        --platform "$PLATFORM" \
        --tag "$IMAGE" \
        --load \
        "$TMPCTX"

    echo "Built: $IMAGE"
}

push() {
    check_deps
    stage_context
    ensure_buildx_builder

    local multi_platform="linux/amd64,linux/arm64"

    echo "Building multi-arch image $IMAGE (platforms=$multi_platform) and pushing..."
    docker buildx build \
        --platform "$multi_platform" \
        --tag "$IMAGE" \
        --push \
        "$TMPCTX"

    echo "Pushed multi-arch manifest: $IMAGE"

    # Push whatever is already committed — but never commit on the user's
    # behalf. This previously ran "git add -A" and committed everything with
    # a generic "Release" message, which silently swallowed real commit
    # messages and would sweep any unrelated work in progress (or a stray
    # credentials file) into a public push with no chance to review it.
    if [[ -n "$(git status --porcelain)" ]]; then
        echo
        echo "WARNING: uncommitted changes — the image was built from them," >&2
        echo "         but they are NOT being committed or pushed:" >&2
        git status --short >&2
        echo >&2
        echo "         Commit them yourself, then run: git push" >&2
        exit 1
    fi

    echo "Pushing git repository..."
    git push
}

run_image() {
    args=()
    [[ -n "${UBERSDR_URL:-}"  ]] && args+=(-e "UBERSDR_URL=$UBERSDR_URL")
    [[ -n "${NDB_STREAMS:-}"  ]] && args+=(-e "NDB_STREAMS=$NDB_STREAMS")
    [[ -n "${PASS:-}"         ]] && args+=(-e "PASS=$PASS")
    [[ -n "${WEB_PORT:-}"     ]] && args+=(-e "WEB_PORT=$WEB_PORT")

    PORT="${WEB_PORT:-6100}"

    docker run --rm -it \
        --platform "$PLATFORM" \
        -p "${PORT}:${PORT}" \
        "${args[@]}" \
        "$IMAGE" \
        "$@"
}

# ---------------------------------------------------------------------------
# Environment variable reference (for docker run -e ...)
# ---------------------------------------------------------------------------
#
#   UBERSDR_URL   UberSDR base URL (default: http://ubersdr:8080)
#   NDB_STREAMS   IQ streams, "centreHz:mode,..." (default: 356000:iq192)
#   PASS          UberSDR bypass password (wide IQ modes usually need one)
#   WEB_PORT      Web UI port (default: 6100)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

case "${1:-build}" in
    build) build ;;
    arm64) PLATFORM=linux/arm64 build ;;
    push)  push  ;;
    run)   shift; run_image "$@" ;;
    *)
        echo "Usage: $0 [build|arm64|push|run [ubersdr_ndb-args...]]" >&2
        exit 1
        ;;
esac
