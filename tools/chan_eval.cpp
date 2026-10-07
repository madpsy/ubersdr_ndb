// chan_eval — decode chosen beacons from a raw IQ file, for comparing decoder
// changes: one channel per frequency (keying and fold decoders; ggmorse only
// with CHAN_GGMORSE=1), each beacon's copy and its ident tally at the end.
//
//   chan_eval capture.iq 192000 356000 341000:EDN 380000:CBL ...

#include "channelizer.h"
#include "ndb_channel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s iq rate centre hz[:IDENT] ...\n", argv[0]); return 2; }
    const double fs = atof(argv[2]), centre = atof(argv[3]);
    const bool ggm = getenv("CHAN_GGMORSE") && atoi(getenv("CHAN_GGMORSE"));
    // CHAN_NOISE: white noise added to the IQ, rms per component in int16
    // units, so strong beacons can be made weak at a known level.
    const float noise = getenv("CHAN_NOISE") ? float(atof(getenv("CHAN_NOISE"))) / 32768.f : 0.f;
    std::mt19937 rng(1);
    std::normal_distribution<float> gauss(0.f, 1.f);
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror("open"); return 1; }
    ndb::Channelizer bank(fs);
    struct Beacon { double hz; std::string ident; std::unique_ptr<ndb::NdbChannel> ch; std::string copy, fold; };
    std::vector<Beacon> bs;
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        auto c = a.find(':');
        Beacon b;
        b.hz = atof(a.substr(0, c).c_str());
        if (c != std::string::npos) b.ident = a.substr(c + 1);
        b.ch = std::make_unique<ndb::NdbChannel>(int(i), centre, b.hz - centre, bank, true, 0.0, ggm);
        bs.push_back(std::move(b));
    }
    std::vector<int16_t> raw(size_t(fs / 50) * 2);
    std::vector<ndb::cf> x;
    double t = 0;
    while (size_t n = fread(raw.data(), 2, raw.size(), in) / 2) {
        x.resize(n);
        for (size_t i = 0; i < n; ++i) {
            x[i] = ndb::cf(raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f);
            if (noise > 0) {
                const float im = gauss(rng);
                x[i] += ndb::cf(noise * gauss(rng), noise * im);
            }
        }
        t += n / fs;
        bank.process(x.data(), n);
        for (auto &b : bs) b.ch->process(bank, t);
    }
    int total_ok = 0, total_bad = 0;
    for (auto &b : bs) {
        auto s = b.ch->snapshot(t);
        // Tokens in the copy: right ident, and anything else 2-4 letters long.
        int ok = 0, bad = 0;
        std::map<std::string, int> others;
        for (const std::string *txt : {&s.text, &s.text_fold}) {
            std::istringstream ss(*txt);
            std::string w;
            while (ss >> w) {
                if (w == b.ident) ++ok;
                else if (w.size() >= 2 && w.size() <= 4 && w.find('?') == std::string::npos) { ++bad; others[w]++; }
            }
        }
        total_ok += ok;
        total_bad += bad;
        printf("%.1f %-4s ok %2d bad %2d  tally %s x%d  pitch %.0f contrast %.1f wpm %.1f win %d | %s | fold: %s",
               b.hz / 1e3, b.ident.c_str(), ok, bad, s.ident.empty() ? "-" : s.ident.c_str(), s.ident_count,
               s.pitch_hz, s.contrast_db, s.speed_wpm, s.window_ms, s.text.substr(s.text.size() > 120 ? s.text.size() - 120 : 0).c_str(),
               s.text_fold.c_str());
        if (ggm) printf(" | ggm: %s", s.text_ggm.c_str());
        printf("\n      other:");
        for (auto &[w, c] : others) printf(" %s%s", w.c_str(), c > 1 ? ("x" + std::to_string(c)).c_str() : "");
        printf("\n");
    }
    printf("TOTAL ok %d bad %d\n", total_ok, total_bad);
}
