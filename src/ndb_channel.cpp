#include "ndb_channel.h"

#include "ggmorse/ggmorse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace ndb {

namespace {

constexpr double kStage1Rate  = 16000.0;
constexpr double kPassHz      = 1300.0;   // channel half-bandwidth kept
constexpr size_t kTextMax     = 400;      // decoded text kept per channel
// The ident tally looks back an hour. A strong beacon is identified within a
// minute either way; the long window is for weak ones, which may give one
// clean copy every few minutes and need those copies to accumulate.
constexpr double kTokenWindow = 3600.0;
constexpr size_t kTokenMax    = 1024;
constexpr size_t kTokenCharsMax = 16;   // longest run of letters kept between word gaps

// NDB idents are machine-keyed slowly — typically 6-12 wpm. Bounding ggmorse's
// speed search to this range is what lets it lock onto them at all: see
// third_party/ggmorse/README.ubersdr_ndb.md.
constexpr float kSpeedMinWpm  = 5.0f;
constexpr float kSpeedMaxWpm  = 20.0f;

}  // namespace

NdbChannel::NdbChannel(int id, double center_hz, double offset_hz, double fs, bool pinned, double now_s,
                       bool use_ggmorse)
    : id_(id), center_hz_(center_hz), fs_(fs), pinned_(pinned),
      key_(kAudioRate), created_(now_s), last_seen_(now_s), now_(now_s)
{
    key_.on_text = [this](const std::string &s) { on_text(s, now_, Source::Keying); };
    rot_.set(offset_hz, fs);

    // Stage 1: fs → 16 kHz. Only needs to protect ±kPassHz from what folds
    // onto it, so the transition band is nearly the whole 16 kHz: short filter.
    const int d1 = int(std::lround(fs / kStage1Rate));
    const double stop1 = kStage1Rate - kPassHz;
    dec1_ = Decimator(design_lowpass(taps_for(stop1 - kPassHz, fs), 0.5 * (kPassHz + stop1), fs), d1);

    // Stage 2: 16 kHz → 4 kHz. Anything above 4000-1300 = 2700 Hz would fold
    // back into ±1300, so that is the stopband edge.
    const double stop2 = kAudioRate - kPassHz;
    dec2_ = Decimator(design_lowpass(taps_for(stop2 - kPassHz, kStage1Rate), 0.5 * (kPassHz + stop2), kStage1Rate),
                      int(kStage1Rate / kAudioRate));

    dc_alpha_ = float(1.0 - std::exp(-1.0 / (1.0 * kAudioRate)));

    // Some NDBs dip or lift the carrier slightly while the ident tone is on.
    // After the AM detector that is a keyed low-frequency step, and on a
    // beacon with shallow tone modulation the step can be as large as the
    // tone itself. ggmorse's own first-order high-pass barely touches it, so
    // take everything below the lowest NDB tone out properly here.
    hp1_.set(250.0, kAudioRate);
    hp2_.set(250.0, kAudioRate);

    if (use_ggmorse) set_ggmorse(true);
}

void NdbChannel::set_ggmorse(bool on)
{
    if (!on) {
        ggm_.reset();
        ggm_audio_.clear();
        ggm_read_ = 0;
        token_ggm_.clear();
        bp_pitch_ = 0.0f;
        ggm_wpm_ = 0.0f;
        return;
    }
    if (ggm_) return;
    last_tune_ = -1e9;
    GGMorse::Parameters p = GGMorse::getDefaultParameters();
    p.sampleRateInp   = float(kAudioRate);
    p.sampleRateOut   = float(kAudioRate);
    p.samplesPerFrame = GGMorse::kDefaultSamplesPerFrame;
    p.sampleFormatInp = GGMORSE_SAMPLE_FORMAT_F32;
    p.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_F32;
    ggm_ = std::make_unique<GGMorse>(p);

    // NDB tones are nominally 400 or 1020 Hz. Keep ggmorse's pitch search to a
    // band around those rather than its 200-1200 default, which reaches down
    // into the hum and low-frequency junk an AM detector passes.
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = -1.0f;   // auto
    dp.speed_wpm    = -1.0f;   // auto
    dp.frequencyRangeMin_hz = 300.0f;
    dp.frequencyRangeMax_hz = 1200.0f;
    dp.speedRangeMin_wpm = kSpeedMinWpm;
    dp.speedRangeMax_wpm = kSpeedMaxWpm;
    ggm_->setParametersDecode(dp);
}

NdbChannel::~NdbChannel() = default;

void NdbChannel::retune(double offset_hz)
{
    rot_.set(offset_hz, fs_);
}

void NdbChannel::lock(float pitch_hz, float speed_wpm)
{
    if (!ggm_) return;
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = pitch_hz > 0 ? pitch_hz : -1.0f;
    dp.speed_wpm    = speed_wpm > 0 ? speed_wpm : -1.0f;
    dp.frequencyRangeMin_hz = pitch_hz > 0 ? std::max(200.0f, pitch_hz - 150.0f) : 300.0f;
    dp.frequencyRangeMax_hz = pitch_hz > 0 ? std::min(1900.0f, pitch_hz + 150.0f) : 1200.0f;
    dp.speedRangeMin_wpm = kSpeedMinWpm;
    dp.speedRangeMax_wpm = kSpeedMaxWpm;
    ggm_->setParametersDecode(dp);
}

