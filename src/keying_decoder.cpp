#include "keying_decoder.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ndb {

namespace {

constexpr size_t kHistory     = 2000;    // 20 s of envelope for the threshold
constexpr int    kRefreshEvery = 100;    // recompute threshold every 1 s
// Below this it is not keying. Measured: noise and the unmodulated spur comb
// sit at 11.5-12.5 dB (debounced envelope, 97th vs 20th percentile); the
// weakest real beacon on the test capture (GLW, fading) at 17. Noise's figure
// is the same in every lane: its envelope is Rayleigh whatever the window.
constexpr float  kMinContrastDb = 15.0f;
constexpr double kMinMarkMs   = 30.0;    // shorter marks are noise
constexpr size_t kMarksKept   = 60;
constexpr size_t kMarksNeeded = 6;
constexpr double kMinDitMs    = 50.0;    // 24 wpm
constexpr double kMaxDitMs    = 250.0;   // ~5 wpm
constexpr size_t kQualityOver = 20;      // marks judged for timing quality
constexpr int    kLaneW[]     = {2, 4, 6, 8};   // lane envelope windows, 10 ms blocks
constexpr size_t kMaxTones    = 3;       // candidate tones followed at once

}  // namespace

const std::map<std::string, char> &morse_table()
{
    static const std::map<std::string, char> t = {
        {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},   {".", 'E'},     {"..-.", 'F'},
        {"--.", 'G'},   {"....", 'H'},  {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
        {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},   {".--.", 'P'},  {"--.-", 'Q'},  {".-.", 'R'},
        {"...", 'S'},   {"-", 'T'},     {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},
        {"-.--", 'Y'},  {"--..", 'Z'},  {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
    };
    return t;
}

KeyingDecoder::KeyingDecoder(double audio_rate)
    : fs_(audio_rate),
      // ~4 s per spectrum at 4 kHz, 0.24 Hz bins: the tone is a clean line
      // after one, and the average settles over a few.
      nfft_(next_pow2(size_t(audio_rate * 4.0))),
      fft_(nfft_),
      fbuf_(nfft_),
      spec_(nfft_ / 2, 0.0f),
      block_(int(std::lround(audio_rate * kEnvMs / 1000.0)))
{
    static_assert(sizeof(kLaneW) / sizeof(kLaneW[0]) == std::tuple_size<decltype(lanes_)>::value);
    for (size_t i = 0; i < lanes_.size(); ++i) lanes_[i].w = kLaneW[i];
}

float KeyingDecoder::contrast_db() const
{
    float c = 0.0f;
    for (const auto &l : lanes_) c = std::max(c, l.contrast_db);
    return c;
}

void KeyingDecoder::Tone::set(double f, double fs)
{
    hz = f;
    step = cf(float(std::cos(2.0 * kPi * f / fs)), float(-std::sin(2.0 * kPi * f / fs)));
}

void KeyingDecoder::process(const float *a, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        // Pitch search runs continuously, so a retuned or re-toned beacon is
        // followed.
        const float w = float(0.5 - 0.5 * std::cos(2.0 * kPi * double(ffill_) / double(nfft_)));
        fbuf_[ffill_] = cf(a[i] * w, 0.0f);
        if (++ffill_ == nfft_) {
            ffill_ = 0;
            update_pitch();
        }
        if (tones_.empty()) continue;

        // Tone envelopes: each candidate mixed to DC, summed over 10 ms.
        for (auto &t : tones_) {
            t.acc += a[i] * t.ph;
            t.ph *= t.step;
        }
        if (++acc_n_ < block_) continue;
        acc_n_ = 0;
        for (auto &t : tones_) {
            t.ph /= std::abs(t.ph);   // keep the phasor on the unit circle
            t.hist.push_back(std::abs(t.acc + t.prev) / float(2 * block_));
            if (t.hist.size() > kHistory) t.hist.pop_front();
            t.prev = t.acc;
            t.acc = cf(0.0f, 0.0f);
        }
        envelope_sample(tones_[tone_].prev);
        if (++since_choose_ >= kRefreshEvery) {
            since_choose_ = 0;
            choose_tone();
        }
    }
}

// The candidates: the three strongest local peaks 300-1200 Hz, at least 20 Hz
// apart. A candidate that is still there keeps its mixer and history.
void KeyingDecoder::update_pitch()
{
    fft_.forward(fbuf_.data());
    const float alpha = spectra_ == 0 ? 1.0f : 0.3f;
    for (size_t k = 0; k < nfft_ / 2; ++k) spec_[k] += alpha * (std::norm(fbuf_[k]) - spec_[k]);
    ++spectra_;
    const double bin = fs_ / double(nfft_);
    const size_t k0 = size_t(300.0 / bin), k1 = std::min(nfft_ / 2 - 2, size_t(1200.0 / bin));
    std::vector<size_t> peaks;
    for (size_t k = k0; k <= k1; ++k)
        if (spec_[k] > spec_[k - 1] && spec_[k] >= spec_[k + 1]) peaks.push_back(k);
    std::sort(peaks.begin(), peaks.end(), [&](size_t x, size_t y) { return spec_[x] > spec_[y]; });
    std::vector<double> found;
    for (size_t k : peaks) {
        double y0 = spec_[k - 1], y1 = spec_[k], y2 = spec_[k + 1];
        double den = y0 - 2.0 * y1 + y2;
        double frac = den != 0.0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
        const double f = (double(k) + frac) * bin;
        bool near = false;
        for (double g : found) near = near || std::fabs(f - g) < 20.0;
        if (near) continue;
        found.push_back(f);
        if (found.size() == kMaxTones) break;
    }
    if (found.empty()) return;

    const double cur = tones_.empty() ? -1.0 : tones_[tone_].hz;
    std::vector<Tone> next;
    for (double f : found) {
        auto it = std::find_if(tones_.begin(), tones_.end(), [&](const Tone &t) { return std::fabs(t.hz - f) < 2.0; });
        next.push_back(it != tones_.end() ? std::move(*it) : Tone{});
        next.back().set(f, fs_);
    }
    tones_ = std::move(next);
    // Stay on the tone being decoded while it is still a candidate; else
    // start on the strongest, until contrast says otherwise.
    size_t keep = 0;
    bool kept = false;
    for (size_t i = 0; i < tones_.size(); ++i)
        if (cur > 0 && std::fabs(tones_[i].hz - cur) < 2.0) keep = i, kept = true;
    tone_ = keep;
    pitch_ = tones_[tone_].hz;
    if (cur > 0 && !kept) retone(tone_);
}

// Move to the candidate with the most on/off contrast, if it clearly has more
// than the one being decoded: 3 dB, over at least 3 s of history.
void KeyingDecoder::choose_tone()
{
    for (auto &t : tones_) {
        if (t.hist.size() < 300) {
            t.contrast_db = 0.0f;
            continue;
        }
        std::vector<float> h(t.hist.begin(), t.hist.end());
        auto pct = [&](double p) {
            auto it = h.begin() + long(p * double(h.size() - 1));
            std::nth_element(h.begin(), it, h.end());
            return *it;
        };
        const float lo = pct(0.20), hi = pct(0.97);
        t.contrast_db = lo > 0 ? 20.0f * std::log10(hi / lo) : 0.0f;
    }
    size_t best = tone_;
    for (size_t i = 0; i < tones_.size(); ++i)
        if (tones_[i].contrast_db > tones_[best].contrast_db) best = i;
    if (best != tone_ && tones_[best].contrast_db >= tones_[tone_].contrast_db + 3.0f) retone(best);
}

// Start decoding a different tone: nothing learned from the old one applies.
void KeyingDecoder::retone(size_t i)
{
    tone_ = i;
    pitch_ = tones_[i].hz;
    for (auto &l : lanes_) {
        const int w = l.w;
        l = Lane();
        l.w = w;
    }
    sel_ = 0;
    filled_ = 0;
    if (on_retone) on_retone();
}

// One 10 ms block has arrived: each lane's envelope is the coherent sum of its
// last W blocks. The tone is known to a fraction of a hertz, so even 80 ms
// loses nothing to phase drift.
void KeyingDecoder::envelope_sample(cf block)
{
    blocks_[size_t(head_)] = block;
    head_ = (head_ + 1) % kMaxW;
    filled_ = std::min(filled_ + 1, kMaxW);
    cf sum(0.0f, 0.0f);
    int k = 0;
    for (auto &l : lanes_) {
        for (; k < l.w; ++k) sum += blocks_[size_t((head_ - 1 - k + 2 * kMaxW) % kMaxW)];
        if (filled_ < l.w) continue;
        const float e = std::abs(sum) / float(l.w * block_);
        if (l.w == 2 && on_envelope) on_envelope(e);
        lane_sample(l, e);
    }
    choose_lane();
}

// The copy comes from the widest lane that fits the speed it has measured,
// else the narrowest (which fits any speed the decoder accepts). A switch only
// happens between words, in both lanes, so no letter is lost or repeated.
void KeyingDecoder::choose_lane()
{
    size_t want = 0;
    for (size_t i = lanes_.size(); i-- > 0;)
        if (lanes_[i].fits()) {
            want = i;
            break;
        }
    if (want != sel_ && lanes_[sel_].idle() && lanes_[want].idle()) sel_ = want;
}

void KeyingDecoder::refresh_threshold(Lane &l)
{
    std::vector<float> h(l.hist.begin(), l.hist.end());
    auto pct = [&](double p) {
        auto it = h.begin() + long(p * double(h.size() - 1));
        std::nth_element(h.begin(), it, h.end());
        return *it;
    };
    const float lo = pct(0.20), hi = pct(0.97);
    l.contrast_db = lo > 0 ? 20.0f * std::log10(hi / lo) : 0.0f;
    // Needs a few seconds of history to have seen both keying and silence.
    l.gate = l.hist.size() >= 300 && l.contrast_db >= kMinContrastDb;
    // On at 50% of the way from floor to peak, off at 40%. A higher on-level
    // (it was 60%) misses dits that a fade or the window's smoothing leaves
    // short of full height, and a lost dit breaks its letter in two; a lower
    // off-level merges a fast beacon's dits across their gaps.
    l.thr = lo + 0.45f * (hi - lo);
    l.hyst = 0.05f * (hi - lo);
}

void KeyingDecoder::lane_sample(Lane &l, float e)
{
    l.hist.push_back(e);
    if (l.hist.size() > kHistory) l.hist.pop_front();
    if (++l.since_refresh >= kRefreshEvery) {
        l.since_refresh = 0;
        refresh_threshold(l);
    }
    if (!l.gate) {
        l.on = false;
        l.run = 0;
        l.code.clear();
        l.in_word = false;
        return;
    }

    const bool on = l.on ? e > l.thr - l.hyst : e > l.thr + l.hyst;
    ++l.run;
    if (on == l.on) {
        l.pending = 0;
        // A letter is complete once the silence after it is long enough;
        // don't wait for the next mark, which for an NDB may be seconds away.
        if (!l.on && l.dit_ms > 0 && l.run * kEnvMs >= 5.0 * l.dit_ms) {
            if (!l.code.empty()) flush_letter(l, true);
            else if (l.in_word) {
                l.in_word = false;
                if (&l == &lane() && on_text) on_text(" ");
            }
        }
        return;
    }
    // Debounce: commit the change once it has persisted.
    const int need = std::max(2, int(std::lround((l.dit_ms > 0 ? 0.3 * l.dit_ms : 20.0) / kEnvMs)));
    if (++l.pending < need) return;
    const double ms = (l.run - l.pending) * kEnvMs;
    if (l.on) end_mark(l, ms);
    else end_space(l, ms);
    l.on = on;
    l.run = l.pending;
    l.pending = 0;
}

void KeyingDecoder::end_mark(Lane &l, double ms)
{
    if (ms < kMinMarkMs) return;
    l.marks.push_back(ms);
    if (l.marks.size() > kMarksKept) l.marks.pop_front();

    // Dit length: split the sorted marks into short and long where the two
    // clusters are best separated and the ratio is dah-like. If every recent
    // mark is the same length (an ident of all dits, or all dahs) there is no
    // valid split, and the previous estimate stands.
    if (l.marks.size() >= kMarksNeeded) {
        std::vector<double> s(l.marks.begin(), l.marks.end());
        std::sort(s.begin(), s.end());
        double best_score = -1.0, best_dit = 0.0;
        for (size_t k = 1; k < s.size(); ++k) {
            double m1 = 0, m2 = 0, v1 = 0, v2 = 0;
            for (size_t i = 0; i < k; ++i) m1 += s[i];
            for (size_t i = k; i < s.size(); ++i) m2 += s[i];
            m1 /= double(k);
            m2 /= double(s.size() - k);
            for (size_t i = 0; i < k; ++i) v1 += (s[i] - m1) * (s[i] - m1);
            for (size_t i = k; i < s.size(); ++i) v2 += (s[i] - m2) * (s[i] - m2);
            const double ratio = m2 / m1;
            if (ratio < 2.0 || ratio > 4.5) continue;
            const double score = (m2 - m1) / (std::sqrt(v1 / double(k)) + std::sqrt(v2 / double(s.size() - k)) + 10.0);
            if (score > best_score) {
                best_score = score;
                best_dit = m1;
            }
        }
        if (best_dit >= kMinDitMs && best_dit <= kMaxDitMs) l.dit_ms = best_dit;
    }
    if (l.dit_ms <= 0) return;

    // Timing quality over the most recent marks.
    size_t n = 0, good = 0;
    for (auto it = l.marks.rbegin(); it != l.marks.rend() && n < kQualityOver; ++it, ++n) {
        const double r = *it / l.dit_ms;
        if ((r >= 0.6 && r <= 1.5) || (r >= 2.2 && r <= 4.0)) ++good;
    }
    l.quality = n ? float(good) / float(n) : 0.0f;

    l.code += ms < 2.0 * l.dit_ms ? '.' : '-';
    // No Morse character has more than 6 elements; if there has been no
    // letter gap by then, emit what we have (as '?') rather than let it grow.
    if (l.code.size() > 6) flush_letter(l, false);
}

void KeyingDecoder::end_space(Lane &l, double ms)
{
    if (l.dit_ms <= 0 || l.code.empty()) return;
    if (ms >= 2.0 * l.dit_ms) flush_letter(l, ms >= 5.0 * l.dit_ms);
}

void KeyingDecoder::flush_letter(Lane &l, bool word)
{
    if (!l.keying()) {
        l.code.clear();
        l.in_word = false;
        return;
    }
    const auto &t = morse_table();
    auto it = t.find(l.code);
    std::string out(1, it == t.end() ? '?' : it->second);
    if (word) out += ' ';
    l.code.clear();
    l.in_word = !word;
    if (&l == &lane() && on_text) on_text(out);
}

}  // namespace ndb
