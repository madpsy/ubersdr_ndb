// slot_rotation_test — a carrier recycled for showing no keying waits out its
// cooldown before it gets a slot back, free slot or not.
//
// One slot, three unkeyed carriers in synthetic IQ: A strongest, B, then C.
// A has the slot first; after its trial it is recycled for B. B then goes off
// air and is dropped, freeing the slot while A is still cooling down. The
// slot must go to C, which has waited all along, not straight back to A.
// (On M9PSY a strong unkeyed carrier at 378 kHz took every slot a revisit
// freed, so the waiting queue never rotated.)
//
//   slot_rotation_test        exit 0 on pass

#include "ndb_decoder.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

int main()
{
    const double fs = 48000.0, centre = 350000.0;
    const double a_hz = -12000.0, b_hz = -4000.0, c_hz = 8000.0;
    const double b_off_at = 70.0, check_at = 140.0;

    ndb::DecoderConfig cfg;
    cfg.max_channels = 1;
    cfg.trial_s = 60.0;
    cfg.drop_after_s = 10.0;   // B (slot ~64 s, off at 70 s) is dropped well inside its trial
    cfg.cooldown_s = 600.0;
    cfg.ggmorse = ndb::DecoderConfig::Ggmorse::Off;
    ndb::NdbDecoder dec(centre, fs, cfg);

    std::mt19937 rng(1);
    std::normal_distribution<float> gauss(0.f, 1.f);
    const size_t block = 4800;   // 0.1 s
    std::vector<int16_t> iq(2 * block);
    double ph[3] = {0, 0, 0};
    const double hz[3] = {a_hz, b_hz, c_hz};
    const float amp[3] = {4000.f, 1500.f, 600.f};
    const float noise = 30.f;
    size_t n = 0;
    double a_first = -1, a_again = -1, c_got = -1;
    bool a_left = false;

    while (n / fs < check_at) {
        const double t = n / fs;
        for (size_t i = 0; i < block; ++i) {
            float re = noise * gauss(rng), im = noise * gauss(rng);
            for (int k = 0; k < 3; ++k) {
                if (k == 1 && t >= b_off_at) continue;
                re += amp[k] * float(std::cos(ph[k]));
                im += amp[k] * float(std::sin(ph[k]));
                ph[k] = std::fmod(ph[k] + 2 * M_PI * hz[k] / fs, 2 * M_PI);
            }
            iq[2 * i] = int16_t(std::lround(re));
            iq[2 * i + 1] = int16_t(std::lround(im));
        }
        dec.process_iq(iq.data(), block);
        n += block;

        bool a_now = false;
        for (const auto &s : dec.snapshot()) {
            const double off = s.freq_hz - centre;
            if (std::fabs(off - a_hz) < 50) a_now = true;
            if (std::fabs(off - c_hz) < 50 && c_got < 0) c_got = dec.stream_time();
        }
        if (a_now && a_first < 0) a_first = dec.stream_time();
        if (!a_now && a_first >= 0) a_left = true;
        if (a_now && a_left && a_again < 0) a_again = dec.stream_time();
    }

    printf("A first slot %.1f s, recycled: %s, back in slot %.1f s; C slot %.1f s\n", a_first, a_left ? "yes" : "no",
           a_again, c_got);
    bool ok = true;
    if (a_first < 0 || !a_left) { printf("FAIL: setup: A never had the slot and gave it up\n"); ok = false; }
    if (a_again >= 0) { printf("FAIL: A took the freed slot back during its cooldown\n"); ok = false; }
    if (c_got < 0) { printf("FAIL: C, waiting all along, never got the freed slot\n"); ok = false; }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