void NdbChannel::seen(float snr_db, double now_s)
{
    snr_db_ = snr_db;
    last_seen_ = now_s;
}

void NdbChannel::process(const cf *x, size_t n, double now_s)
{
    now_ = now_s;
    cf y1, y2;
    for (size_t i = 0; i < n; ++i) {
        if (!dec1_.push(rot_.mix(x[i]), y1)) continue;
        if (!dec2_.push(y1, y2)) continue;

        // AM envelope, normalised by the carrier so every channel reaches
        // ggmorse at the same scale regardless of signal strength: with the
        // carrier at 1.0, a fully modulated tone swings the audio ±1.
        //
        // TODO(A1A): a keyed-carrier NDB has no tone, so this yields a keyed
        // DC step that ggmorse cannot see. Detect that case (dc_ itself keying
        // on and off) and switch to a BFO — mix the carrier to ~800 Hz instead
        // of DC and feed Re(z) — so the keyed carrier becomes a keyed tone.
        const float m = std::abs(y2);
        dc_ += dc_alpha_ * (m - dc_);
        const float a = dc_ > 1e-9f ? (m - dc_) / dc_ : 0.0f;
        on_audio(std::clamp(a, -2.0f, 2.0f));
    }
    run_morse();
    if (key_.keying() || (last_text_ >= 0 && now_s - last_text_ < 1.0)) last_active_ = now_s;
}

void NdbChannel::on_audio(float a)
{
    audio_.push_back(0.5f * hp2_.process(hp1_.process(a)));
}


// Point ggmorse at what the keying decoder has learned. Called every few
// seconds while attached: the tone estimate settles over the first spectra.
void NdbChannel::tune_ggmorse()
{
    const float pitch = key_.pitch_hz();
    if (pitch <= 0.0f) return;
    const float wpm = key_.wpm();   // > 0 once a dit length is estimated
    if (std::fabs(pitch - bp_pitch_) > 3.0f) {
        // ±75 Hz: wide enough for a slightly drifting tone and its keying
        // sidebands at ~10 wpm, narrow enough to shed most of the noise.
        bp1_.set(pitch, 150.0, kAudioRate);
        bp2_.set(pitch, 150.0, kAudioRate);
        bp_pitch_ = pitch;
    } else if (std::fabs(wpm - ggm_wpm_) < 0.5f) {
        return;   // nothing changed
    }
    ggm_wpm_ = wpm;
    lock(pitch, wpm);
}

void NdbChannel::run_morse()
{
    key_.process(audio_.data(), audio_.size());
    if (audio_tap) audio_tap(audio_.data(), audio_.size());
    if (!ggm_) {
        audio_.clear();
        return;
    }
    if (now_ - last_tune_ >= 5.0) {
        last_tune_ = now_;
        tune_ggmorse();
    }
    // Until the tone is known, ggmorse gets the full audio and searches for it.
    for (float a : audio_) ggm_audio_.push_back(bp_pitch_ > 0.0f ? bp2_.process(bp1_.process(a)) : a);
    audio_.clear();

    // ggmorse pulls audio through a callback, asking for exactly one frame at
    // a time and stopping when the callback returns 0, so hand it whatever is
    // queued in whole frames and keep the remainder for next time.
    ggm_->decode([this](void *data, uint32_t n_bytes) -> uint32_t {
        const size_t need = n_bytes / sizeof(float);
        if (ggm_audio_.size() - ggm_read_ < need) return 0;
        std::memcpy(data, ggm_audio_.data() + ggm_read_, need * sizeof(float));
        ggm_read_ += need;
        return n_bytes;
    });
    if (ggm_read_ > 0) {
        ggm_audio_.erase(ggm_audio_.begin(), ggm_audio_.begin() + long(ggm_read_));
        ggm_read_ = 0;
    }

    // ggmorse appends to these on every frame for plotting and only empties
    // them when taken; left alone they grow ~10 MB a day per channel.
    GGMorse::ThresholdF th;
    ggm_->takeThresholdF(th);
    GGMorse::SignalF sig;
    ggm_->takeSignalF(sig);

    GGMorse::TxRx rx;
    if (ggm_->takeRxData(rx) > 0) {
        cost_ = ggm_->getStatistics().costFunction;
        on_text(std::string(rx.begin(), rx.end()), now_, Source::Ggmorse);
    }
}

