#!/usr/bin/env bash
# fetch_rww.sh — download RXX's active NDB list (RWW signals, CSV) to a file.
#
#   tools/fetch_rww.sh out.csv
#
# RXX (https://rxx.classaxe.com) is the listeners' beacon database, and has
# beacons OurAirports lacks: NQY on 347 kHz, OSA on 351 and OL on 339 among
# them. Run when the image is built (Dockerfile) and by ./build.sh — never
# by the addon itself, so RXX sees one request per release, not one per user.
#
# The CSV is the signals search form's export. The form carries a CSRF token
# bound to a session, so fetch the form first, with a cookie jar, and post
# its token back.

set -euo pipefail

out="${1:?usage: $0 out.csv}"
url="https://rxx.classaxe.com/en/rww/signals"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

curl -fsSL -m 60 -c "$tmp/jar" -o "$tmp/form.html" "$url"
token="$(grep -o 'name="form\[_token\]" value="[^"]*' "$tmp/form.html" | sed 's/.*value="//' | head -1)"
[[ -n "$token" ]] || { echo "fetch_rww: no form token at $url" >&2; exit 1; }

# Active NDBs (status 1), every region, sorted by frequency. The export
# ignores the page limit.
curl -fsSL -m 300 -b "$tmp/jar" -o "$tmp/rww.csv" "$url" \
    --data-urlencode "form[_token]=$token" \
    --data 'form[show]=csv&form[filename]=rww_signals.csv&form[sort]=khz&form[order]=a' \
    --data 'form[limit]=50&form[page]=0&form[type][]=NDB&form[status][]=1&form[sp_itu_clause]=AND' \
    --data 'form[listener_invert]=0&form_sortby=khz|a&form[submit]='

# Fail rather than ship an image without it: the header must be the export's,
# and there are ~4500 active NDBs, so far fewer means a broken answer.
head -1 "$tmp/rww.csv" | grep -q '^"KHz","ID","Type",.*"Lat","Lon"' \
    || { echo "fetch_rww: $url did not return the CSV" >&2; exit 1; }
rows="$(grep -c '^[0-9.]*,"[^"]*","NDB",' "$tmp/rww.csv" || true)"
(( rows >= 1000 )) || { echo "fetch_rww: only $rows NDBs from $url" >&2; exit 1; }
mv "$tmp/rww.csv" "$out"
echo "fetch_rww: $rows NDBs -> $out"
