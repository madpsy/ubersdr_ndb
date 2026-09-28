# ubersdr_ndb

**Multi-beacon NDB decoder addon for [UberSDR](https://github.com/madpsy/ka9q_ubersdr)**

Requests one or more wideband IQ streams from UberSDR, finds every non-directional beacon (NDB) carrier in them, and decodes each beacon's Morse ident **in parallel**. Decoded idents are matched against the [OurAirports](https://ourairports.com/data/) navaid database and located relative to the receiver. A web UI shows the spectrum, a live beacon table, the copy as it is decoded, a persistent heard log, and a map.

The default, one `iq96` stream centred on 359 kHz, covers roughly 316–402 kHz and costs a single UberSDR session, however many beacons are in it. That centre is chosen from the navaid list: it takes in 93 of the 110 NDBs in the UK and Ireland, including those at 399–401 kHz. For another region, see [Streams](#streams).

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
        │     gone for 2 min → dropped (30 min if it was identified)
        ▼
NdbChannel (one per beacon)
   rotate carrier to DC → FIR ↓ to 16 kHz → FIR ↓ to 4 kHz (±1.3 kHz)
   → AM envelope ÷ carrier level → 4th-order 250 Hz high-pass
   ├→ KeyingDecoder (NDB-specific: the copy shown)   ─┐
   └→ ggmorse (general CW decoder: second opinion)   ─┴→ ident tally:
        most frequent clean 2–4 char token over the last hour
        │
        ▼
main.cpp ─ navaid match, heard log (/data/heard.tsv), HTTP + WebSocket
        │
        ▼
Browser: static/index.html + app.js  (spectrum · beacons · map · live copy · heard log)
```

Everything is decided server-side. A browser that connects is sent the full current state straight away (status, spectrum, heard log, and the last few minutes of copy), then live updates: decoded characters as they arrive, status at 1 Hz, spectrum every 2 s.

### Two Morse decoders

Most NDBs are A2A: a continuous carrier with the ident keyed as a 400 or 1020 Hz tone. An envelope detector turns that into a keyed audio tone, and two decoders read it.

**KeyingDecoder** (`src/keying_decoder.*`) is written for how NDBs key: a machine sending the same letters at a fixed speed and level, with a long silence between repetitions. It learns the things that don't change, over a span long enough to include both keying and silence:

- the tone, from a long averaged spectrum;
- the on/off threshold, from the last 20 s of tone envelope;
- the dit length, from the last 60 marks.

Classification is then textbook. State changes are debounced by ~30% of a dit, so a fade inside a dah doesn't split it. Letters are only emitted while the tone's on/off contrast is at least 15 dB and the timing looks like machine keying (a dit of 50–250 ms, with most marks at exactly one or three dits), so noise produces nothing. Its copy is the one shown and streamed live.

Decoding is blind: neither decoder is told what to expect. The navaid database is used afterwards, to name and locate what was copied. An ident that matches nothing on its frequency needs 5 copies, rather than 2, before it enters the heard log.

**[ggmorse](https://github.com/ggerganov/ggmorse)** is the library UberSDR's own CW decoder (`cw-decoder`) is built on, vendored from `ka9q_ubersdr/audio_extensions/morse/external/ggmorse` with changes for this use; see [`third_party/ggmorse/README.ubersdr_ndb.md`](third_party/ggmorse/README.ubersdr_ndb.md). It feeds the ident tally as a second opinion. On its own it struggles with NDBs because it re-estimates threshold and speed over a rolling 3 s window, which an NDB's inter-ident silence dominates.

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
| `NDB_STREAMS` | `359000:iq96` | IQ streams, comma-separated `centreHz:mode`. One session each. |
| `PASS` | | UberSDR bypass password. Wide IQ modes usually need one. |
| `NDB_PINNED` | | Hz, comma-separated: always decode these, even if not detected |
| `NDB_SNR` | `10` | Carrier detection threshold, dB above the noise floor |
| `NDB_ASSIST_KM` | `1500` | Published NDBs this close get a 7 dB threshold (0 = off) |
| `NDB_MAX_CHANNELS` | `24` | Per stream |
| `RECEIVER_LAT` / `RECEIVER_LON` | from UberSDR | Override the receiver position |
| `MIN_MARGIN` | `26` | Reduced-depth IQ margin in dB (0 = lossless) |
| `NDB_LOG_SUMMARY` | off | Log the full beacon table every N seconds (debugging) |
| `WEB_PORT` | `6100` | |

### Streams

| mode | span | usable (outer 5% dropped) |
|---|---|---|
| `iq48` | ±24 kHz | ±21.6 kHz |
| `iq96` | ±48 kHz | ±43.2 kHz |
| `iq192` | ±96 kHz | ±86.4 kHz |
| `iq384` | ±192 kHz | ±172.8 kHz |

Cover more of the band with more streams (overlaps are de-duplicated), or one wider one:

```yaml
NDB_STREAMS: "300000:iq96,390000:iq96,480000:iq96"   # ~257–523 kHz, 3 sessions
NDB_STREAMS: "390000:iq384"                          # ~217–563 kHz, 1 session
```

Plain `iq` (10 kHz) is not supported. It is too narrow to be worth it, and each channel decimates by integer factors to 16 kHz and 4 kHz.

---

## Web UI and API

| Endpoint | |
|---|---|
| `GET /` | the UI (`static/`) |
| WebSocket `/` | pushes `status`, `spectrum`, `decodes`, `heard` (JSON) |
| `GET /api/status` | streams, receiver, every channel with its navaid match and candidates |
| `GET /api/spectrum` | averaged spectrum + floor per stream (2048 points) |
| `GET /api/heard` | heard log |
| `GET /api/decodes` | recent live copy |
| `GET /api/navaids?max_km=2500` | published NDBs in the covered band |

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
./build/ubersdr_ndb --url http://ubersdr:8080 --stream 359000:iq96 --web-static static
./docker.sh                 # local image;  ./docker.sh push  for multi-arch
```

### Offline / debugging

```bash
# record the IQ of a live run
ubersdr_ndb --url … --stream 359000:iq96 --dump-iq cap.iq
# replay it (as fast as it decodes, or --realtime to watch the UI)
ubersdr_ndb --iq-file cap.iq --rate 96000 --center 359000 --lat 56.04 --lon -3.35
# one channel's ggmorse input as a WAV; CHAN_WPM / CHAN_PITCH pin ggmorse
cmake --build build --target chan_audio
./build/chan_audio cap.iq 96000 359000 341000 edn.wav
```

---

## Status and next steps

A working base. Tested against a live receiver in Scotland: from one `iq96` stream it identified EDN, UW, PIK, ATF, DND and CBL, placed them correctly, and put nothing false in the heard log. It uses ~35% of one core and ~80 MB for 24 channels.

Known gaps, roughly in order of payoff:

- **Fold the keying over the ident period.** An NDB repeats the same ident every few seconds, forever. Averaging the tone envelope over that period integrates a weak beacon up out of the noise, where decoding each repetition on its own never will. This is the big one for weak and DX beacons. 331.0 **GLW** (Glasgow, 70 km) is the test case: its carrier is 30 dB up but its tone sidebands are 16–23 dB below it, and the keying is fragmented on both sidebands and at every envelope bandwidth tried.
- **Unkeyed carriers.** At the test site, most of the carriers without an ident sit on exact kHz, and each one's strongest "sideband" is its neighbour 1 kHz away. That's an unmodulated spur comb (local RFI), not NDBs. The UI marks a carrier "no keying" after a minute below 15 dB tone contrast. Recognising the comb in the detector would stop it using channels at all.
- **Is ggmorse still earning its CPU?** It is most of the per-channel cost. If it doesn't beat KeyingDecoder on weak beacons, make it optional. If it does, the change to make is lengthening its 3 s analysis window to span an ident gap.
- **A1A (keyed-carrier) beacons** demodulate to a keyed DC step that ggmorse can't see. Detect it and switch the channel to a BFO (see the TODO in `ndb_channel.cpp`).
- **Channel filter.** It's a fixed ±1.3 kHz. Narrowing it to the carrier and the found tone would let beacons closer than 2.5 kHz to a strong one be decoded instead of suppressed.
- **Unidentified carriers on exact kHz** (344, 352, 361 … kHz) that never copy are probably not all NDBs. A "never copied in N hours" state would tidy the table.
