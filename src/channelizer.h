// channelizer.h — a polyphase filter bank over one stream's IQ, shared by
// every beacon channel (ported from ubersdr-skimmer's Channelizer).
//
// A channel used to mix the whole stream to its carrier and FIR-filter it down
// itself, so its cost was paid at the full IQ rate, once per channel. The bank
// instead splits the stream once into subbands 3 kHz apart, each sampled at
// 12 kHz (4x oversampled) and flat over about ±4.9 kHz; a channel reads the
// one subband its carrier is nearest the centre of, and its ±1.3 kHz lies
// flat inside it wherever the carrier falls. Per input sample the bank costs
// six multiply-adds a component and a share of a small FFT, whatever the
// number of channels; a channel then works at 12 kHz rather than 192.

#pragma once

#include "dsp.h"
#include "fft.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ndb {

class Channelizer {
public:
    explicit Channelizer(double rate);

    // Subbands at this IQ rate (a power of two), and their output rate:
    // 12 kHz at iq48, iq96, iq192 and iq384.
    static size_t subbands_for(double rate);
    static double out_rate_for(double rate) { return rate / double(subbands_for(rate) / kOver); }

    size_t size() const { return m_; }
    double spacing() const { return rate_ / double(m_); }
    double out_rate() const { return rate_ / double(d_); }
    double flat_hz() const { return flat_hz_; }

    // The subband nearest off_hz from the IQ centre, and off_hz from its centre.
    int subband_for(double off_hz, double *rel_hz = nullptr) const;
    // Subband k's centre, from the IQ centre.
    double centre_of(int k) const;

    // Feed IQ. The rows completed by this call are then readable with read();
    // the next call starts afresh.
    void process(const cf *iq, size_t n);
    size_t rows() const { return rows_; }
    // Subband k's samples from this call's rows, phase-continuous across calls.
    void read(int k, std::vector<cf> &out) const;

private:
    void row();

    double rate_;
    size_t m_, d_, taps_;
    static constexpr unsigned kOver = 4;
    cf turn_[kOver];
    double flat_hz_;
    std::vector<float> h_;           // the prototype, time-reversed
    std::vector<float> re_, im_;     // input: taps_ - 1 history, then new samples
    size_t fill_, since_ = 0;
    std::vector<float> fr_, fi_;     // a row's fold
    std::unique_ptr<Fft> fft_;
    std::vector<cf> out_;            // this call's rows, m_ each
    size_t rows_ = 0;                // in out_
    uint64_t total_ = 0;             // rows ever, for the subband phase
    uint64_t first_ = 0;             // total_ at this call's first row
};

}  // namespace ndb
