#!/usr/bin/env bash
# restart.sh — restart the ubersdr_ndb service
#
# Usage:
#   ./restart.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/ndb"

cd "${INSTALL_DIR}"
echo "Stopping ubersdr_ndb..."
docker compose down
echo "Starting ubersdr_ndb..."
docker compose up -d --remove-orphans
echo "Done."
echo "  View logs : docker compose logs -f"
