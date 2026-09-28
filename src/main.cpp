// ubersdr_ndb — multi-NDB decoder addon for UberSDR
//
// Connects to an UberSDR instance, requests one or more wideband IQ streams,
// finds every NDB carrier in each, and decodes each beacon's Morse ident in
// parallel. Decoded idents are matched against the OurAirports navaid list and
// located relative to the receiver. Serves a web UI (spectrum, beacon table,
// map) and a JSON API on a local port.
//
// Usage:
//   ubersdr_ndb --url http://ubersdr:8080 --stream 356000:iq192 [--stream ...] [options]
//   ubersdr_ndb --iq-file capture.iq --rate 96000 --center 359000   (offline)
//
// Run with --help for the full option list.

#include "navaids.h"
#include "ndb_decoder.h"
#include "pcm_v4.hpp"

#include <curl/curl.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kDefaultWebPort = 6100;
constexpr int kSpectrumPoints = 2048;
const char   *kUserAgent      = "ubersdr_ndb/0.1";

// Reduced-depth IQ margin, as the other addons use it (see ubersdr_loran
// main.go): 26 dB below the band's own noise floor is transparent and roughly
// halves the stream. 0 asks for lossless.
constexpr int kMinMarginDefault = 26;

std::atomic<bool> g_running{true};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct StreamSpec {
    double      center_hz = 0.0;
    std::string mode = "iq96";
};

struct Options {
    std::string url;
    std::string password;
    std::vector<StreamSpec> streams;
    int         web_port = kDefaultWebPort;
    std::string web_static = "./static";
    int         min_margin = kMinMarginDefault;
    std::string navaids_path;
    double      rx_lat = NAN, rx_lon = NAN;
    double      assist_km = 1500.0;  // published beacons this close get the lower detection threshold
    double      map_km = 2500.0;     // the map's "unheard beacons" layer reaches this far
    std::string data_dir;            // heard log persisted here ("" = memory only)
    int         summary_every = 0;   // seconds between beacon-table dumps to the log (0 = never)
    std::string dump_iq;        // write received IQ (int16 interleaved) here (first stream)
    std::string iq_file;        // offline: read IQ from here instead
    double      file_rate = 0.0;
    bool        realtime = false;
    ndb::DecoderConfig dec;
};

void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s --url <http://host:port> --stream <Hz[:mode]> [--stream ...] [options]\n"
        "       %s --iq-file <file> --rate <Hz> --center <Hz> [options]\n"
        "\n"
        "Source:\n"
        "  --url URL          UberSDR base URL (http/https)\n"
        "  --pass PASSWORD    bypass password (wide IQ modes usually need one)\n"
        "  --stream HZ[:MODE] an IQ stream centred on HZ; MODE is iq48 | iq96 | iq192 | iq384\n"
        "                     (default iq96). Repeatable; also accepts a comma-separated list.\n"
        "  --center HZ        same as --stream HZ (and the centre of --iq-file)\n"
        "  --min-margin DB    reduced-depth IQ margin, 0=lossless, else 15-60 (default: %d)\n"
        "  --iq-file FILE     decode a raw int16 interleaved I/Q file instead of connecting\n"
        "  --rate HZ          sample rate of --iq-file\n"
        "  --realtime         pace --iq-file at its sample rate (for watching the UI)\n"
        "  --dump-iq FILE     also write the first stream's IQ to FILE (for --iq-file later)\n"
        "\n"
        "Decoder:\n"
        "  --ndb HZ           always decode this frequency (repeatable / comma-separated)\n"
        "  --no-auto          only decode --ndb frequencies, no carrier search\n"
        "  --ggmorse MODE     ggmorse as a second decoder: auto (default) gives a small pool\n"
        "                     to unidentified channels showing keying; all = every channel\n"
        "                     (~10x the CPU); off = keying decoder only\n"
        "  --ggmorse-slots N  auto: ggmorse instances at once, per stream (default: 6)\n"
        "  --snr DB           carrier detection threshold above floor (default: %.0f)\n"
        "  --max-channels N   cap on simultaneous beacons per stream (default: %d)\n"
        "  --drop-after S     forget a beacon unseen for S seconds (default: %.0f)\n"
        "\n"
        "Beacon database:\n"
        "  --navaids FILE     OurAirports navaids.csv (default: next to the binary, then\n"
        "                     /usr/local/share/ubersdr_ndb/navaids.csv)\n"
        "  --lat DEG --lon DEG  receiver position (default: from UberSDR /api/description)\n"
        "  --assist-km KM     published beacons within KM get a lower detection threshold\n"
        "                     (default: 1500, 0 = off)\n"
        "  --map-km KM        radius of the map's unheard-beacons layer (default: 2500)\n"
        "  --data-dir DIR     keep the heard log in DIR/heard.tsv across restarts\n"
        "  --summary-every S  log the full beacon table every S seconds (default: 0 = off;\n"
        "                     --iq-file runs always print one at the end)\n"
        "\n"
        "Web:\n"
        "  --web-port N       web UI port (default: %d, 0 = disabled)\n"
        "  --web-static DIR   web UI files (default: ./static)\n",
        argv0, argv0, kMinMarginDefault, ndb::DetectorConfig{}.snr_threshold_db,
        ndb::DecoderConfig{}.max_channels, ndb::DecoderConfig{}.drop_after_s, kDefaultWebPort);
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty()) out.push_back(item);
    return out;
}

bool valid_mode(const std::string &m) { return m == "iq48" || m == "iq96" || m == "iq192" || m == "iq384"; }

bool parse_stream(const std::string &s, StreamSpec &out)
{
    auto colon = s.find(':');
    out.center_hz = atof(s.substr(0, colon).c_str());
    out.mode = colon == std::string::npos ? "iq96" : s.substr(colon + 1);
    if (out.center_hz <= 0) {
        fprintf(stderr, "error: bad stream '%s' (want HZ or HZ:MODE)\n", s.c_str());
        return false;
    }
    if (!valid_mode(out.mode)) {
        fprintf(stderr, "error: stream '%s': mode must be iq48, iq96, iq192 or iq384 "
                        "(plain iq is 10 kHz: too narrow, and not a multiple of 16 kHz)\n", s.c_str());
        return false;
    }
    return true;
}

