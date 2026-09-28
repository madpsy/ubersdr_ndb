#!/usr/bin/env bash
# start.sh — start the ubersdr_ndb service
#
# Usage:
#   ./start.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/ndb"

cd "${INSTALL_DIR}"
echo "Starting ubersdr_ndb..."
docker compose up -d --remove-orphans
echo "Done."
echo "  View logs : docker compose logs -f"
