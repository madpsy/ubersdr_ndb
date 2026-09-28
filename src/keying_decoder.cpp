#include "keying_decoder.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ndb {

namespace {

constexpr double kEnvMs       = 10.0;    // envelope resolution
constexpr size_t kHistory     = 2000;    // 20 s of envelope for the threshold
constexpr int    kRefreshEvery = 100;    // recompute threshold every 1 s
// Below this it is not keying. Measured: noise and the unmodulated spur comb
// sit at 11.5-12.5 dB (debounced envelope, 97th vs 20th percentile); the
// weakest real beacon on the test capture (GLW, fading) at 17.
constexpr float  kMinContrastDb = 15.0f;
constexpr double kMinMarkMs   = 30.0;    // shorter marks are noise
constexpr size_t kMarksKept   = 60;
constexpr size_t kMarksNeeded = 6;
constexpr double kMinDitMs    = 50.0;    // 24 wpm
constexpr double kMaxDitMs    = 250.0;   // ~5 wpm
constexpr size_t kQualityOver = 20;      // marks judged for timing quality

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

}  // namespace

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
        if (pitch_ <= 0.0) continue;

        // Tone envelope: mix to DC at the pitch, integrate over 10 ms.
        acc_ += a[i] * cf(float(std::cos(ph_)), float(-std::sin(ph_)));
        ph_ += 2.0 * kPi * pitch_ / fs_;
        if (ph_ > 2.0 * kPi) ph_ -= 2.0 * kPi;
        if (++acc_n_ == block_) {
            float e = std::abs(acc_) / float(block_);
            acc_ = cf(0.0f, 0.0f);
            acc_n_ = 0;
            // Two-sample average: 20 ms of integration, still well under the
            // ~100 ms dit of a fast NDB.
            on_envelope(0.5f * (e + prev_env_));
            prev_env_ = e;
        }
    }
}

void KeyingDecoder::update_pitch()
{
    fft_.forward(fbuf_.data());
    const float alpha = spectra_ == 0 ? 1.0f : 0.3f;
    for (size_t k = 0; k < nfft_ / 2; ++k) spec_[k] += alpha * (std::norm(fbuf_[k]) - spec_[k]);
    ++spectra_;
    const double bin = fs_ / double(nfft_);
    size_t k0 = size_t(300.0 / bin), k1 = std::min(nfft_ / 2 - 2, size_t(1200.0 / bin));
    size_t best = k0;
    for (size_t k = k0; k <= k1; ++k)
        if (spec_[k] > spec_[best]) best = k;
    double y0 = spec_[best - 1], y1 = spec_[best], y2 = spec_[best + 1];
    double den = y0 - 2.0 * y1 + y2;
    double frac = den != 0.0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
    pitch_ = (double(best) + frac) * bin;
}

void KeyingDecoder::refresh_threshold()
{
    std::vector<float> h(hist_.begin(), hist_.end());
    auto pct = [&](double p) {
        auto it = h.begin() + long(p * double(h.size() - 1));
        std::nth_element(h.begin(), it, h.end());
        return *it;
    };
    const float lo = pct(0.20), hi = pct(0.97);
    contrast_db_ = lo > 0 ? 20.0f * std::log10(hi / lo) : 0.0f;
    // Needs a few seconds of history to have seen both keying and silence.
    keying_ = hist_.size() >= 300 && contrast_db_ >= kMinContrastDb;
    thr_ = 0.5f * (lo + hi);
    hyst_ = 0.1f * (hi - lo);
}

void KeyingDecoder::on_envelope(float e)
{
    hist_.push_back(e);
    if (hist_.size() > kHistory) hist_.pop_front();
    if (++since_refresh_ >= kRefreshEvery) {
        since_refresh_ = 0;
        refresh_threshold();
    }
    if (!keying_) {
        on_ = false;
        run_ = 0;
        code_.clear();
        return;
    }

    const bool on = on_ ? e > thr_ - hyst_ : e > thr_ + hyst_;
    ++run_;
    if (on == on_) {
        pending_ = 0;
        // A letter is complete once the silence after it is long enough;
        // don't wait for the next mark, which for an NDB may be seconds away.
        if (!on_ && dit_ms_ > 0 && !code_.empty() && run_ * kEnvMs >= 5.0 * dit_ms_) flush_letter(true);
        return;
    }
    // Debounce: commit the change once it has persisted.
    const int need = std::max(2, int(std::lround((dit_ms_ > 0 ? 0.3 * dit_ms_ : 20.0) / kEnvMs)));
    if (++pending_ < need) return;
    const double ms = (run_ - pending_) * kEnvMs;
    if (on_) end_mark(ms);
    else end_space(ms);
    on_ = on;
    run_ = pending_;
    pending_ = 0;
}

void KeyingDecoder::end_mark(double ms)
{
    if (ms < kMinMarkMs) return;
    marks_.push_back(ms);
    if (marks_.size() > kMarksKept) marks_.pop_front();

    // Dit length: split the sorted marks into short and long where the two
    // clusters are best separated and the ratio is dah-like. If every recent
    // mark is the same length (an ident of all dits, or all dahs) there is no
    // valid split, and the previous estimate stands.
    if (marks_.size() >= kMarksNeeded) {
        std::vector<double> s(marks_.begin(), marks_.end());
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
        if (best_dit >= kMinDitMs && best_dit <= kMaxDitMs) dit_ms_ = best_dit;
    }
    if (dit_ms_ <= 0) return;

    // Timing quality over the most recent marks.
    size_t n = 0, good = 0;
    for (auto it = marks_.rbegin(); it != marks_.rend() && n < kQualityOver; ++it, ++n) {
        const double r = *it / dit_ms_;
        if ((r >= 0.6 && r <= 1.5) || (r >= 2.2 && r <= 4.0)) ++good;
    }
    quality_ = n ? float(good) / float(n) : 0.0f;

    code_ += ms < 2.0 * dit_ms_ ? '.' : '-';
}

void KeyingDecoder::end_space(double ms)
{
    if (dit_ms_ <= 0 || code_.empty()) return;
    if (ms >= 2.0 * dit_ms_) flush_letter(ms >= 5.0 * dit_ms_);
}

void KeyingDecoder::flush_letter(bool word)
{
    if (!keying()) {
        code_.clear();
        return;
    }
    const auto &t = morse_table();
    auto it = t.find(code_);
    std::string out(1, it == t.end() ? '?' : it->second);
    if (word) out += ' ';
    code_.clear();
    if (on_text) on_text(out);
}

}  // namespace ndb
