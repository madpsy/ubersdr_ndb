#!/bin/sh
# entrypoint.sh — translate environment variables into ubersdr_ndb flags
#
# Environment variables:
#   UBERSDR_URL        UberSDR base URL (default: http://ubersdr:8080)
#   PASS               UberSDR bypass password (wide IQ modes usually need one)
#   NDB_STREAMS        IQ streams, comma-separated "centreHz:mode" (default: 356000:iq192)
#                      mode is iq48 | iq96 | iq192 | iq384 (±24 / ±48 / ±96 / ±192 kHz)
#   NDB_PINNED         Frequencies in Hz always decoded, comma-separated (optional)
#   NDB_SNR            Carrier detection threshold in dB above the floor (default: 10)
#   NDB_MAX_CHANNELS   Cap on simultaneous beacons per stream (default: 48)
#   NDB_ASSIST_KM      Published beacons within this range get a lower detection
#                      threshold (default: 1500, 0 = off)
#   NDB_RADIUS_KM      Decodes are only matched to published beacons within this
#                      many km, and the map shows unheard ones out to it
#                      (default: 2000, 0 = no limit)
#   NDB_SHOW_UNLISTED  1 = also decode carriers where no beacon is published, and show
#                      idents that match none (default: the known list only)
#   NDB_GGMORSE        ggmorse second decoder: auto (default) | all (~10x the CPU) | off
#   NDB_GGMORSE_SLOTS  auto: ggmorse instances at once, per stream (default: 6)
#   RECEIVER_LAT/LON   Override the receiver position from /api/description (optional)
#   MIN_MARGIN         Reduced-depth IQ margin in dB (default: 26, 0 = lossless, else 15-60)
#   WEB_PORT           Web UI port (default: 6100)
#   DATA_DIR           Heard log directory (default: /data)
#   WEB_STATIC         Web UI files (default: /usr/local/share/ubersdr_ndb/static)
#   NAVAIDS            OurAirports navaids.csv (default: /usr/local/share/ubersdr_ndb/navaids.csv)
#   NDB_LOG_SUMMARY    Log the full beacon table every N seconds (default: off)

set -e

URL="${UBERSDR_URL:-http://ubersdr:8080}"
STREAMS="${NDB_STREAMS:-356000:iq192}"
PORT="${WEB_PORT:-6100}"
DATA="${DATA_DIR:-/data}"
STATIC="${WEB_STATIC:-/usr/local/share/ubersdr_ndb/static}"
NAV="${NAVAIDS:-/usr/local/share/ubersdr_ndb/navaids.csv}"

set -- --url "$URL" --stream "$STREAMS" --web-port "$PORT" --web-static "$STATIC" \
       --navaids "$NAV" --data-dir "$DATA" "$@"
[ -n "$PASS" ]             && set -- "$@" --pass "$PASS"
[ -n "$NDB_PINNED" ]       && set -- "$@" --ndb "$NDB_PINNED"
[ -n "$NDB_SNR" ]          && set -- "$@" --snr "$NDB_SNR"
[ -n "$NDB_MAX_CHANNELS" ] && set -- "$@" --max-channels "$NDB_MAX_CHANNELS"
[ -n "$NDB_ASSIST_KM" ]    && set -- "$@" --assist-km "$NDB_ASSIST_KM"
[ -n "$NDB_RADIUS_KM" ]    && set -- "$@" --radius-km "$NDB_RADIUS_KM"
[ -n "$NDB_GGMORSE" ]      && set -- "$@" --ggmorse "$NDB_GGMORSE"
case "$NDB_SHOW_UNLISTED" in 1|true|yes) set -- "$@" --show-unlisted ;; esac
[ -n "$NDB_GGMORSE_SLOTS" ] && set -- "$@" --ggmorse-slots "$NDB_GGMORSE_SLOTS"
[ -n "$RECEIVER_LAT" ]     && set -- "$@" --lat "$RECEIVER_LAT"
[ -n "$RECEIVER_LON" ]     && set -- "$@" --lon "$RECEIVER_LON"
[ -n "$MIN_MARGIN" ]       && set -- "$@" --min-margin "$MIN_MARGIN"
[ -n "$NDB_LOG_SUMMARY" ]  && set -- "$@" --summary-every "$NDB_LOG_SUMMARY"

exec /usr/local/bin/ubersdr_ndb "$@"