bool parse_args(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s needs a value\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(argv[0]); exit(0); }
        else if (a == "--url")          o.url = next("--url");
        else if (a == "--pass")         o.password = next("--pass");
        else if (a == "--stream" || a == "--center") {
            for (const auto &s : split(next(a.c_str()), ',')) {
                StreamSpec sp;
                if (!parse_stream(s, sp)) return false;
                o.streams.push_back(sp);
            }
        }
        else if (a == "--min-margin")   o.min_margin = atoi(next("--min-margin").c_str());
        else if (a == "--iq-file")      o.iq_file = next("--iq-file");
        else if (a == "--rate")         o.file_rate = atof(next("--rate").c_str());
        else if (a == "--realtime")     o.realtime = true;
        else if (a == "--dump-iq")      o.dump_iq = next("--dump-iq");
        else if (a == "--ndb")
            for (const auto &s : split(next("--ndb"), ',')) o.dec.pinned_hz.push_back(atof(s.c_str()));
        else if (a == "--no-auto")      o.dec.auto_detect = false;
        else if (a == "--ggmorse") {
            std::string m = next("--ggmorse");
            if (m == "off") o.dec.ggmorse = ndb::DecoderConfig::Ggmorse::Off;
            else if (m == "auto") o.dec.ggmorse = ndb::DecoderConfig::Ggmorse::Auto;
            else if (m == "all") o.dec.ggmorse = ndb::DecoderConfig::Ggmorse::All;
            else { fprintf(stderr, "error: --ggmorse must be off, auto or all\n"); return false; }
        }
        else if (a == "--ggmorse-slots") o.dec.ggm_slots = atoi(next("--ggmorse-slots").c_str());
        else if (a == "--snr")          o.dec.detector.snr_threshold_db = float(atof(next("--snr").c_str()));
        else if (a == "--max-channels") o.dec.max_channels = atoi(next("--max-channels").c_str());
        else if (a == "--drop-after")   o.dec.drop_after_s = atof(next("--drop-after").c_str());
        else if (a == "--navaids")      o.navaids_path = next("--navaids");
        else if (a == "--lat")          o.rx_lat = atof(next("--lat").c_str());
        else if (a == "--lon")          o.rx_lon = atof(next("--lon").c_str());
        else if (a == "--assist-km")    o.assist_km = atof(next("--assist-km").c_str());
        else if (a == "--map-km")       o.map_km = atof(next("--map-km").c_str());
        else if (a == "--data-dir")     o.data_dir = next("--data-dir");
        else if (a == "--summary-every") o.summary_every = atoi(next("--summary-every").c_str());
        else if (a == "--web-port")     o.web_port = atoi(next("--web-port").c_str());
        else if (a == "--web-static")   o.web_static = next("--web-static");
        else { fprintf(stderr, "error: unknown option %s\n", a.c_str()); return false; }
    }
    if (o.streams.empty()) { fprintf(stderr, "error: at least one --stream is required\n"); return false; }
    if (o.url.empty() == o.iq_file.empty()) {
        fprintf(stderr, "error: give exactly one of --url or --iq-file\n");
        return false;
    }
    if (!o.iq_file.empty()) {
        if (o.file_rate <= 0) { fprintf(stderr, "error: --iq-file needs --rate\n"); return false; }
        if (o.streams.size() != 1) { fprintf(stderr, "error: --iq-file takes exactly one --center\n"); return false; }
        o.streams[0].mode = "file";
    }
    if (o.min_margin != 0 && (o.min_margin < 15 || o.min_margin > 60)) {
        fprintf(stderr, "error: --min-margin must be 0 (lossless) or 15-60\n");
        return false;
    }
    if (!o.dec.auto_detect && o.dec.pinned_hz.empty()) {
        fprintf(stderr, "error: --no-auto needs at least one --ndb\n");
        return false;
    }
    while (!o.url.empty() && o.url.back() == '/') o.url.pop_back();
    return true;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

// One IQ stream: the thread receiving it writes, the web threads read.
struct Stream {
    int index = 0;
    StreamSpec spec;
    ndb::DecoderConfig cfg;        // pinned_hz narrowed to what this stream covers
    std::mutex mu;
    std::unique_ptr<ndb::NdbDecoder> dec;
    std::atomic<bool> connected{false};
    std::string status_msg = "starting";
};

struct Receiver {
    std::mutex mu;
    bool   have_pos = false;
    double lat = 0.0, lon = 0.0;
    std::string name, location, callsign;
};

// One chunk of live copy, as ggmorse produced it.
struct DecodeEvent {
    int64_t t_ms;       // wall clock
    int     id;         // channel id as the status document numbers them
    double  freq_hz;
    std::string text;
};

// The heard log: every beacon ever identified, kept server-side (and on disk
// with --data-dir) so a browser that connects now sees the whole history, not
// just what happens to be decoding at this moment.
struct HeardEntry {
    std::string ident;          // published ident when matched, else decoded
    bool   confirmed = false;   // matched the database (exactly or all-but-last-letter)
    double freq_hz = 0.0;       // measured carrier
    std::string name, country;
    double lat = NAN, lon = NAN, dist_km = -1.0, bearing_deg = -1.0;
    int64_t first_s = 0, last_s = 0;  // wall clock
    float  best_snr_db = 0.0f;
    int    best_copies = 0;     // highest ident tally seen
};

struct App {
    Options o;
    std::vector<std::unique_ptr<Stream>> streams;
    Receiver rx;
    ndb::NavaidDb navaids;         // loaded once; receiver set before the web server starts
    std::mutex navaids_mu;         // guards set_receiver() against concurrent readers

    std::mutex decodes_mu;
    std::vector<DecodeEvent> decodes_pending;   // not yet pushed
    std::deque<DecodeEvent> decodes_recent;     // backfill for new clients

    std::mutex heard_mu;
    std::map<std::string, HeardEntry> heard;    // key: ident@kHz
    bool heard_dirty = false;
};

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Length of a well-formed UTF-8 sequence starting at s[i], or 0 if it isn't.
size_t utf8_len(const std::string &s, size_t i)
{
    const unsigned char c = s[i];
    size_t n = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c <= 0xdf ? 2 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) return 0;
    return n;
}

// JSON string body. Also guarantees valid UTF-8: every WebSocket text frame
// carries JSON built here, and IXWebSocket closes the socket (1007) rather
// than send a frame that isn't. Navaid names are legitimately UTF-8 and pass
// through; a stray byte is replaced with U+FFFD.
std::string json_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 2);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = s[i];
        if (c >= 0x80) {
            size_t n = utf8_len(s, i);
            if (n) { o.append(s, i, n); i += n - 1; }
            else o += "\xef\xbf\xbd";
            continue;
        }
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}

std::string q(const std::string &s) { return "\"" + json_escape(s) + "\""; }
std::string q_str(const std::string &s) { return q(s); }

std::string num(double v, int places = 1)
{
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof b, "%.*f", places, v);
    return b;
}

// Minimal extraction from UberSDR's /api/description, which we only need four
// fields of: find `"key":` after `anchor` and read the scalar that follows.
std::string json_scalar_after(const std::string &body, const std::string &anchor, const std::string &key)
{
    size_t a = anchor.empty() ? 0 : body.find("\"" + anchor + "\"");
    if (a == std::string::npos) return "";
    size_t k = body.find("\"" + key + "\"", a);
    if (k == std::string::npos) return "";
    size_t c = body.find(':', k);
    if (c == std::string::npos) return "";
    size_t p = body.find_first_not_of(" \t\r\n", c + 1);
    if (p == std::string::npos) return "";
    if (body[p] == '"') {
        std::string out;
        for (size_t i = p + 1; i < body.size() && body[i] != '"'; ++i) {
            if (body[i] == '\\' && i + 1 < body.size()) ++i;
            out += body[i];
        }
        return out;
    }
    size_t e = body.find_first_of(",}]", p);
    return body.substr(p, e - p);
}

