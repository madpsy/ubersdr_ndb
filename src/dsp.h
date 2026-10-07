// dsp.h — the small DSP toolkit the NDB decoder is built from.
//
// Nothing here is NDB-specific: a windowed-sinc lowpass designer, a complex
// decimating FIR, a complex rotator (NCO) for mixing a carrier down to DC, and
// biquads. Header-only, no dependencies. (FFTs are in fft.h, over FFTW.)

#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace ndb {

using cf = std::complex<float>;

constexpr double kPi = 3.14159265358979323846;

inline size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// ---------------------------------------------------------------------------
// Windowed-sinc lowpass, Blackman window. Unity gain at DC.
//
// Blackman gives ~74 dB stopband with a transition of about 5.5·fs/ntaps,
// which is what taps_for() inverts.
// ---------------------------------------------------------------------------
inline std::vector<float> design_lowpass(int ntaps, double cutoff_hz, double fs)
{
    std::vector<float> h(ntaps);
    const double fc = cutoff_hz / fs;
    const int m = ntaps - 1;
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        double x = i - m / 2.0;
        double sinc = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
        double w = 0.42 - 0.5 * std::cos(2.0 * kPi * i / m) + 0.08 * std::cos(4.0 * kPi * i / m);
        h[i] = float(sinc * w);
        sum += h[i];
    }
    for (auto &v : h) v = float(v / sum);
    return h;
}

inline int taps_for(double transition_hz, double fs)
{
    int n = int(std::ceil(5.5 * fs / transition_hz));
    return n | 1;  // odd, so the filter has a centre tap
}

// ---------------------------------------------------------------------------
// Complex decimating FIR. Only every D'th output is computed.
//
// The history is kept twice over (a "mirrored" ring) so every dot product runs
// over one contiguous span without wrapping, and as separate real and
// imaginary arrays, with eight partial sums each: without -ffast-math the
// compiler will not reorder one running sum, and interleaved complex samples
// cost a shuffle per load. Split, it vectorises to plain multiply-adds.
// ---------------------------------------------------------------------------
class Decimator {
public:
    Decimator() = default;
    Decimator(std::vector<float> taps, int decim)
        : h_(std::move(taps)), d_(decim), re_(2 * h_.size()), im_(2 * h_.size()), n_(h_.size()) {}

    // Push one sample; returns true and sets `out` when an output is due.
    bool push(cf x, cf &out)
    {
        re_[pos_] = re_[pos_ + n_] = x.real();
        im_[pos_] = im_[pos_ + n_] = x.imag();
        if (++pos_ == n_) pos_ = 0;
        if (++phase_ < d_) return false;
        phase_ = 0;
        // Oldest sample is at pos_, newest at pos_+n_-1.
        const float *pr = re_.data() + pos_, *pi = im_.data() + pos_, *h = h_.data();
        constexpr size_t K = 8;
        float sr[K] = {}, si[K] = {};
        size_t i = 0;
        for (; i + K <= n_; i += K)
            for (size_t k = 0; k < K; ++k) {
                sr[k] += h[i + k] * pr[i + k];
                si[k] += h[i + k] * pi[i + k];
            }
        for (; i < n_; ++i) {
            sr[0] += h[i] * pr[i];
            si[0] += h[i] * pi[i];
        }
        float r = 0, m = 0;
        for (size_t k = 0; k < K; ++k) {
            r += sr[k];
            m += si[k];
        }
        out = cf(r, m);
        return true;
    }

    int decimation() const { return d_; }

private:
    std::vector<float> h_;
    int d_ = 1;
    std::vector<float> re_, im_;
    size_t n_ = 0;
    size_t pos_ = 0;
    int phase_ = 0;
};

// ---------------------------------------------------------------------------
// Complex rotator: multiplies the input by exp(-j·2π·f·t), moving a carrier at
// +f down to DC. Retuning keeps phase continuity. The phasor is renormalised
// periodically so float rounding cannot let its magnitude drift.
// ---------------------------------------------------------------------------
class Rotator {
public:
    void set(double freq_hz, double fs)
    {
        freq_ = freq_hz;
        double a = -2.0 * kPi * freq_hz / fs;
        step_ = std::complex<double>(std::cos(a), std::sin(a));
    }

    double freq() const { return freq_; }

    cf mix(cf x)
    {
        cf r = x * cf(float(ph_.real()), float(ph_.imag()));
        ph_ *= step_;
        if (++count_ == 4096) {
            count_ = 0;
            ph_ /= std::abs(ph_);
        }
        return r;
    }

private:
    double freq_ = 0.0;
    std::complex<double> ph_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
    int count_ = 0;
};

// ---------------------------------------------------------------------------
// RBJ-cookbook biquad high-pass (Butterworth Q by default). Real-valued.
// ---------------------------------------------------------------------------
class HighPass {
public:
    void set(double fc, double fs, double q = 0.7071067811865476)
    {
        double w = 2.0 * kPi * fc / fs, c = std::cos(w), a = std::sin(w) / (2.0 * q);
        double a0 = 1.0 + a;
        b0_ = float((1.0 + c) / 2.0 / a0);
        b1_ = float(-(1.0 + c) / a0);
        b2_ = b0_;
        a1_ = float(-2.0 * c / a0);
        a2_ = float((1.0 - a) / a0);
    }

    float process(float x)
    {
        float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

// RBJ-cookbook band-pass, 0 dB peak gain at fc. Real-valued.
class BandPass {
public:
    void set(double fc, double bw_hz, double fs)
    {
        const double w = 2.0 * kPi * fc / fs, c = std::cos(w), q = fc / bw_hz, a = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + a;
        b0_ = float(a / a0);
        b2_ = -b0_;
        a1_ = float(-2.0 * c / a0);
        a2_ = float((1.0 - a) / a0);
    }

    float process(float x)
    {
        float y = b0_ * x + z1_;
        z1_ = -a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

private:
    float b0_ = 1, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

}  // namespace ndb
