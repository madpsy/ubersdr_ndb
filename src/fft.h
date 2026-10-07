// fft.h — forward FFTs of fixed size over FFTW (single precision), each
// transforming in buffers it owns. Planning is serialised, since FFTW's
// planner is not thread-safe (every stream has its own thread); executing a
// plan is. Plans are FFTW_ESTIMATE: measuring the 64k-point detector FFT and
// the channels' 16k ones took 3.5 s of CPU at start-up, to save well under a
// millisecond a second once running (the transforms here are a small share
// of the work; ubersdr-skimmer's, which measures, are most of its).

#pragma once

#include <complex>
#include <cstddef>

namespace ndb {

// Complex in, complex out, in place: fill data(), forward(), read data().
// Unnormalised; bin 0 is DC, the top half negative frequencies.
class Fft {
public:
    explicit Fft(size_t n);
    ~Fft();
    Fft(const Fft &) = delete;
    Fft &operator=(const Fft &) = delete;

    size_t size() const { return n_; }
    std::complex<float> *data() { return buf_; }
    const std::complex<float> *data() const { return buf_; }
    void forward();

private:
    size_t n_;
    std::complex<float> *buf_;
    void *plan_;
};

// Real in, the n/2 + 1 non-negative frequencies out (forward), or those back
// to n real samples (inverse, unnormalised: n times the input).
class RealFft {
public:
    explicit RealFft(size_t n, bool with_inverse = false);
    ~RealFft();
    RealFft(const RealFft &) = delete;
    RealFft &operator=(const RealFft &) = delete;

    size_t size() const { return n_; }
    float *in() { return in_; }
    std::complex<float> *out() { return out_; }
    const std::complex<float> *out() const { return out_; }
    void forward();   // in() -> out()
    void inverse();   // out() -> in() (out() is overwritten)

private:
    size_t n_;
    float *in_;
    std::complex<float> *out_;
    void *fwd_;
    void *inv_ = nullptr;
};

}  // namespace ndb
