#!/usr/bin/env bash
# stop.sh — stop the ubersdr_ndb service
#
# Usage:
#   ./stop.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/ndb"

cd "${INSTALL_DIR}"
echo "Stopping ubersdr_ndb..."
docker compose down
echo "Done."