std::string navaid_json(const ndb::NavaidHit &h, bool exact)
{
    const auto &n = *h.nav;
    std::string j = "{\"ident\":" + q(n.ident) + ",\"name\":" + q(n.name) + ",\"country\":" + q(n.country) +
                    ",\"freq_hz\":" + num(n.freq_hz, 0) + ",\"lat\":" + num(n.lat, 4) + ",\"lon\":" + num(n.lon, 4) +
                    ",\"power\":" + q(n.power);
    if (h.dist_km >= 0) j += ",\"dist_km\":" + num(h.dist_km, 0) + ",\"bearing_deg\":" + num(h.bearing_deg, 0);
    j += ",\"exact\":" + std::string(exact ? "true" : "false") + "}";
    return j;
}

// ---------------------------------------------------------------------------
// Status / spectrum / navaid documents
// ---------------------------------------------------------------------------

struct ChanOut {
    ndb::ChannelSnapshot c;
    int stream;
};

std::string status_json(App &app)
{
    std::string j = "{\"type\":\"status\"";

    {
        std::lock_guard<std::mutex> lk(app.rx.mu);
        j += ",\"receiver\":{\"name\":" + q(app.rx.name) + ",\"location\":" + q(app.rx.location) +
             ",\"callsign\":" + q(app.rx.callsign);
        if (app.rx.have_pos) j += ",\"lat\":" + num(app.rx.lat, 5) + ",\"lon\":" + num(app.rx.lon, 5);
        j += "}";
    }
    j += ",\"navaids_loaded\":" + std::to_string(app.navaids.size());
    j += ",\"map_km\":" + num(app.o.map_km, 0);

    std::vector<ChanOut> chans;
    j += ",\"streams\":[";
    for (size_t i = 0; i < app.streams.size(); ++i) {
        Stream &s = *app.streams[i];
        std::lock_guard<std::mutex> lk(s.mu);
        if (i) j += ",";
        j += "{\"index\":" + std::to_string(i) + ",\"center_hz\":" + num(s.spec.center_hz, 0) +
             ",\"mode\":" + q(s.spec.mode) + ",\"connected\":" + (s.connected ? "true" : "false") +
             ",\"message\":" + q(s.status_msg);
        if (s.dec) {
            j += ",\"sample_rate\":" + num(s.dec->sample_rate(), 0) + ",\"stream_time\":" + num(s.dec->stream_time());
            j += ",\"waiting\":" + std::to_string(s.dec->waiting());
            for (auto &c : s.dec->snapshot()) chans.push_back({std::move(c), int(i)});
        }
        j += "}";
    }
    j += "]";

    // Overlapping streams hear the same beacon twice: keep the stronger copy.
    std::sort(chans.begin(), chans.end(), [](const ChanOut &a, const ChanOut &b) { return a.c.freq_hz < b.c.freq_hz; });
    std::vector<ChanOut> uniq;
    for (auto &c : chans) {
        if (!uniq.empty() && std::fabs(uniq.back().c.freq_hz - c.c.freq_hz) < 10.0) {
            if (c.c.snr_db > uniq.back().c.snr_db) uniq.back() = std::move(c);
            continue;
        }
        uniq.push_back(std::move(c));
    }

    std::lock_guard<std::mutex> nlk(app.navaids_mu);
    j += ",\"channels\":[";
    bool first = true;
    for (const auto &co : uniq) {
        const auto &c = co.c;
        if (!first) j += ",";
        first = false;
        j += "{\"id\":" + std::to_string(co.stream * 1000 + c.id) + ",\"stream\":" + std::to_string(co.stream);
        j += ",\"freq_hz\":" + num(c.freq_hz) + ",\"snr_db\":" + num(c.snr_db) + ",\"carrier_db\":" + num(c.carrier_db);
        j += ",\"pitch_hz\":" + num(c.pitch_hz, 0) + ",\"speed_wpm\":" + num(c.speed_wpm, 0) + ",\"cost\":" + num(c.cost, 3);
        j += ",\"ident\":" + q(c.ident) + ",\"ident_count\":" + std::to_string(c.ident_count);
        j += ",\"contrast_db\":" + num(c.contrast_db) + ",\"keying\":" + (c.keying ? "true" : "false");
        j += ",\"ggmorse\":" + std::string(c.ggmorse ? "true" : "false");
        j += ",\"text\":" + q(c.text) + ",\"text_ggm\":" + q(c.text_ggm) + ",\"pinned\":" + (c.pinned ? "true" : "false");
        j += ",\"age_s\":" + num(c.age_s, 0) + ",\"last_seen_s\":" + num(c.last_seen_s, 0) +
             ",\"last_text_s\":" + num(c.last_text_s, 0);

        // Name it: the published NDB whose ident matches what was decoded,
        // else the nearest few on this frequency as candidates.
        bool exact = false;
        auto m = app.navaids.match(c.freq_hz, c.ident, exact);
        if (m.nav) j += ",\"navaid\":" + navaid_json(m, exact);
        auto cands = app.navaids.candidates(c.freq_hz);
        j += ",\"candidates\":[";
        for (size_t k = 0; k < cands.size() && k < 4; ++k) {
            if (k) j += ",";
            j += navaid_json(cands[k], false);
        }
        j += "]}";
    }
    return j + "]}";
}

std::string spectrum_json(App &app)
{
    std::string j = "{\"type\":\"spectrum\",\"streams\":[";
    for (size_t i = 0; i < app.streams.size(); ++i) {
        Stream &s = *app.streams[i];
        std::vector<float> db, fl;
        double fs = 0;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.dec) {
                s.dec->spectrum(kSpectrumPoints, db, fl);
                fs = s.dec->sample_rate();
            }
        }
        if (i) j += ",";
        j += "{\"index\":" + std::to_string(i) + ",\"center_hz\":" + num(s.spec.center_hz, 0) +
             ",\"span_hz\":" + num(fs, 0) + ",\"db\":[";
        for (size_t k = 0; k < db.size(); ++k) j += (k ? "," : "") + num(db[k]);
        j += "],\"floor\":[";
        for (size_t k = 0; k < fl.size(); ++k) j += (k ? "," : "") + num(fl[k]);
        j += "]}";
    }
    return j + "]}";
}

// Every published NDB inside the covered spectrum (and, when the receiver is
// known, within max_km of it) — the map's "not heard" layer.
std::string navaids_json(App &app, double max_km)
{
    std::vector<std::pair<double, double>> bands;
    for (auto &sp : app.streams) {
        std::lock_guard<std::mutex> lk(sp->mu);
        double half = sp->dec ? 0.45 * sp->dec->sample_rate() : 0.0;
        if (half > 0) bands.push_back({sp->spec.center_hz - half, sp->spec.center_hz + half});
    }
    std::lock_guard<std::mutex> nlk(app.navaids_mu);
    std::string j = "{\"type\":\"navaids\",\"navaids\":[";
    bool first = true;
    for (const auto &n : app.navaids.all()) {
        bool in = false;
        for (auto &b : bands) in = in || (n.freq_hz >= b.first && n.freq_hz <= b.second);
        if (!in) continue;
        auto h = app.navaids.locate(n);
        if (h.dist_km >= 0 && h.dist_km > max_km) continue;
        if (!first) j += ",";
        first = false;
        j += navaid_json(h, false);
    }
    return j + "]}";
}

