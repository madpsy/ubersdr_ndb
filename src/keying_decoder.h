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
//   - the tone: the strongest line 300-1200 Hz in a long averaged spectrum;
//   - the tone envelope, 10 ms resolution, from mixing the audio to DC at the
//     tone and integrating over 10 ms;
//   - the on/off threshold: midway between the 20th and 97th percentiles of
//     the last 20 s of envelope, with hysteresis;
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
// Noise must produce nothing. Letters are only emitted while the tone's on/off
// contrast is at least 15 dB (noise alone reaches ~12) and the timing looks
// like machine keying: a dit
// of 50-250 ms (5-24 wpm), and most recent marks within tolerance of either
// one or three dits. Noise "marks" are short and scattered; an NDB's cluster
// tightly at exactly those two lengths.

#pragma once

#include "dsp.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace ndb {

class KeyingDecoder {
public:
    explicit KeyingDecoder(double audio_rate);

    void process(const float *a, size_t n);

    // Decoded characters (and ' ' at word gaps), as they complete.
    std::function<void(const std::string &)> on_text;

    float pitch_hz() const { return float(pitch_); }
    float wpm() const { return dit_ms_ > 0 ? float(1200.0 / dit_ms_) : 0.0f; }
    float contrast_db() const { return contrast_db_; }
    float timing_quality() const { return quality_; }  // share of recent marks that are clean dits/dahs
    bool  keying() const { return keying_ && dit_ms_ > 0 && quality_ >= kMinQuality; }

    static constexpr float kMinQuality = 0.75f;

private:
    void update_pitch();
    void on_envelope(float e);
    void refresh_threshold();
    void end_mark(double ms);
    void end_space(double ms);
    void flush_letter(bool word);

    double fs_;
    // Pitch search: averaged power spectrum of the audio.
    size_t nfft_;
    FFT fft_;
    std::vector<cf> fbuf_;
    std::vector<float> spec_;
    size_t ffill_ = 0;
    int spectra_ = 0;
    double pitch_ = 0.0;

    // Tone envelope.
    double ph_ = 0.0;
    cf acc_{0.0f, 0.0f};
    int acc_n_ = 0;
    int block_;             // audio samples per envelope sample (10 ms)
    float prev_env_ = 0.0f;

    // Threshold from the recent envelope history.
    std::deque<float> hist_;
    int since_refresh_ = 0;
    float thr_ = 0.0f, hyst_ = 0.0f;
    float contrast_db_ = 0.0f;
    bool keying_ = false;

    // Keying state.
    bool on_ = false;
    int run_ = 0;           // envelope samples in the current state
    int pending_ = 0;       // consecutive samples disagreeing with on_ (debounce)
    std::deque<double> marks_;
    double dit_ms_ = 0.0;
    float quality_ = 0.0f;
    std::string code_;
};

}  // namespace ndb
