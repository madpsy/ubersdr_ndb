// ndb_channel.h — one NDB: tune, demodulate, decode the Morse ident.
//
// Signal path, per channel:
//
//   wideband IQ (fs) ──► rotator: carrier → DC
//                    ──► FIR ↓D1 to 16 kHz        (coarse; kills far-off signals)
//                    ──► FIR ↓4  to  4 kHz        (±1.3 kHz channel)
//                    ──► |z|  AM envelope, normalised by the carrier level
//                    ──┬► KeyingDecoder (NDB-specific; the copy shown)  ─┐
//                      │    └► FoldDecoder (the ident averaged over       ─┤
//                      │        a minute of repeats; weak beacons)          ├► ident tally
//                      └► ggmorse (general CW decoder; second opinion)   ─┘
//
// 4 kHz is ggmorse's internal base rate, so it is fed with no resampling. The
// AM detector turns an A2A NDB (continuous carrier, ident keyed as a 400 or
// 1020 Hz tone) into exactly what ggmorse expects: a keyed audio tone.
//
// An A1A NDB (keyed carrier, no tone) would demodulate to a keyed DC level
// instead. That is not handled yet — see TODO in ndb_channel.cpp.

#pragma once

#include "dsp.h"
#include "fold_decoder.h"
#include "keying_decoder.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class GGMorse;

namespace ndb {

constexpr double kAudioRate = 4000.0;  // == GGMorse::kBaseSampleRate

struct ChannelSnapshot {
    int         id = 0;
    double      freq_hz = 0.0;        // absolute
    double      offset_hz = 0.0;      // from IQ centre
    float       snr_db = 0.0f;        // from the wideband detector
    float       carrier_db = 0.0f;    // carrier level at the channel output
    float       pitch_hz = 0.0f;      // ggmorse estimate (the ident tone)
    float       speed_wpm = 0.0f;     // ggmorse estimate
    float       cost = 1.0f;          // ggmorse cost of the last decode (lower = better)
    float       contrast_db = 0.0f;   // tone on/off contrast (KeyingDecoder)
    int         window_ms = 0;        // tone envelope window the copy is decoded with
    float       periodicity = 0.0f;   // strength of the ident cycle, last minute (FoldDecoder; noise ~0.05)
    float       cycle_s = 0.0f;       // its length
    bool        ggmorse = false;      // a ggmorse instance is attached right now
    bool        keying = false;       // KeyingDecoder sees on/off keying
    std::string ident;                // most frequent repeated token, or ""
    int         ident_count = 0;      // how many times it was seen
    std::string text;                 // recent copy (KeyingDecoder)
    std::string text_ggm;             // recent copy (ggmorse), for comparison
    std::string text_fold;            // copies from the repeat-averaged ident (FoldDecoder)
    bool        pinned = false;       // requested on the command line; never dropped
    double      age_s = 0.0;          // since the channel was created
    double      last_seen_s = 0.0;    // since the detector last saw the carrier
    double      last_text_s = -1.0;   // since ggmorse last produced text (-1 = never)
    // Identified beacons give their channel up and are tracked by carrier
    // (see NdbDecoder); these say how current the ident shown is.
    bool        tracking = false;     // no channel now: carrier watched, ident remembered
    bool        visit = false;        // a channel reopened to re-copy a remembered ident
    double      ident_age_s = -1.0;   // since the ident was last copied (-1 = no ident)
    bool        ident_fresh = true;   // copied recently enough to count as heard now
};

class NdbChannel {
public:
    // use_ggmorse: also run ggmorse as a second decoder feeding the ident
    // tally. It is ~90% of the CPU of a channel, so it is optional.
    NdbChannel(int id, double center_hz, double offset_hz, double fs, bool pinned, double now_s,
               bool use_ggmorse = false);
    ~NdbChannel();

    NdbChannel(const NdbChannel &) = delete;
    NdbChannel &operator=(const NdbChannel &) = delete;

    // now_s is stream time (seconds of IQ consumed), used for all ages below.
    void process(const cf *x, size_t n, double now_s);

    // Carrier moved (drift, or a better estimate): retune, phase-continuously.
    void retune(double offset_hz);

    // Attach or detach ggmorse. Attaching starts it on audio from now on;
    // detaching frees it (its copy so far stays in the tally).
    void set_ggmorse(bool on);
    bool ggmorse() const { return ggm_ != nullptr; }

    // Pin ggmorse's pitch and/or speed (<= 0 leaves that one on auto).
    void lock(float pitch_hz, float speed_wpm);

    // Called by the manager on each detection pass that finds this carrier.
    void seen(float snr_db, double now_s);

    // Stream time this channel last showed tone keying (or copied text);
    // -1 if never. The decoder recycles channels that have shown none.
    double last_active_s() const { return last_active_; }
    double created_s() const { return created_; }

    int    id() const { return id_; }
    double offset_hz() const { return rot_.freq(); }
    bool   pinned() const { return pinned_; }
    double last_seen_s() const { return last_seen_; }

    ChannelSnapshot snapshot(double now_s) const;

    // Copies of this exact token in the tally.
    int copies(const std::string &token, double now_s) const;

    // Optional tap on the 4 kHz audio exactly as ggmorse receives it (for
    // debugging, and a future listen-in feature).
    std::function<void(const float *, size_t)> audio_tap;

    // Called with each chunk of text as ggmorse produces it (for the live
    // decode feed). Runs on the IQ thread, under the caller's stream lock.
    std::function<void(const std::string &)> on_decode;

private:
    void on_audio(float a);
    void run_morse();
    enum class Source { Keying, Ggmorse, Fold };
    void on_text(const std::string &s, double now_s, Source src);
    void end_token(std::string &token, double now_s);

    int id_;
    double center_hz_;
    double fs_;
    bool pinned_;

    Rotator rot_;
    Decimator dec1_, dec2_;

    // AM detector state.
    float dc_ = 0.0f;           // carrier magnitude, slow average
    float dc_alpha_;            // ~1 s time constant at the audio rate
    HighPass hp1_, hp2_;        // 4th-order high-pass ahead of ggmorse

    // ggmorse and the audio queued for it.
    std::unique_ptr<GGMorse> ggm_;
    std::vector<float> audio_;       // this call's audio, for the keying decoder
    std::vector<float> ggm_audio_;   // band-passed audio queued for ggmorse
    size_t ggm_read_ = 0;
    // ggmorse is handed what the keying decoder has learned: audio narrowed
    // to the tone, and the pitch (and speed, when known) fixed, so it spends
    // its effort on the one thing left to decide.
    BandPass bp1_, bp2_;
    float bp_pitch_ = 0.0f;
    float ggm_wpm_ = 0.0f;
    double last_tune_ = -1e9;
    void tune_ggmorse();

    KeyingDecoder key_;
    FoldDecoder fold_;

    // Decoded text and ident tally. Each decoder builds its own tokens; both
    // feed the one tally.
    std::string text_, text_ggm_, text_fold_;
    std::string token_key_, token_ggm_, token_fold_;
    struct Tok { std::string s; double t; };
    std::deque<Tok> tokens_;    // recent candidate ident tokens

    float  snr_db_ = 0.0f;
    float  cost_ = 1.0f;
    double created_ = 0.0;
    double last_seen_ = 0.0;
    double last_text_ = -1.0;
    double last_active_ = -1.0;
    double now_ = 0.0;          // stream time of the last process() call
};

}  // namespace ndb