// Published beacons near enough to plausibly be heard. Empty until the
// receiver position is known — without it "near" means nothing, and assisting
// every NDB on Earth would just lower the threshold everywhere.
std::vector<double> assist_freqs(App &app)
{
    std::vector<double> out;
    if (app.o.assist_km <= 0) return out;
    std::lock_guard<std::mutex> nlk(app.navaids_mu);
    if (!app.navaids.have_receiver()) return out;
    for (const auto &n : app.navaids.all())
        if (app.navaids.locate(n).dist_km <= app.o.assist_km) out.push_back(n.freq_hz);
    return out;
}

// Fold the current identifications into the heard log. Called once a second.
void update_heard(App &app)
{
    std::vector<ndb::ChannelSnapshot> snaps;
    for (auto &sp : app.streams) {
        std::lock_guard<std::mutex> lk(sp->mu);
        if (!sp->dec) continue;
        for (auto &c : sp->dec->snapshot())
            if (!c.ident.empty() && c.last_seen_s < 60) snaps.push_back(std::move(c));
    }
    // An ident that matches the published beacon on its frequency is logged
    // on the tally's usual two copies; one that matches nothing needs more,
    // since there is no second source to agree with it.
    constexpr int kUnmatchedCopies = 5;
    if (snaps.empty()) return;
    const int64_t now = now_ms() / 1000;
    std::lock_guard<std::mutex> nlk(app.navaids_mu);
    std::lock_guard<std::mutex> hlk(app.heard_mu);
    for (const auto &c : snaps) {
        bool exact = false;
        auto m = app.navaids.match(c.freq_hz, c.ident, exact);
        if (!m.nav && c.ident_count < kUnmatchedCopies) continue;
        const std::string ident = m.nav ? m.nav->ident : c.ident;
        char key[64];
        snprintf(key, sizeof key, "%s@%.1f", ident.c_str(), (m.nav ? m.nav->freq_hz : c.freq_hz) / 1e3);
        auto &e = app.heard[key];
        if (e.first_s == 0) {
            e.first_s = now;
            fprintf(stderr, "heard: %s %.1f Hz%s\n", ident.c_str(), c.freq_hz,
                    m.nav ? (" — " + m.nav->name + " " + m.nav->country).c_str() : " (not in database)");
        }
        e.ident = ident;
        e.confirmed = m.nav != nullptr;
        e.freq_hz = c.freq_hz;
        if (m.nav) {
            e.name = m.nav->name;
            e.country = m.nav->country;
            e.lat = m.nav->lat;
            e.lon = m.nav->lon;
            e.dist_km = m.dist_km;
            e.bearing_deg = m.bearing_deg;
        }
        e.last_s = now;
        e.best_snr_db = std::max(e.best_snr_db, c.snr_db);
        e.best_copies = std::max(e.best_copies, c.ident_count);
        app.heard_dirty = true;
    }
    // Bounded, so months of running can't grow it (or heard.tsv) without
    // limit: past the cap, forget whatever was heard longest ago.
    constexpr size_t kHeardMax = 2000;
    while (app.heard.size() > kHeardMax) {
        auto oldest = app.heard.begin();
        for (auto it = app.heard.begin(); it != app.heard.end(); ++it)
            if (it->second.last_s < oldest->second.last_s) oldest = it;
        app.heard.erase(oldest);
    }
}

std::string heard_json(App &app)
{
    std::lock_guard<std::mutex> hlk(app.heard_mu);
    std::string j = "{\"type\":\"heard\",\"now\":" + std::to_string(now_ms() / 1000) + ",\"entries\":[";
    bool first = true;
    for (const auto &[key, e] : app.heard) {
        if (!first) j += ",";
        first = false;
        j += "{\"key\":" + q(key) + ",\"ident\":" + q(e.ident) + ",\"confirmed\":" + (e.confirmed ? "true" : "false") +
             ",\"freq_hz\":" + num(e.freq_hz) + ",\"name\":" + q(e.name) + ",\"country\":" + q(e.country) +
             ",\"lat\":" + num(e.lat, 4) + ",\"lon\":" + num(e.lon, 4) +
             ",\"dist_km\":" + (e.dist_km >= 0 ? num(e.dist_km, 0) : "null") +
             ",\"bearing_deg\":" + (e.bearing_deg >= 0 ? num(e.bearing_deg, 0) : "null") +
             ",\"first_s\":" + std::to_string(e.first_s) + ",\"last_s\":" + std::to_string(e.last_s) +
             ",\"best_snr_db\":" + num(e.best_snr_db) + ",\"best_copies\":" + std::to_string(e.best_copies) + "}";
    }
    return j + "]}";
}

// heard.tsv: one entry per line, tab-separated, in HeardEntry field order.
// Plain text rather than JSON so reading it back needs no parser.
void save_heard(App &app)
{
    if (app.o.data_dir.empty()) return;
    std::string tmp = app.o.data_dir + "/heard.tsv.tmp", path = app.o.data_dir + "/heard.tsv";
    {
        std::lock_guard<std::mutex> hlk(app.heard_mu);
        if (!app.heard_dirty) return;
        FILE *f = fopen(tmp.c_str(), "w");
        if (!f) { fprintf(stderr, "warning: cannot write %s\n", tmp.c_str()); return; }
        for (const auto &[key, e] : app.heard) {
            auto clean = [](std::string s) { for (char &c : s) if (c == '\t' || c == '\n') c = ' '; return s; };
            fprintf(f, "%s\t%s\t%d\t%.1f\t%s\t%s\t%.5f\t%.5f\t%.0f\t%.0f\t%lld\t%lld\t%.1f\t%d\n", clean(key).c_str(),
                    clean(e.ident).c_str(), e.confirmed ? 1 : 0, e.freq_hz, clean(e.name).c_str(), clean(e.country).c_str(),
                    e.lat, e.lon, e.dist_km, e.bearing_deg, (long long)e.first_s, (long long)e.last_s, e.best_snr_db,
                    e.best_copies);
        }
        fclose(f);
        app.heard_dirty = false;
    }
    rename(tmp.c_str(), path.c_str());
}

void load_heard(App &app)
{
    if (app.o.data_dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(app.o.data_dir, ec);
    if (ec) fprintf(stderr, "warning: cannot create %s: %s\n", app.o.data_dir.c_str(), ec.message().c_str());
    std::ifstream f(app.o.data_dir + "/heard.tsv");
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
        std::vector<std::string> v;
        size_t a = 0, b;
        while ((b = line.find('\t', a)) != std::string::npos) { v.push_back(line.substr(a, b - a)); a = b + 1; }
        v.push_back(line.substr(a));
        if (v.size() != 14) continue;
        HeardEntry e;
        e.ident = v[1];
        e.confirmed = v[2] == "1";
        e.freq_hz = atof(v[3].c_str());
        e.name = v[4];
        e.country = v[5];
        e.lat = atof(v[6].c_str());
        e.lon = atof(v[7].c_str());
        e.dist_km = atof(v[8].c_str());
        e.bearing_deg = atof(v[9].c_str());
        e.first_s = atoll(v[10].c_str());
        e.last_s = atoll(v[11].c_str());
        e.best_snr_db = float(atof(v[12].c_str()));
        e.best_copies = atoi(v[13].c_str());
        app.heard[v[0]] = e;
        ++n;
    }
    if (n) fprintf(stderr, "heard log: %zu beacons from %s/heard.tsv\n", n, app.o.data_dir.c_str());
}

