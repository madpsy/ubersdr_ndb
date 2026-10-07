// keying_decoder.h — a Morse decoder built for how NDBs actually key.
//
// ggmorse is a general CW decoder: it re-estimates the threshold and the
// speed over a rolling 3 s window, because a human operator's fist and level
// drift. An NDB is the opposite case — a machine sending the same two or three
// letters at a fixed speed and level, forever, with a long silence (2-8 s)
// between repetitions. In ggmorse's 3 s window that silence dominates, its
// estimates wander in every gap, and a beacon with a short ident and a long
// gap (UW on 368 kHz: 1.5 s of keying, then 6 s of nothing) never copies,
// even at 56 dB SNR.
//
// This decoder instead learns the things that do not change, over a long
// enough span to include both keying and silence:
//
//   - the tone: of the three strongest lines 300-1200 Hz in a long averaged
//     spectrum, the one that is keyed. Each is mixed to DC and its on/off
//     contrast measured; a steady line (a beat against a neighbouring carrier,
//     a spur) has almost none. GLW on 331 kHz sits 1.1 kHz from another
//     carrier, and their steady 1.1 kHz beat is stronger than its 400 Hz tone;
//   - the tone envelope: the audio mixed to DC at the tone, summed coherently
//     over 10 ms blocks, then over a sliding window of W blocks;
//   - the on/off threshold: from the 20th and 97th percentiles of the last
//     20 s of envelope, on at 50% of the way between them and off at 40%;
//   - the dit length: the short cluster of the last 60 mark lengths, split
//     where dah/dit is 2-4.5.
//
// Classification is then textbook: mark < 2 dits is a dit, else a dah; a
// space of 2-5 dits ends a letter, 5+ ends a word (and an NDB's inter-ident
// gap is always a word gap).
//
// A fade or a noise spike inside a dah would split it in two, so a change of
// state only counts once it has lasted ~30% of a dit (debounce); the run
// lengths are then measured from where the change began.
//
// The envelope window sets the noise bandwidth: 20 ms is ~50 Hz, which a
// 24 wpm beacon (50 ms dit) needs, but a 10 wpm one (120 ms dit) can take
// 60 ms, 3x less noise. The best window depends on the speed, which is not
// known until keying has been decoded, so the decoder runs as a few lanes, one
// per window (20/40/60/80 ms), each learning its own threshold, dit and
// quality. They are cheap: all share the tone mixing, and work at 100 Hz. The
// copy comes from the widest lane whose window is at most 0.6 of its dit, and
// switches lanes only between words. Coherent integration does not raise the
// noise's own contrast (noise stays Rayleigh at any window), so one contrast
// gate serves every lane.
//
// Noise must produce nothing. Letters are only emitted while the tone's on/off
// contrast is at least 15 dB (noise alone reaches ~12) and the timing looks
// like machine keying: a dit of 50-250 ms (5-24 wpm), and most recent marks
// within tolerance of either one or three dits. Noise "marks" are short and
// scattered; an NDB's cluster tightly at exactly those two lengths.
//
// The 20 ms lane's envelope is also passed out (on_envelope) for the fold
// decoder, which averages it over many ident repetitions.

#pragma once

#include "dsp.h"
#include "fft.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ndb {

// The means of two clusters of positive numbers, split by their logarithms,
// or false when they do not form two (long/short under 1.8). From
// ubersdr-skimmer (playground_cw.cpp).
bool two_clusters(const std::vector<double> &xs, double &lo, double &hi);

// International Morse: dots and dashes ("-.-.") to the letter or digit.
const std::map<std::string, char> &morse_table();

class KeyingDecoder {
public:
    explicit KeyingDecoder(double audio_rate);

    void process(const float *a, size_t n);

    // Decoded characters (and ' ' at word gaps), as they complete.
    std::function<void(const std::string &)> on_text;

    // Every 10 ms once the tone is known: the 20 ms tone envelope.
    std::function<void(float)> on_envelope;
    // The decoder moved to a different tone; the envelope before it is
    // unrelated to the envelope after.
    std::function<void()> on_retone;

    static constexpr double kEnvMs = 10.0;   // envelope resolution

    float pitch_hz() const { return float(pitch_); }
    float wpm() const { return lane().dit_ms > 0 ? float(1200.0 / lane().dit_ms) : 0.0f; }
    // Best on/off contrast of any lane (it is what says keying is present).
    float contrast_db() const;
    float timing_quality() const { return lane().quality; }  // share of recent marks that are clean dits/dahs
    bool  keying() const { return lane().keying(); }
    int   window_ms() const { return int(lane().w * kEnvMs); }  // envelope window of the lane copied

    static constexpr float kMinQuality = 0.75f;

private:
    struct Lane {
        int w = 2;              // envelope window, in 10 ms blocks

        // Threshold from the recent envelope history.
        std::deque<float> hist;
        int since_refresh = 0;
        float thr = 0.0f, hyst = 0.0f;
        float contrast_db = 0.0f;
        bool gate = false;      // contrast high enough to be keying

        // Keying state.
        bool on = false;
        int run = 0;            // envelope samples in the current state
        int pending = 0;        // consecutive samples disagreeing with on (debounce)
        std::deque<double> marks;
        double dit_ms = 0.0;
        float quality = 0.0f;
        std::string code;
        std::vector<double> letter;   // this letter's mark lengths, ms
        bool in_word = false;   // letters emitted since the last word gap

        bool keying() const { return gate && dit_ms > 0 && quality >= kMinQuality; }
        // Keying, at a speed this lane's window is short enough for.
        bool fits() const { return keying() && w * kEnvMs <= 0.6 * dit_ms; }
        bool idle() const { return !in_word && code.empty(); }
    };

    // A candidate tone: mixed to DC, and its keying contrast tracked.
    struct Tone {
        double hz = 0.0;
        cf ph{1.0f, 0.0f}, step{1.0f, 0.0f};   // mixer phasor
        cf acc{0.0f, 0.0f}, prev{0.0f, 0.0f};  // this and the last 10 ms block
        std::deque<float> hist;                // 20 ms envelope, last 20 s
        float contrast_db = 0.0f;
        void set(double f, double fs);
    };

    void update_pitch();
    void choose_tone();
    void retone(size_t i);
    void envelope_sample(cf block);
    void lane_sample(Lane &l, float e);
    void refresh_threshold(Lane &l);
    void end_mark(Lane &l, double ms);
    void end_space(Lane &l, double ms);
    void flush_letter(Lane &l, bool word);
    void choose_lane();
    const Lane &lane() const { return lanes_[sel_]; }

    double fs_;
    // Pitch search: averaged power spectrum of the audio.
    size_t nfft_;
    RealFft fft_;      // its input is filled with windowed audio as it comes
    std::vector<float> fwin_;
    std::vector<float> spec_;
    size_t ffill_ = 0;
    int spectra_ = 0;
    double pitch_ = 0.0;
    std::vector<Tone> tones_;
    size_t tone_ = 0;       // the one decoded
    int since_choose_ = 0;

    // Tone envelope: 10 ms complex block sums, the last few kept.
    int acc_n_ = 0;
    int block_;             // audio samples per 10 ms block
    static constexpr int kMaxW = 8;
    std::array<cf, kMaxW> blocks_{};
    int head_ = 0;          // where the next block goes
    int filled_ = 0;        // blocks held, up to kMaxW

    std::array<Lane, 4> lanes_;
    size_t sel_ = 0;        // lane the copy comes from
};

}  // namespace ndb
