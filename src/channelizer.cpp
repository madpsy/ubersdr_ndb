#include "channelizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ndb {

namespace {

constexpr double kStopDb = 70.0;          // prototype stopband
constexpr size_t kTapsPerBand = 6;
constexpr size_t kBlock = 4096;           // input buffered before it is moved down

double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

}  // namespace

// Each row is, for every subband k at once,
//   y_k = sum over l of h[l] x[n0 - l] e^(-j 2 pi k (n0 - l) / M)
// at the newest sample n0: the input mixed down by k * rate / M and
// low-passed by the prototype h. The terms fold into M sums (l modulo M) and
// one M-point FFT does every k. With the decimation M / 4, the phase
// e^(-j 2 pi k n0 / M) left over comes to e^(-j 2 pi k (row + 1) / 4),
// applied as a subband is read. The prototype is a Kaiser-windowed sinc cut
// off at twice the spacing.
size_t Channelizer::subbands_for(double rate)
{
    size_t half = 1;
    while (rate / double(half * 2) >= 4000.0) half *= 2;   // 192 kHz: 32
    return 2 * half;                                       // 64 subbands, 3 kHz apart
}

Channelizer::Channelizer(double rate) : rate_(rate)
{
    m_ = subbands_for(rate);
    d_ = m_ / kOver;                                       // 12 kHz out
    taps_ = kTapsPerBand * m_;
    const double cutoff = rate / double(m_) * kOver / 2.0;
    const double beta = 0.1102 * (kStopDb - 8.7);
    const double transition = (kStopDb - 8.0) / (2.285 * 2.0 * kPi * double(taps_ - 1)) * rate;
    flat_hz_ = cutoff - transition / 2.0;
    std::vector<double> h(taps_);
    double sum = 0.0;
    const double mid = double(taps_ - 1) / 2.0;
    for (size_t i = 0; i < taps_; ++i) {
        const double t = double(i) - mid;
        const double x = 2.0 * cutoff / rate * t;
        const double sinc = t == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double r = t / mid;
        h[i] = sinc * bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel_i0(beta);
        sum += h[i];
    }
    h_.resize(taps_);
    for (size_t i = 0; i < taps_; ++i) h_[i] = float(h[taps_ - 1 - i] / sum);
    re_.assign(taps_ - 1 + kBlock, 0.0f);
    im_.assign(taps_ - 1 + kBlock, 0.0f);
    fill_ = taps_ - 1;
    fr_.assign(m_, 0.0f);
    fi_.assign(m_, 0.0f);
    fft_ = std::make_unique<Fft>(m_);
    for (unsigned p = 0; p < kOver; ++p) turn_[p] = std::polar(1.0f, float(-2.0 * kPi * p / kOver));
}

int Channelizer::subband_for(double off_hz, double *rel_hz) const
{
    long k = std::lround(off_hz / spacing());
    const long half = long(m_ / 2);
    k = std::clamp(k, -half, half - 1);
    if (rel_hz) *rel_hz = off_hz - double(k) * spacing();
    return int(k < 0 ? k + long(m_) : k);
}

double Channelizer::centre_of(int k) const
{
    const long kk = k >= int(m_ / 2) ? long(k) - long(m_) : long(k);
    return double(kk) * spacing();
}

void Channelizer::process(const cf *iq, size_t n)
{
    first_ = total_;
    rows_ = 0;
    out_.resize((n / d_ + 1) * m_);
    while (n > 0) {
        if (fill_ == re_.size()) {
            const size_t keep = taps_ - 1;
            std::memmove(re_.data(), re_.data() + fill_ - keep, keep * sizeof(float));
            std::memmove(im_.data(), im_.data() + fill_ - keep, keep * sizeof(float));
            fill_ = keep;
        }
        const size_t take = std::min({n, d_ - since_, re_.size() - fill_});
        float *__restrict r = re_.data() + fill_;
        float *__restrict i = im_.data() + fill_;
        for (size_t j = 0; j < take; ++j) {
            r[j] = iq[j].real();
            i[j] = iq[j].imag();
        }
        fill_ += take;
        since_ += take;
        iq += take;
        n -= take;
        if (since_ == d_) {
            row();
            since_ = 0;
        }
    }
}

void Channelizer::row()
{
    const size_t m = m_;
    const float *__restrict wr = re_.data() + fill_ - taps_;
    const float *__restrict wi = im_.data() + fill_ - taps_;
    const float *__restrict h = h_.data();
    float *__restrict fr = fr_.data();
    float *__restrict fi = fi_.data();
    for (size_t j = 0; j < m; ++j) {
        fr[j] = h[j] * wr[j];
        fi[j] = h[j] * wi[j];
    }
    for (size_t c = m; c < taps_; c += m)
        for (size_t j = 0; j < m; ++j) {
            fr[j] += h[c + j] * wr[c + j];
            fi[j] += h[c + j] * wi[c + j];
        }
    cf *buf = fft_->data();
    for (size_t j = 0; j < m; ++j) buf[j] = cf(fr[j], fi[j]);
    fft_->forward();
    if ((rows_ + 1) * m > out_.size()) out_.resize((rows_ + 1) * m);
    std::memcpy(&out_[rows_ * m], buf, m * sizeof(cf));
    ++rows_;
    ++total_;
}

void Channelizer::read(int k, std::vector<cf> &out) const
{
    out.resize(rows_);
    const unsigned step = unsigned(uint64_t(k) % kOver);
    unsigned p = unsigned((uint64_t(k) * (first_ + 1)) % kOver);
    const cf *src = out_.data() + k;
    for (size_t r = 0; r < rows_; ++r) {
        const cf v = src[r * m_];
        out[r] = p ? v * turn_[p] : v;
        p = (p + step) % kOver;
    }
}

}  // namespace ndb