std::string decodes_json(const std::vector<DecodeEvent> &ev, bool backfill)
{
    std::string j = std::string("{\"type\":\"decodes\",\"backfill\":") + (backfill ? "true" : "false") + ",\"items\":[";
    for (size_t i = 0; i < ev.size(); ++i) {
        if (i) j += ",";
        j += "{\"t\":" + std::to_string(ev[i].t_ms) + ",\"id\":" + std::to_string(ev[i].id) +
             ",\"freq_hz\":" + num(ev[i].freq_hz) + ",\"text\":" + q(ev[i].text) + "}";
    }
    return j + "]}";
}

// Take what arrived since the last call, and remember it for backfill.
std::vector<DecodeEvent> take_decodes(App &app)
{
    constexpr size_t kRecent = 300;
    std::lock_guard<std::mutex> lk(app.decodes_mu);
    std::vector<DecodeEvent> out;
    out.swap(app.decodes_pending);
    for (const auto &e : out) {
        app.decodes_recent.push_back(e);
        if (app.decodes_recent.size() > kRecent) app.decodes_recent.pop_front();
    }
    return out;
}

// Search the whole navaid list by ident, name, or frequency ("341", "341.5"),
// and say for each hit whether it is live now, in the heard log, and inside
// the spectrum being received. Ranked: exact ident, ident prefix, frequency,
// name; then nearest first.
std::string search_json(App &app, const std::string &query, size_t limit)
{
    std::string q;
    for (char c : query)
        if (!isspace((unsigned char)c) || !q.empty()) q += c;
    while (!q.empty() && isspace((unsigned char)q.back())) q.pop_back();
    std::string qu = q, ql = q;
    for (char &c : qu) c = char(toupper((unsigned char)c));
    for (char &c : ql) c = char(tolower((unsigned char)c));
    char *endp = nullptr;
    const double qkhz = q.empty() ? 0.0 : strtod(q.c_str(), &endp);
    const bool is_freq = endp && *endp == 0 && qkhz >= 100.0 && qkhz <= 2000.0;

    std::string out = "{\"type\":\"search\",\"query\":" + q_str(q) + ",\"results\":[";
    if (q.empty()) return out + "]}";

    // What is live, and what is in the heard log, keyed by ident.
    std::map<std::string, std::pair<double, float>> live;   // ident -> freq, snr
    std::vector<std::pair<double, double>> bands;
    for (auto &sp : app.streams) {
        std::lock_guard<std::mutex> lk(sp->mu);
        if (!sp->dec) continue;
        const double half = 0.45 * sp->dec->sample_rate();
        bands.push_back({sp->spec.center_hz - half, sp->spec.center_hz + half});
        std::lock_guard<std::mutex> nlk(app.navaids_mu);
        for (const auto &c : sp->dec->snapshot()) {
            bool exact = false;
            auto m = app.navaids.match(c.freq_hz, c.ident, exact);
            if (m.nav) live[m.nav->ident + "@" + std::to_string(long(m.nav->freq_hz))] = {c.freq_hz, c.snr_db};
        }
    }
    std::map<std::string, HeardEntry> heard;
    {
        std::lock_guard<std::mutex> hlk(app.heard_mu);
        for (const auto &[k, e] : app.heard)
            if (e.confirmed) heard[e.ident + "@" + std::to_string(long(std::lround(e.freq_hz / 500.0) * 500))] = e;
    }

    struct Hit { int rank; double dist; ndb::NavaidHit h; };
    std::vector<Hit> hits;
    {
        std::lock_guard<std::mutex> nlk(app.navaids_mu);
        for (const auto &n : app.navaids.all()) {
            std::string name = n.name;
            for (char &c : name) c = char(tolower((unsigned char)c));
            int rank = -1;
            if (n.ident == qu) rank = 0;
            else if (qu.size() >= 1 && n.ident.compare(0, qu.size(), qu) == 0) rank = 1;
            else if (is_freq && std::fabs(n.freq_hz / 1e3 - qkhz) <= 0.5) rank = 2;
            else if (ql.size() >= 2 && name.find(ql) != std::string::npos) rank = 3;
            if (rank < 0) continue;
            auto h = app.navaids.locate(n);
            hits.push_back({rank, h.dist_km < 0 ? 1e9 : h.dist_km, h});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        return a.rank != b.rank ? a.rank < b.rank : a.dist < b.dist;
    });

    for (size_t i = 0; i < hits.size() && i < limit; ++i) {
        const auto &n = *hits[i].h.nav;
        std::string j = navaid_json(hits[i].h, false);
        j.pop_back();   // reopen the object to add status
        bool in_band = false;
        for (auto &b : bands) in_band = in_band || (n.freq_hz >= b.first && n.freq_hz <= b.second);
        j += ",\"in_band\":" + std::string(in_band ? "true" : "false");
        auto lv = live.find(n.ident + "@" + std::to_string(long(n.freq_hz)));
        if (lv != live.end()) j += ",\"live\":{\"freq_hz\":" + num(lv->second.first) + ",\"snr_db\":" + num(lv->second.second) + "}";
        auto hd = heard.find(n.ident + "@" + std::to_string(long(std::lround(n.freq_hz / 500.0) * 500)));
        if (hd != heard.end())
            j += ",\"heard\":{\"last_s\":" + std::to_string(hd->second.last_s) + ",\"first_s\":" +
                 std::to_string(hd->second.first_s) + ",\"best_snr_db\":" + num(hd->second.best_snr_db) + "}";
        out += (i ? "," : "") + j + "}";
    }
    return out + "],\"total\":" + std::to_string(hits.size()) + ",\"now\":" + std::to_string(now_ms() / 1000) + "}";
}

// ---------------------------------------------------------------------------
// HTTP helpers (libcurl) and session registration
// ---------------------------------------------------------------------------

std::string make_uuid4()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t hi = gen(), lo = gen();
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char b[37];
    snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", unsigned(hi >> 32), unsigned((hi >> 16) & 0xFFFF),
             unsigned(hi & 0xFFFF), unsigned(lo >> 48), (unsigned long long)(lo & 0xFFFFFFFFFFFFULL));
    return b;
}

size_t curl_write(char *p, size_t sz, size_t n, void *ud)
{
    static_cast<std::string *>(ud)->append(p, sz * n);
    return sz * n;
}

