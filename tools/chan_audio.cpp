// chan_audio — run one NdbChannel over a raw IQ file and write the 4 kHz
// audio ggmorse sees as a WAV, printing the decoded text as it goes.
//
//   chan_audio <iq file> <rate> <centre Hz> <beacon Hz> <out.wav>
#include "ndb_channel.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

static void wav_header(FILE *f, uint32_t n)
{
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(4000); u32(8000); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(n * 2);
}

int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: %s iq rate centre beacon out.wav\n", argv[0]); return 2; }
    const double fs = atof(argv[2]), centre = atof(argv[3]), beacon = atof(argv[4]);
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[5], "wb");
    if (!in || !out) { perror("open"); return 1; }
    std::vector<int16_t> pcm;
    ndb::NdbChannel ch(1, centre, beacon - centre, fs, true, 0.0, true);
    // CHAN_PITCH / CHAN_WPM pin ggmorse, to separate pitch/speed-search
    // failures from everything upstream of it.
    const char *lp = getenv("CHAN_PITCH"), *lw = getenv("CHAN_WPM");
    if (lp || lw) ch.lock(lp ? float(atof(lp)) : 0.f, lw ? float(atof(lw)) : 0.f);
    ch.audio_tap = [&](const float *a, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            float v = a[i] * 16384.0f;
            pcm.push_back(int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
        }
    };
    std::vector<int16_t> raw(size_t(fs / 50) * 2);
    std::vector<ndb::cf> x;
    double t = 0;
    size_t shown = 0;
    while (size_t n = fread(raw.data(), 2, raw.size(), in) / 2) {
        x.resize(n);
        for (size_t i = 0; i < n; ++i) x[i] = ndb::cf(raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f);
        t += n / fs;
        ch.process(x.data(), n, t);
        auto s = ch.snapshot(t);
        if (s.text.size() != shown) {
            fprintf(stderr, "%7.1fs pitch %4.0f wpm %4.1f | %s\n", t, s.pitch_hz, s.speed_wpm, s.text.c_str());
            shown = s.text.size();
        }
    }
    wav_header(out, uint32_t(pcm.size()));
    fwrite(pcm.data(), 2, pcm.size(), out);
    fclose(out);
    return 0;
}
