// dsp.h — the small DSP toolkit the NDB decoder is built from.
//
// Nothing here is NDB-specific: a radix-2 FFT for the wideband carrier search,
// a windowed-sinc lowpass designer, a complex decimating FIR, and a complex
// rotator (NCO) for mixing a carrier down to DC. Header-only, no dependencies.

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
// In-place iterative radix-2 complex FFT. Twiddles and the bit-reversal table
// are computed once per size, so repeated transforms of the same length (the
// only way this is used) cost nothing but the butterflies.
// ---------------------------------------------------------------------------
class FFT {
public:
    explicit FFT(size_t n) : n_(n), tw_(n / 2), rev_(n)
    {
        for (size_t i = 0; i < n / 2; ++i) {
            double a = -2.0 * kPi * double(i) / double(n);
            tw_[i] = cf(float(std::cos(a)), float(std::sin(a)));
        }
        size_t bits = 0;
        while ((size_t(1) << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
            rev_[i] = r;
        }
    }

    size_t size() const { return n_; }

    void forward(cf *x) const
    {
        for (size_t i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len / 2, step = n_ / len;
            for (size_t i = 0; i < n_; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    cf t = x[i + j + half] * tw_[j * step];
                    x[i + j + half] = x[i + j] - t;
                    x[i + j] += t;
                }
            }
        }
    }

private:
    size_t n_;
    std::vector<cf> tw_;
    std::vector<size_t> rev_;
};

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
// over one contiguous span without wrapping.
// ---------------------------------------------------------------------------
class Decimator {
public:
    Decimator() = default;
    Decimator(std::vector<float> taps, int decim)
        : h_(std::move(taps)), d_(decim), hist_(2 * h_.size()), n_(h_.size()) {}

    // Push one sample; returns true and sets `out` when an output is due.
    bool push(cf x, cf &out)
    {
        hist_[pos_] = x;
        hist_[pos_ + n_] = x;
        if (++pos_ == n_) pos_ = 0;
        if (++phase_ < d_) return false;
        phase_ = 0;
        // Oldest sample is at pos_, newest at pos_+n_-1.
        const cf *p = &hist_[pos_];
        float re = 0.f, im = 0.f;
        for (size_t i = 0; i < n_; ++i) {
            re += h_[i] * p[i].real();
            im += h_[i] * p[i].imag();
        }
        out = cf(re, im);
        return true;
    }

    int decimation() const { return d_; }

private:
    std::vector<float> h_;
    int d_ = 1;
    std::vector<cf> hist_;
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

}  // namespace ndb