long http_request(const std::string &url, const std::string *post_body, std::string &resp)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_slist *h = nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (post_body) {
        h = curl_slist_append(h, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body->c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    }
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    long code = -1;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

std::string url_encode(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

std::string ws_url(const Options &o, const StreamSpec &sp, const std::string &session)
{
    std::string base = o.url;
    if (base.rfind("https://", 0) == 0) base = "wss://" + base.substr(8);
    else if (base.rfind("http://", 0) == 0) base = "ws://" + base.substr(7);
    std::string u = base + "/ws?frequency=" + std::to_string(long(sp.center_hz)) + "&mode=" + sp.mode +
                    "&format=pcm-zstd&version=4&user_session_id=" + session;
    // Absent min_margin is the server's lossless path; see kMinMarginDefault.
    if (o.min_margin > 0) u += "&min_margin=" + std::to_string(o.min_margin);
    if (!o.password.empty()) u += "&password=" + url_encode(o.password);
    return u;
}

// Receiver identity and position from /api/description, unless --lat/--lon
// were given. Retried on each stream (re)connect until it succeeds.
void fetch_receiver(App &app)
{
    {
        std::lock_guard<std::mutex> lk(app.rx.mu);
        if (!app.rx.name.empty() && app.rx.have_pos) return;
    }
    std::string body;
    long code = http_request(app.o.url + "/api/description", nullptr, body);
    if (code != 200) {
        fprintf(stderr, "warning: GET /api/description -> %ld; receiver position unknown\n", code);
        return;
    }
    std::lock_guard<std::mutex> lk(app.rx.mu);
    app.rx.name = json_scalar_after(body, "receiver", "name");
    app.rx.location = json_scalar_after(body, "receiver", "location");
    app.rx.callsign = json_scalar_after(body, "receiver", "callsign");
    if (!app.rx.have_pos) {
        std::string la = json_scalar_after(body, "gps", "lat"), lo = json_scalar_after(body, "gps", "lon");
        if (!la.empty() && !lo.empty()) {
            app.rx.lat = atof(la.c_str());
            app.rx.lon = atof(lo.c_str());
            app.rx.have_pos = !(app.rx.lat == 0.0 && app.rx.lon == 0.0);
        }
        if (app.rx.have_pos) {
            std::lock_guard<std::mutex> nlk(app.navaids_mu);
            app.navaids.set_receiver(app.rx.lat, app.rx.lon);
        }
        // Decoders created before the position was known have no assist
        // list yet. (Streams lock after rx.mu here; nothing locks the other
        // way round.)
        if (app.rx.have_pos) {
            auto freqs = assist_freqs(app);
            for (auto &sp : app.streams) {
                std::lock_guard<std::mutex> slk(sp->mu);
                if (sp->dec) sp->dec->set_assist(freqs);
            }
        }
    }
    fprintf(stderr, "receiver: %s — %s (%.4f, %.4f)%s\n", app.rx.name.c_str(), app.rx.location.c_str(), app.rx.lat,
            app.rx.lon, app.rx.have_pos ? "" : " [no position]");
}

// ---------------------------------------------------------------------------
// IQ into a stream's decoder
// ---------------------------------------------------------------------------

std::vector<double> assist_freqs(App &app);

struct IqSink {
    App &app;
    Stream &st;
    FILE *dump = nullptr;
    bool failed = false;

    void feed(const int16_t *iq, size_t n_samples, int rate)
    {
        if (failed) return;
        if (dump) fwrite(iq, sizeof(int16_t), n_samples, dump);
        std::lock_guard<std::mutex> lk(st.mu);
        if (!st.dec || st.dec->sample_rate() != rate) {
            std::string why = ndb::NdbDecoder::check_sample_rate(rate);
            if (!why.empty()) {
                fprintf(stderr, "[s%d] error: %s\n", st.index, why.c_str());
                st.status_msg = why;
                failed = true;
                return;
            }
            fprintf(stderr, "[s%d] IQ %d Hz, centre %.0f Hz: %.1f-%.1f kHz\n", st.index, rate, st.spec.center_hz,
                    (st.spec.center_hz - 0.5 * rate) / 1e3, (st.spec.center_hz + 0.5 * rate) / 1e3);
            st.dec = std::make_unique<ndb::NdbDecoder>(st.spec.center_hz, double(rate), st.cfg);
            st.dec->set_assist(assist_freqs(app));
            const int base = st.index * 1000;
            App *a = &app;
            st.dec->on_decode = [a, base](int id, double f, const std::string &text) {
                std::lock_guard<std::mutex> lk(a->decodes_mu);
                a->decodes_pending.push_back({now_ms(), base + id, f, text});
            };
        }
        st.dec->process_iq(iq, n_samples / 2);
    }
};

// ---------------------------------------------------------------------------
// Live UberSDR client, one per stream
// ---------------------------------------------------------------------------

void run_client(App &app, Stream &st)
{
    const Options &o = app.o;
    FILE *dump = nullptr;
    if (st.index == 0 && !o.dump_iq.empty()) {
        dump = fopen(o.dump_iq.c_str(), "wb");
        if (!dump) fprintf(stderr, "warning: cannot open %s for writing\n", o.dump_iq.c_str());
    }
    auto set_msg = [&](const std::string &m) {
        std::lock_guard<std::mutex> lk(st.mu);
        st.status_msg = m;
    };

    int backoff = 5;
    while (g_running) {
        fetch_receiver(app);
        const std::string session = make_uuid4();
        std::string resp;
        std::string body = "{\"user_session_id\":\"" + session + "\"";
        if (!o.password.empty()) body += ",\"password\":" + q(o.password);
        body += "}";
        long code = http_request(o.url + "/connection", &body, resp);
        if (code < 0) {
            fprintf(stderr, "[s%d] cannot reach %s/connection\n", st.index, o.url.c_str());
            set_msg("cannot reach UberSDR");
        } else if (resp.find("\"allowed\":true") == std::string::npos) {
            fprintf(stderr, "[s%d] connection refused (HTTP %ld): %s\n", st.index, code, resp.c_str());
            set_msg("UberSDR refused the connection");
        } else {
            if (resp.find("\"" + st.spec.mode + "\"") == std::string::npos)
                fprintf(stderr, "[s%d] warning: allowed_iq_modes does not list %s — %s\n", st.index,
                        st.spec.mode.c_str(),
                        o.password.empty() ? "a bypass password is probably needed" : "check the password");
            backoff = 5;

            IqSink sink{app, st, dump};
            ubersdr::PCMv4StreamDecoder v4;
            std::atomic<bool> done{false};
            unsigned long decode_errors = 0;

            ix::WebSocket ws;
            ws.setUrl(ws_url(o, st.spec, session));
            ws.setHandshakeTimeout(10);
            ws.disableAutomaticReconnection();
            ix::SocketTLSOptions tls;
            tls.caFile = "NONE";
            ws.setTLSOptions(tls);
            ws.setExtraHeaders({{"User-Agent", kUserAgent}});

            ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr &m) {
                switch (m->type) {
                case ix::WebSocketMessageType::Open:
                    fprintf(stderr, "[s%d] connected: %.0f Hz %s\n", st.index, st.spec.center_hz, st.spec.mode.c_str());
                    st.connected = true;
                    set_msg("connected");
                    break;
                case ix::WebSocketMessageType::Close:
                    fprintf(stderr, "[s%d] websocket closed: %s\n", st.index, m->closeInfo.reason.c_str());
                    done = true;
                    break;
                case ix::WebSocketMessageType::Error:
                    fprintf(stderr, "[s%d] websocket error: %s\n", st.index, m->errorInfo.reason.c_str());
                    done = true;
                    break;
                case ix::WebSocketMessageType::Message: {
                    if (!m->binary) {
                        if (m->str.find("\"error\"") != std::string::npos)
                            fprintf(stderr, "[s%d] server: %s\n", st.index, m->str.c_str());
                        break;
                    }
                    // Every frame must reach the v4 decoder, even one we then
                    // drop: its predictor is backward adaptive.
                    const auto *p = reinterpret_cast<const uint8_t *>(m->str.data());
                    if (ubersdr::PCMv4StreamDecoder::isZstdFrame(p, m->str.size())) {
                        if (++decode_errors == 1)
                            fprintf(stderr, "[s%d] server sent a protocol v1 (zstd) frame: UberSDR is older than "
                                            "0.1.63 and cannot serve audio protocol version 4\n", st.index);
                        break;
                    }
                    ubersdr::PCMv4Header h;
                    std::string err;
                    if (!v4.decode(p, m->str.size(), h, err)) {
                        if (++decode_errors == 1 || decode_errors % 500 == 0)
                            fprintf(stderr, "[s%d] pcm v4: %s (%lu consecutive)\n", st.index, err.c_str(), decode_errors);
                        break;
                    }
                    decode_errors = 0;
                    if (h.channels != 2) {
                        fprintf(stderr, "[s%d] error: expected I/Q (2 channels), got %d\n", st.index, h.channels);
                        break;
                    }
                    if (v4.samples() && h.sampleCount > 0) sink.feed(v4.samples(), size_t(h.sampleCount), h.sampleRate);
                    break;
                }
                default:
                    break;
                }
            });

            ws.start();
            int ping = 0;
            while (!done && g_running && !sink.failed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (++ping >= 300 && st.connected) {  // 30 s keepalive
                    ping = 0;
                    ws.sendText("{\"type\":\"ping\"}");
                }
            }
            ws.stop();
            st.connected = false;
            if (sink.failed) break;  // unusable sample rate: retrying won't help
            set_msg("reconnecting");
        }
        if (!g_running) break;
        fprintf(stderr, "[s%d] reconnecting in %ds\n", st.index, backoff);
        for (int i = 0; i < backoff * 10 && g_running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backoff = std::min(backoff * 2, 60);
    }
    if (dump) fclose(dump);
}

// ---------------------------------------------------------------------------
// Offline: raw IQ file (single stream)
// ---------------------------------------------------------------------------

void run_file(App &app)
{
    const Options &o = app.o;
    Stream &st = *app.streams[0];
    FILE *f = fopen(o.iq_file.c_str(), "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", o.iq_file.c_str()); return; }
    IqSink sink{app, st};
    const size_t chunk = size_t(o.file_rate / 50) * 2;  // 20 ms
    std::vector<int16_t> buf(chunk);
    st.connected = true;
    { std::lock_guard<std::mutex> lk(st.mu); st.status_msg = "reading " + o.iq_file; }
    auto t0 = std::chrono::steady_clock::now();
    size_t total = 0;
    while (g_running) {
        size_t n = fread(buf.data(), sizeof(int16_t), chunk, f) & ~size_t(1);
        if (n == 0) break;
        sink.feed(buf.data(), n, int(o.file_rate));
        if (sink.failed) break;
        total += n / 2;
        if (o.realtime)
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(1e6 * double(total) / o.file_rate)));
    }
    fclose(f);
    st.connected = false;
    std::lock_guard<std::mutex> lk(st.mu);
    st.status_msg = "end of file";
}

