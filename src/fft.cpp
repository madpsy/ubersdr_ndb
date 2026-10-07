#include "fft.h"

#include <fftw3.h>

#include <mutex>
#include <new>

namespace ndb {

namespace {

std::mutex &planner_lock()
{
    static std::mutex m;
    return m;
}

}  // namespace

Fft::Fft(size_t n) : n_(n)
{
    std::lock_guard<std::mutex> lock(planner_lock());
    buf_ = reinterpret_cast<std::complex<float> *>(fftwf_malloc(sizeof(fftwf_complex) * n));
    if (!buf_) throw std::bad_alloc();
    auto *p = reinterpret_cast<fftwf_complex *>(buf_);
    plan_ = fftwf_plan_dft_1d(int(n), p, p, FFTW_FORWARD, FFTW_ESTIMATE);
    for (size_t i = 0; i < n; ++i) buf_[i] = {};
}

Fft::~Fft()
{
    std::lock_guard<std::mutex> lock(planner_lock());
    fftwf_destroy_plan(static_cast<fftwf_plan>(plan_));
    fftwf_free(buf_);
}

void Fft::forward() { fftwf_execute(static_cast<fftwf_plan>(plan_)); }

RealFft::RealFft(size_t n, bool with_inverse) : n_(n)
{
    std::lock_guard<std::mutex> lock(planner_lock());
    in_ = static_cast<float *>(fftwf_malloc(sizeof(float) * n));
    out_ = reinterpret_cast<std::complex<float> *>(fftwf_malloc(sizeof(fftwf_complex) * (n / 2 + 1)));
    if (!in_ || !out_) throw std::bad_alloc();
    auto *o = reinterpret_cast<fftwf_complex *>(out_);
    fwd_ = fftwf_plan_dft_r2c_1d(int(n), in_, o, FFTW_ESTIMATE);
    if (with_inverse) inv_ = fftwf_plan_dft_c2r_1d(int(n), o, in_, FFTW_ESTIMATE);
    for (size_t i = 0; i < n; ++i) in_[i] = 0.0f;
    for (size_t i = 0; i < n / 2 + 1; ++i) out_[i] = {};
}

RealFft::~RealFft()
{
    std::lock_guard<std::mutex> lock(planner_lock());
    fftwf_destroy_plan(static_cast<fftwf_plan>(fwd_));
    if (inv_) fftwf_destroy_plan(static_cast<fftwf_plan>(inv_));
    fftwf_free(in_);
    fftwf_free(out_);
}

void RealFft::forward() { fftwf_execute(static_cast<fftwf_plan>(fwd_)); }
void RealFft::inverse() { fftwf_execute(static_cast<fftwf_plan>(inv_)); }

}  // namespace ndb
