# ubersdr_ndb

**Multi-beacon NDB decoder addon for [UberSDR](https://github.com/madpsy/ka9q_ubersdr)**

Requests one or more wideband IQ streams from UberSDR, finds every non-directional beacon (NDB) carrier in them, and decodes each beacon's Morse ident **in parallel**. Decoded idents are matched against the [OurAirports](https://ourairports.com/data/) navaid database and located relative to the receiver. A web UI shows the spectrum, a live beacon table, the copy as it is decoded, a persistent heard log, a map, and reception stats over time for propagation.

The default, one `iq192` stream centred on 356 kHz, covers roughly 270–442 kHz and costs a single UberSDR session, however many beacons are in it. It's chosen from the navaid list: the UK and Ireland's 110 NDBs sit on 77 frequencies from 277 to 545 kHz, all but one of them at 277–434 kHz. This window takes in every one except Lichfield (545 kHz); add `545000:iq48` for that. For another region, see [Streams](#streams).

---

## How it works

```
UberSDR /ws  mode=iq96 (96 kHz IQ, protocol v4, reduced-depth)       × each stream
        │
        ▼
CarrierDetector  ─ 32k-point FFT (2.9 Hz bins), averaged
        │          noise floor = 25th percentile per 1 kHz, interpolated
        │          narrow peaks ≥ 10 dB over floor (7 dB near a published NDB)
        │          drops tone sidebands and weak peaks beside strong carriers
        ▼
NdbDecoder  ─ carrier seen on 2 passes → channel; drifting carrier → retune;
        │     gone for 2 min → dropped; identified → channel freed, carrier
        │     tracked, ident re-copied on a revisit every 15 min
        ▼
NdbChannel (one per beacon)
   rotate carrier to DC → FIR ↓ to 16 kHz → FIR ↓ to 4 kHz (±1.3 kHz)
   → AM envelope ÷ carrier level → 4th-order 250 Hz high-pass
   ├→ KeyingDecoder (NDB-specific: the copy shown)   ─┐
   │    └→ FoldDecoder (the ident averaged over       ─┤
   │         a minute of repeats: weak beacons)        ├→ ident tally:
   └→ ggmorse (general CW decoder: second opinion,   ─┘
        on a small pool of channels at a time)
        most frequent clean 2–4 char token over the last hour
        │
        ▼
main.cpp ─ navaid match, heard log (/data/heard.tsv), reception history
           (/data/history.tsv), HTTP + WebSocket
        │
        ▼
Browser: static/index.html + app.js  (spectrum · beacons · map · live copy · heard log)
```

Everything is decided server-side. A browser that connects is sent the full current state straight away (status, spectrum, heard log, and the last few minutes of copy), then live updates: decoded characters as they arrive, status at 1 Hz, spectrum every 2 s.

### Three Morse decoders

Most NDBs are A2A: a continuous carrier with the ident keyed as a 400 or 1020 Hz tone. An envelope detector turns that into a keyed audio tone, and three decoders read it.

**KeyingDecoder** (`src/keying_decoder.*`) is written for how NDBs key: a machine sending the same letters at a fixed speed and level, with a long silence between repetitions. It learns the things that don't change, over a span long enough to include both keying and silence:

- the tone: of the three strongest lines in a long averaged spectrum, the one that is keyed (a steady beat against a neighbouring carrier can be stronger than the tone itself);
- the on/off threshold, from the last 20 s of tone envelope: on at 50% of the way from floor to peak, off at 40%;
- the dit length, from the last 60 marks.

Classification is then textbook. State changes are debounced by ~30% of a dit, so a fade inside a dah doesn't split it. Letters are only emitted while the tone's on/off contrast is at least 15 dB and the timing looks like machine keying (a dit of 50–250 ms, with most marks at exactly one or three dits), so noise produces nothing. Its copy is the one shown and streamed live.

The tone envelope is integrated coherently, and how long for sets the noise bandwidth: 20 ms (~50 Hz) is what a 24 wpm beacon needs, but a 10 wpm one, with a 120 ms dit, can take 60 ms and 3× less noise. So the decoder runs four lanes at once, integrating over 20, 40, 60 and 80 ms, each learning its own threshold and dit. The copy comes from the widest lane whose window is at most 0.6 of its dit, and moves between lanes only between words. Noise's own contrast is the same in every lane (~12 dB), so the one 15 dB gate serves them all.

**FoldDecoder** (`src/fold_decoder.*`) is for beacons too weak to copy one ident at a time. A beacon sends the same ident on a fixed cycle, so each minute of tone envelope is cut at its cycle (found by autocorrelation) and the repeats averaged: 6–15 of them, cutting the noise 2.5–4×. The average is then read like a single ident. A copy is only given out when the cycle is clear and every mark and space in the average fits one or three units of a single speed; noise, folded, produces nothing. Each minute is a separate copy, from audio no other copy saw, and counts in the tally like any other. The UI shows its copies, in italics, when the keying decoder has none.

Decoding is blind: neither decoder is told what to expect. The navaid database is used afterwards, to name and locate what was copied. An ident that matches nothing on its frequency needs 5 copies, rather than 2, before it enters the heard log.

**[ggmorse](https://github.com/ggerganov/ggmorse)** is the library UberSDR's own CW decoder (`cw-decoder`) is built on, vendored from `ka9q_ubersdr/audio_extensions/morse/external/ggmorse` with changes for this use; see [`third_party/ggmorse/README.ubersdr_ndb.md`](third_party/ggmorse/README.ubersdr_ndb.md). It feeds the ident tally as a second opinion. On its own it struggles with NDBs because it re-estimates threshold and speed over a rolling 3 s window, which an NDB's inter-ident silence dominates.

ggmorse costs ~10× the keying decoder, and on strong beacons adds nothing, so it isn't run everywhere. A pool of instances (`NDB_GGMORSE_SLOTS`, default 6 per stream) goes to the channels where a second opinion helps: not yet identified, but showing signs of keying: a clear ident cycle, or tone contrast ≥ 13.5 dB (above noise, but under the keying decoder's 15 dB gate). A channel gives its instance back once identified, after 5 minutes without success (then rests 15 minutes so others get a turn), or if the keying evidence fades. Each instance is also handed what the keying decoder has learned: its audio is band-passed ±75 Hz around the known tone, and its pitch and (when known) speed are fixed, so it only has to decide the threshold. The UI marks those channels `2nd`.

On the test capture, the pool identifies the same beacons as running ggmorse on every channel, for 8.5× less CPU.

On a capture at the same site (M9PSY, Dalgety Bay), a 20 ms envelope with its on-level at 60% copied nothing from CBL (380 kHz, 161 km), though it is easy to read by ear: a 10 wpm beacon at ~11 dB of contrast. With the lanes and the lower on-level it copies 23 of 30 repeats in 4 minutes. In 6 minutes of the whole band, the three decoders together identify EDN, PIK, UW, DND, CBL, ATF and GLW, and HB (420 kHz, not in OurAirports; likely Belfast City). Before these changes they identified only the first four.

### Revisits

A beacon sends nothing but its ident, so once that is copied there is nothing more to decode. An identified beacon gives its channel up straight away, and its carrier is tracked by the detector, which scans the whole stream anyway, at no cost. Every `NDB_REVISIT_MIN` (15) minutes it gets a revisit: a channel reopens on it until the ident is copied `NDB_IDENT_COPIES` (2) more times, usually within a minute or two. A due revisit takes the next free slot ahead of waiting carriers. Once 5 minutes overdue, it takes the slot of the unidentified channel that has held one longest.

A revisit that copies nothing in `NDB_VISIT_TIMEOUT_S` (180 s) is a miss, and is tried again 5 minutes later. After 3 misses running, 2 hours without a copy, or 30 minutes without the carrier, the beacon is dropped. If a revisit copies a different ident, the new beacon replaces the old one.

Between revisits, a beacon counts as heard while its carrier is there and its last copy is less than 23 minutes old (revisit + grace + timeout). The table shows it as tracked, with its live SNR.

On the test capture, 368.0 **UW** (a 56 dB beacon with 1.5 s of keying then 6 s of silence) never copied in ggmorse, but copies as `UW UW UW …` in KeyingDecoder. EDN, PIK, ATF and DND copy complete every time, where ggmorse often dropped the last letter.

### Naming beacons

The OurAirports `navaids.csv` is downloaded when the image is built (and by `./build.sh`). For each carrier the UI shows:

| mark | meaning |
|---|---|
| **EDN ✓** | decoded ident equals the published beacon on that frequency |
| **CBL ≈** | decoded `CB`, which is the published ident minus its last letter (ggmorse often drops the final letter of a slow ident) |
| *IVR Inverness* | nothing decoded yet: the nearest published beacon on that frequency, as a guess |

The receiver position comes from UberSDR's `/api/description`. It drives distances, bearings, the map, and the lower detection threshold near published beacons.

---

## Quick install (Docker)

```bash
curl -fsSL https://raw.githubusercontent.com/madpsy/ubersdr_ndb/main/install.sh | bash
```

This creates `~/ubersdr/ndb/`, downloads `docker-compose.yml` and the helper scripts, and starts the container. The UI is at **http://localhost:6100/**.

### UberSDR addon proxy configuration

| Field | Value |
|-------|-------|
| Name | `ndb` |
| Host | `ndb` |
| Port | `6100` |
| Enabled | ✅ |
| Strip prefix | ✅ |
| Rewrite WebSocket origin | ❌ |
| Rate limit | `100` |

The UI is then at `/addon/ndb/`. It uses only relative URLs, so it needs no prefix injection.

---

## Configuration

Edit `~/ubersdr/ndb/docker-compose.yml`, then `./restart.sh`.

| Variable | Default | |
|---|---|---|
| `UBERSDR_URL` | `http://ubersdr:8080` | UberSDR base URL |
| `NDB_STREAMS` | `356000:iq192` | IQ streams, comma-separated `centreHz:mode`. One session each. Wide modes may need `PASS`. |
| `PASS` | | UberSDR bypass password. Wide IQ modes usually need one. |
| `NDB_PINNED` | | Hz, comma-separated: always decode these, even if not detected |
| `NDB_SNR` | `10` | Carrier detection threshold, dB above the noise floor |
| `NDB_ASSIST_KM` | `1500` | Published NDBs this close get a 7 dB threshold (0 = off) |
| `NDB_MAX_CHANNELS` | `8` | Most carriers decoded at once, per stream. Identified beacons don't keep one (see [Revisits](#revisits)), so slots go to unidentified carriers and revisits. When all are taken, waiting carriers are admitted strongest first, and a channel gives up its slot if it never showed keying in a 3-minute trial, or last keyed over 30 minutes ago. Pinned channels keep theirs. So every carrier gets a turn, and spurs can't crowd out beacons. |
| `NDB_REVISIT_MIN` | `15` | Minutes between revisits of an identified beacon |
| `NDB_VISIT_TIMEOUT_S` | `180` | A revisit that copies nothing in this long is a miss |
| `NDB_IDENT_COPIES` | `2` | Copies of a published beacon's ident that identify it, and that reconfirm it on a revisit. Counted over the last hour, not necessarily in a row. A first identification always needs at least 2; an ident matching no published beacon, or only all but its last letter, needs 5. |
| `NDB_RADIUS_KM` | `2000` | Radius around the receiver. With `NDB_SHOW_UNLISTED=0`, only carriers near a beacon published inside it get a decoder. Decodes are only matched to beacons inside it, since a shared ident far away is far more likely a misread. The map shows unheard beacons out to it. `0` = no limit. (In Europe, published NDBs are dense enough that most whole-kHz frequencies have one within 2000 km, so a smaller radius filters candidates harder.) |
| `NDB_SHOW_UNLISTED` | `0` | `0` means the known list only. A carrier gets a decoder only if a beacon within `NDB_RADIUS_KM` is published within ±300 Hz of it, and only idents matching one are shown. `1` decodes every carrier (spurs included, at a CPU cost) and also shows idents matching nothing published ("not in database"). Pinned frequencies are always decoded. |
| `NDB_GGMORSE` | `auto` | ggmorse second decoder: `auto` (a pool for unidentified channels showing keying), `all` (every channel, ~10× the CPU), `off` |
| `NDB_GGMORSE_SLOTS` | `6` | `auto`: ggmorse instances at once, per stream |
| `RECEIVER_LAT` / `RECEIVER_LON` | from UberSDR | Override the receiver position |
| `MIN_MARGIN` | `26` | Reduced-depth IQ margin in dB (0 = lossless) |
| `NDB_LOG_SUMMARY` | off | Log the full beacon table every N seconds (debugging) |
| `NDB_MQTT` | `1` | `0` turns [MQTT](#mqtt-and-home-assistant) off |
| `UBERSDR_INGEST_URL` | `http://<UBERSDR_URL host>:6926` | UberSDR's addon MQTT ingest port, only if the operator moved it |
| `WEB_PORT` | `6100` | |

### Streams

| mode | span | usable (outer 5% dropped) |
|---|---|---|
| `iq48` | ±24 kHz | ±21.6 kHz |
| `iq96` | ±48 kHz | ±43.2 kHz |
| `iq192` | ±96 kHz | ±86.4 kHz |
| `iq384` | ±192 kHz | ±172.8 kHz |

Streams can be combined (overlaps are de-duplicated). Coverage of the UK and Ireland, from the navaid list:

```yaml
NDB_STREAMS: "356000:iq192"                # default: all but Lichfield (545), 1 session, 192 kHz of IQ
NDB_STREAMS: "356000:iq192,545000:iq48"    # all of them, 2 sessions, 240 kHz of IQ (the least)
NDB_STREAMS: "411000:iq384"                # all of them, 1 session, 384 kHz of IQ
NDB_STREAMS: "359000:iq96"                 # 316–402 kHz: 93 of 110 NDBs, no password needed
```

Plain `iq` (10 kHz) is not supported. It is too narrow to be worth it, and each channel decimates by integer factors to 16 kHz and 4 kHz.

---

## Web UI and API

**Reception stats** (the Stats button, or `#stats` on the URL): beacons heard per hour and the furthest heard per hour, over 24 hours, 7 or 30 days; by hour of day, the average heard at once and, per distance band, the share of time its beacons were heard — where skywave opening up at night shows; and every beacon, least heard first, with its availability, SNR and a 24-cell hour-of-day strip, plus the published beacons never heard. Hour of day is UTC or the receiver's own time zone (from UberSDR's `/api/description`). Each section downloads as CSV.

History is sampled once a minute into 15-minute buckets and kept 30 days in `DATA_DIR/history.tsv`, which is rewritten daily without expired buckets, so it stays bounded (a few MB); `heard.tsv` is capped at 2000 beacons.

| Endpoint | |
|---|---|
| `GET /` | the UI (`static/`) |
| WebSocket `/` | pushes `status`, `spectrum`, `decodes`, `heard` (JSON) |
| `GET /api/status` | streams, receiver, every channel with its navaid match and candidates |
| `GET /api/spectrum` | averaged spectrum + floor per stream (2048 points) |
| `GET /api/beacons?max_age=300` | **for polling:** identified beacons heard in the last `max_age` seconds (1–604800, default 300; anything else is a `400`), most recent first, with position, distance, first/last heard, `last_identified` (the last copy of the ident; `last_heard` also counts its carrier between revisits), best SNR, and current SNR if live |
| `GET /api/stats?hours=168` | reception history for the stats view (1–720 hours, default 168): minutes heard and mean SNR per beacon per hour, receiver uptime per hour, and the published beacons in band and radius not heard. Per-hour values are strings of one base64 character per hour (0–63), first hour `t0`. |
| `GET /api/heard` | heard log |
| `GET /api/decodes` | recent live copy |
| `GET /api/navaids?max_km=…` | published NDBs in the covered band (default radius `NDB_RADIUS_KM`) |
| `GET /api/search?q=…` | search the whole navaid list by ident, name or frequency, with live / heard / in-band status |

---

## MQTT and Home Assistant

Running beside an UberSDR receiver with MQTT enabled, this publishes through the receiver's own MQTT connection, and shows up in Home Assistant as a device nested under the receiver. **There is nothing to configure.** It uses UberSDR's addon ingest port, as the receiver's other addons do (see `addon_mqtt.md` in [ka9q_ubersdr](https://github.com/madpsy/ka9q_ubersdr)). The port knows who is calling from the connection itself, so there is no broker address, credential or topic to set. Where there is no port to reach (MQTT off on the receiver), or it does not recognise this container as an installed addon, it logs that once, stays dormant, and asks again every 30 s. `NDB_MQTT=0` turns it off.

Only identified beacons are published, from the heard log. The live Morse copy never goes to MQTT.

| Topic (under `ubersdr/metrics/addons/ndb/`) | Retained | |
|---|---|---|
| `summary` | yes | Every 30 s, and within seconds of an event: streams, carriers decoding / waiting / tracked, counts heard now / last hour / last 24 h / new in 24 h / logged, the beacons heard now (strongest first, up to 50), the strongest, the farthest now and in 24 h, the last identified and the last new beacon |
| `events` | no | `new`: a beacon identified for the first time. `returned`: one heard again after an hour or more unheard while its stream was up (time the receiver or this addon was down doesn't count, so a restart doesn't set it off), with `absent_s`, the whole gap. Each event is sent once and in order, and waits out the receiver's rate limit instead of being dropped. |
| `status` | yes | `online` / `offline`, maintained by UberSDR |

A beacon, in the summary and in events:

```json
{"ident": "CBL", "name": "Campbeltown", "country": "GB", "confirmed": true, "freq_khz": 380.0,
 "dist_km": 161, "bearing_deg": 246, "lat": 55.4356, "lon": -5.6881, "snr_db": 26.9}
```

When the receiver has Home Assistant discovery on, these are declared, all reading the one retained summary: **Receiving** (connectivity, with the streams as attributes), **Beacons Heard Now** (the list as attributes), **Beacons Heard (1 h)**, **(24 h)**, **New Beacons (24 h)**, **Beacons Logged**, **Strongest Beacon** and **Strongest SNR**, **Farthest Beacon Now** and **(24 h)** (km, the beacon as attributes), **Last Identified**, **Last New Beacon**, **Carriers Decoding**, **Beacons Tracked**, and, as diagnostics, **Streams Connected**, **Beacon Database** and **Started**. A figure with nothing to show (nothing heard yet, or the receiver position unknown) stays `unknown` rather than reading zero.

---

## Management scripts

| Script | Action |
|--------|--------|
| `./start.sh` | Start the container |
| `./stop.sh` | Stop the container |
| `./restart.sh` | Restart the container |
| `./update.sh` | Re-run the installer: pull the latest image and restart |

---

## Building from source

Needs `build-essential cmake libcurl4-openssl-dev libssl-dev zlib1g-dev pkg-config`.

```bash
./build.sh                  # clones IXWebSocket, fetches navaids.csv, builds build/ubersdr_ndb
./build/ubersdr_ndb --url http://ubersdr:8080 --stream 356000:iq192 --web-static static
./docker.sh                 # local image;  ./docker.sh push  for multi-arch
```

### Offline / debugging

```bash
# record the IQ of a live run
ubersdr_ndb --url … --stream 356000:iq192 --dump-iq cap.iq
# replay it (as fast as it decodes, or --realtime to watch the UI)
ubersdr_ndb --iq-file cap.iq --rate 96000 --center 359000 --lat 56.04 --lon -3.35
# one channel's ggmorse input as a WAV; CHAN_WPM / CHAN_PITCH pin ggmorse
cmake --build build --target chan_audio
./build/chan_audio cap.iq 96000 359000 341000 edn.wav
```

---

## Status and next steps

A working base. Tested against a live receiver in Scotland: it identified EDN, UW, PIK, ATF, DND, CBL and CFN (333 km), placed them correctly, and put nothing false in the heard log. With the default `iq192` stream and 48 channels (before revisits, when every identified beacon kept a channel) it used ~16% of one core and ~45 MB. About two thirds of that is the per-channel mixer and decimation filters; a shared FFT channelizer would cut it further if needed.

Known gaps, roughly in order of payoff:

- **Unkeyed carriers.** At the test site, most of the carriers without an ident sit on exact kHz, and each one's strongest "sideband" is its neighbour 1 kHz away. That's an unmodulated spur comb (local RFI), not NDBs. The UI marks a carrier "no keying" after a minute below 15 dB tone contrast with no ident cycle. Recognising the comb in the detector would stop it using channels at all.
- **A1A (keyed-carrier) beacons** demodulate to a keyed DC step that ggmorse can't see. Detect it and switch the channel to a BFO (see the TODO in `ndb_channel.cpp`).
- **Channel filter.** It's a fixed ±1.3 kHz. Narrowing it to the carrier and the found tone would let beacons closer than 2.5 kHz to a strong one be decoded instead of suppressed.
- **Unidentified carriers on exact kHz** (344, 352, 361 … kHz) that never copy are probably not all NDBs. A "never copied in N hours" state would tidy the table.