void print_summary(App &app)
{
    std::lock_guard<std::mutex> nlk(app.navaids_mu);
    for (auto &sp : app.streams) {
        std::lock_guard<std::mutex> lk(sp->mu);
        if (!sp->dec) continue;
        fprintf(stderr, "\n[s%d] %-10s %5s %5s %4s %5s  %-7s %-22s %s\n", sp->index, "freq_hz", "snr", "pitch", "wpm",
                "cost", "ident", "navaid", "text");
        for (const auto &c : sp->dec->snapshot()) {
            std::string t = c.text.size() > 40 ? c.text.substr(c.text.size() - 40) : c.text;
            std::string g = c.text_ggm.size() > 24 ? c.text_ggm.substr(c.text_ggm.size() - 24) : c.text_ggm;
            t += "  | ggm: " + g;
            bool exact = false;
            auto m = app.navaids.match(c.freq_hz, c.ident, exact);
            std::string nav = "-";
            if (m.nav) {
                char b[96];
                snprintf(b, sizeof b, "%s%s %s %.0fkm", m.nav->ident.c_str(), exact ? "" : "?",
                         m.nav->name.substr(0, 10).c_str(), m.dist_km);
                nav = b;
            }
            std::string id = c.ident.empty() ? "-" : c.ident + "x" + std::to_string(c.ident_count);
            fprintf(stderr, "     %-10.1f %5.1f %5.0f %4.0f %5.2f  %-7s %-22s %s\n", c.freq_hz, c.snr_db, c.pitch_hz,
                    c.speed_wpm, c.cost, id.c_str(), nav.c_str(), t.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------

std::string content_type(const std::string &name)
{
    auto ends = [&](const char *s) {
        size_t n = strlen(s);
        return name.size() >= n && name.compare(name.size() - n, n, s) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    return "application/octet-stream";
}

std::string url_decode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string query_param(const std::string &uri, const std::string &key)
{
    auto qp = uri.find('?');
    if (qp == std::string::npos) return "";
    for (const auto &kv : split(uri.substr(qp + 1), '&')) {
        auto eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : kv.substr(eq + 1);
    }
    return "";
}

std::unique_ptr<ix::HttpServer> start_web(App &app)
{
    auto web = std::make_unique<ix::HttpServer>(app.o.web_port, "0.0.0.0");
    web->setOnConnectionCallback(
        [&app](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
            // Behind the UberSDR addon proxy the prefix is stripped; the page
            // uses only relative URLs, so nothing needs injecting. Route on
            // the last path segment, which also shrugs off an unstripped one.
            std::string path = req->uri.substr(0, req->uri.find('?'));
            std::string leaf = path.substr(path.find_last_of('/') + 1);
            bool api = path.find("/api/") != std::string::npos;

            auto resp = std::make_shared<ix::HttpResponse>();
            resp->statusCode = 200;
            resp->headers["Access-Control-Allow-Origin"] = "*";
            resp->headers["Cache-Control"] = "no-cache";
            if (api && leaf == "status") {
                resp->headers["Content-Type"] = "application/json";
                resp->body = status_json(app);
            } else if (api && leaf == "spectrum") {
                resp->headers["Content-Type"] = "application/json";
                resp->body = spectrum_json(app);
            } else if (api && leaf == "heard") {
                resp->headers["Content-Type"] = "application/json";
                resp->body = heard_json(app);
            } else if (api && leaf == "decodes") {
                std::vector<DecodeEvent> recent;
                {
                    std::lock_guard<std::mutex> lk(app.decodes_mu);
                    recent.assign(app.decodes_recent.begin(), app.decodes_recent.end());
                }
                resp->headers["Content-Type"] = "application/json";
                resp->body = decodes_json(recent, true);
            } else if (api && leaf == "search") {
                resp->headers["Content-Type"] = "application/json";
                resp->body = search_json(app, url_decode(query_param(req->uri, "q")), 60);
            } else if (api && leaf == "navaids") {
                std::string mk = query_param(req->uri, "max_km");
                resp->headers["Content-Type"] = "application/json";
                resp->body = navaids_json(app, mk.empty() ? app.o.map_km : atof(mk.c_str()));
            } else {
                if (leaf.empty()) leaf = "index.html";
                // Flat static directory; refuse anything that is not a plain file name.
                bool ok = leaf.find("..") == std::string::npos &&
                          std::all_of(leaf.begin(), leaf.end(), [](char c) {
                              return isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_';
                          });
                std::ifstream f(app.o.web_static + "/" + leaf, std::ios::binary);
                if (!ok || !f) {
                    resp->statusCode = 404;
                    resp->body = "not found";
                } else {
                    std::stringstream ss;
                    ss << f.rdbuf();
                    resp->body = ss.str();
                    resp->headers["Content-Type"] = content_type(leaf);
                }
            }
            return resp;
        });
    // Browsers open a WebSocket on the same port. On connect they get the whole
    // current picture at once — status, spectrum, heard log and the recent
    // live copy — then live updates: decodes as they happen, status at 1 Hz,
    // spectrum at 0.5 Hz, heard log every 10 s. Nothing polls through the
    // proxy's rate limit, and nothing is accepted from the browser.
    web->setOnClientMessageCallback(
        [&app](std::shared_ptr<ix::ConnectionState>, ix::WebSocket &ws, const ix::WebSocketMessagePtr &m) {
            if (m->type != ix::WebSocketMessageType::Open) return;
            std::vector<DecodeEvent> recent;
            {
                std::lock_guard<std::mutex> lk(app.decodes_mu);
                recent.assign(app.decodes_recent.begin(), app.decodes_recent.end());
            }
            ws.sendText(status_json(app));
            ws.sendText(spectrum_json(app));
            ws.sendText(heard_json(app));
            ws.sendText(decodes_json(recent, true));
        });
    if (!web->listenAndStart()) {
        fprintf(stderr, "error: cannot listen on port %d\n", app.o.web_port);
        return nullptr;
    }
    fprintf(stderr, "web UI: http://localhost:%d/ (static: %s)\n", app.o.web_port, app.o.web_static.c_str());
    return web;
}

std::string default_navaids_path(const char *argv0)
{
    std::string self = argv0;
    auto slash = self.find_last_of('/');
    std::vector<std::string> tries = {
        (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/navaids.csv",
        "./navaids.csv",
        "/usr/local/share/ubersdr_ndb/navaids.csv",
    };
    for (const auto &p : tries)
        if (std::ifstream(p)) return p;
    return tries.back();
}

}  // namespace

int main(int argc, char **argv)
{
    App app;
    Options &o = app.o;
    if (!parse_args(argc, argv, o)) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });
    signal(SIGPIPE, SIG_IGN);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    ix::initNetSystem();

    if (o.navaids_path.empty()) o.navaids_path = default_navaids_path(argv[0]);
    if (app.navaids.load(o.navaids_path))
        fprintf(stderr, "navaids: %zu NDBs from %s\n", app.navaids.size(), o.navaids_path.c_str());
    else
        fprintf(stderr, "warning: no navaid database at %s — beacons will not be named\n", o.navaids_path.c_str());

    if (std::isfinite(o.rx_lat) && std::isfinite(o.rx_lon)) {
        app.rx.have_pos = true;
        app.rx.lat = o.rx_lat;
        app.rx.lon = o.rx_lon;
        app.navaids.set_receiver(o.rx_lat, o.rx_lon);
    }

    load_heard(app);

    for (size_t i = 0; i < o.streams.size(); ++i) {
        auto s = std::make_unique<Stream>();
        s->index = int(i);
        s->spec = o.streams[i];
        s->cfg = o.dec;
        // Each pinned frequency goes to the first stream that covers it. The
        // modes' sample rates are known up front, so this needs no stream data.
        s->cfg.pinned_hz.clear();
        app.streams.push_back(std::move(s));
    }
    for (double f : o.dec.pinned_hz) {
        bool placed = false;
        for (auto &s : app.streams) {
            double fs = s->spec.mode == "file" ? o.file_rate : atof(s->spec.mode.c_str() + 2) * 1000.0;
            if (std::fabs(f - s->spec.center_hz) < 0.45 * fs) {
                s->cfg.pinned_hz.push_back(f);
                placed = true;
                break;
            }
        }
        if (!placed) fprintf(stderr, "warning: --ndb %.0f is outside every stream — ignored\n", f);
    }

    std::unique_ptr<ix::HttpServer> web;
    if (o.web_port > 0) {
        web = start_web(app);
        if (!web) return 1;
    }

    // Housekeeping and push, every 250 ms: live decodes go out as they arrive;
    // status, spectrum and the heard log on slower multiples. The heard log is
    // maintained whether or not anyone is watching.
    std::thread pusher([&] {
        for (uint64_t tick = 1; g_running; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            auto fresh = take_decodes(app);
            if (tick % 4 == 0) update_heard(app);
            if (tick % 240 == 0) save_heard(app);
            if (!web) continue;
            auto clients = web->getClients();
            if (clients.empty()) continue;
            std::vector<std::string> out;
            if (!fresh.empty()) out.push_back(decodes_json(fresh, false));
            if (tick % 4 == 0) out.push_back(status_json(app));
            if (tick % 8 == 0) out.push_back(spectrum_json(app));
            if (tick % 40 == 0) out.push_back(heard_json(app));
            // A browser that stops reading (a suspended tab, a stalled
            // proxy) would otherwise have every push queued for it in
            // memory indefinitely. Skip it while its backlog is large; it
            // catches up from the next full status once it drains.
            constexpr size_t kMaxBacklog = 2 * 1024 * 1024;
            for (auto &c : clients) {
                if (c->bufferedAmount() > kMaxBacklog) continue;
                for (const auto &s : out) c->sendText(s);
            }
        }
    });

    if (!o.iq_file.empty()) {
        run_file(app);
        print_summary(app);
        if (o.realtime && web) {
            fprintf(stderr, "file done; web UI still up, Ctrl-C to exit\n");
            while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        g_running = false;
    } else {
        std::vector<std::thread> clients;
        for (auto &s : app.streams) clients.emplace_back(run_client, std::ref(app), std::ref(*s));
        // The beacon table is a debugging aid: a line per carrier, every
        // interval, drowns the container log. Off unless asked for.
        int n = 0;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (o.summary_every > 0 && ++n % o.summary_every == 0) print_summary(app);
        }
        for (auto &t : clients) t.join();
    }

    if (pusher.joinable()) pusher.join();
    update_heard(app);
    save_heard(app);
    if (web) web->stop();
    ix::uninitNetSystem();
    curl_global_cleanup();
    return 0;
}