void NdbChannel::on_text(const std::string &s, double now_s, Source src)
{
    const bool primary = src == Source::Keying;
    std::string &text = primary ? text_ : text_ggm_;
    std::string &token = primary ? token_key_ : token_ggm_;
    for (char c : s) {
        // ggmorse emits '\n' when it re-acquires on a new pitch: a break, so
        // treat it as a word gap.
        if (c == '\n' || c == '\r') c = ' ';
        // Morse is ASCII, but ggmorse's alphabet includes a few non-ASCII
        // letters and noise can decode as one. A lone high byte is invalid
        // UTF-8, and IXWebSocket closes a socket rather than send it — which
        // took down every browser connection once one got into the copy.
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
        if (c == ' ') {
            if (!text.empty() && text.back() != ' ') text += ' ';
            end_token(token, now_s);
            continue;
        }
        text += c;
        // No ident is longer than a few letters; a run this long is noise
        // that never produced a word gap, and must not grow without bound.
        if (token.size() < kTokenCharsMax) token += char(std::toupper((unsigned char)c));
        else token = "?";
    }
    if (text.size() > kTextMax) text.erase(0, text.size() - kTextMax);
    // The live feed and "last copied" follow the copy that is shown.
    if (!s.empty() && primary) {
        last_text_ = now_s;
        if (on_decode) {
            std::string clean = s;
            for (char &c : clean) {
                if (c == '\n' || c == '\r') c = ' ';
                else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
            }
            on_decode(clean);
        }
    }
}

// NDB idents are two or three characters (occasionally one or four), sent
// over and over with a long gap between. Decoded noise is scattered single
// letters and nonsense; a real ident is the same short token recurring. So
// the ident is simply the most frequent 2-4 character clean token seen in the
// last few minutes, and it needs to have been seen at least twice.
//
// If the gaps between repetitions were too short for ggmorse to call a word
// break, the token arrives run together ("ABCABCABC"); a token made of a 2-4
// character unit repeated whole is counted as that unit.
void NdbChannel::end_token(std::string &token, double now_s)
{
    std::string t;
    t.swap(token);
    if (t.empty() || t.find('?') != std::string::npos) return;
    for (char c : t)
        if (!std::isalnum((unsigned char)c)) return;

    auto push = [&](const std::string &s) {
        tokens_.push_back({s, now_s});
        if (tokens_.size() > kTokenMax) tokens_.pop_front();
    };

    if (t.size() >= 2 && t.size() <= 4) {
        push(t);
        return;
    }
    for (size_t p = 2; p <= 4; ++p) {
        if (t.size() < 2 * p || t.size() % p != 0) continue;
        bool periodic = true;
        for (size_t i = p; i < t.size() && periodic; ++i)
            if (t[i] != t[i % p]) periodic = false;
        if (periodic) {
            for (size_t r = 0; r < t.size() / p; ++r) push(t.substr(0, p));
            return;
        }
    }
}

ChannelSnapshot NdbChannel::snapshot(double now_s) const
{
    ChannelSnapshot s;
    s.id = id_;
    s.offset_hz = rot_.freq();
    s.freq_hz = center_hz_ + s.offset_hz;
    s.snr_db = snr_db_;
    s.carrier_db = dc_ > 0 ? 20.0f * std::log10(dc_) : -200.0f;
    // Tone and speed from the keying decoder once it has them — its pitch is
    // from a long averaged spectrum, and its speed is measured, not searched.
    s.pitch_hz = key_.pitch_hz() > 0 || !ggm_ ? key_.pitch_hz() : ggm_->getStatistics().estimatedPitch_Hz;
    s.speed_wpm = key_.keying() ? key_.wpm() : 0.0f;
    s.cost = cost_;
    s.contrast_db = key_.contrast_db();
    s.ggmorse = ggm_ != nullptr;
    s.keying = key_.keying();
    s.text = text_;
    s.text_ggm = text_ggm_;
    s.pinned = pinned_;
    s.age_s = now_s - created_;
    s.last_seen_s = now_s - last_seen_;
    s.last_text_s = last_text_ < 0 ? -1.0 : now_s - last_text_;

    std::map<std::string, std::pair<int, double>> tally;  // count, latest time
    for (const auto &t : tokens_) {
        if (now_s - t.t > kTokenWindow) continue;
        auto &e = tally[t.s];
        e.first++;
        e.second = std::max(e.second, t.t);
    }
    double best_t = -1.0;
    for (const auto &[tok, e] : tally) {
        if (e.first < 2) continue;
        if (e.first > s.ident_count || (e.first == s.ident_count && e.second > best_t)) {
            s.ident = tok;
            s.ident_count = e.first;
            best_t = e.second;
        }
    }
    // ggmorse sometimes hears a slow NDB's letter gap as a word gap, so a
    // three-letter ident also turns up as its first two letters ("EDN" and
    // "ED"), and the fragment, being in every copy, can out-count the whole.
    // Prefer a longer token containing the winner when it is itself well
    // represented.
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &[tok, e] : tally) {
            if (e.first < 2 || tok.size() <= s.ident.size()) continue;
            if (tok.find(s.ident) == std::string::npos) continue;
            if (e.first * 10 >= s.ident_count * 3) {
                s.ident = tok;
                s.ident_count = e.first;
                changed = true;
                break;
            }
        }
    }
    return s;
}

}  // namespace ndb
