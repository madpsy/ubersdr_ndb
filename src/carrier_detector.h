// carrier_detector.h — find NDB carriers in a wideband IQ stream.
//
// An NDB is, spectrally, a steady unmodulated carrier with (for A2A, the usual
// case) a pair of keyed tone sidebands 400 or 1020 Hz either side of it. The
// carrier is on continuously, so a long, fine-resolution averaged spectrum
// shows it as a narrow line standing well out of the noise, while the keyed
// sidebands — on for a fraction of the time — average down well below it.
//
// The detector keeps an exponentially averaged power spectrum at ~3 Hz
// resolution, estimates a local noise floor, and reports narrow peaks that
// clear it by a threshold. Peaks that are one half of a symmetric pair around
// a stronger carrier are dropped as that carrier's tone sidebands.

#pragma once

#include "dsp.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ndb {

struct Carrier {
    double offset_hz = 0.0;   // relative to the IQ centre frequency
    float  level_db  = 0.0f;  // averaged bin power, dBFS-ish (relative)
    float  snr_db    = 0.0f;  // above the local noise floor
};

struct DetectorConfig {
    float  snr_threshold_db  = 10.0f;  // peak must clear the floor by this
    float  edge_fraction     = 0.45f;  // ignore |offset| > edge_fraction·fs
    float  avg_alpha         = 0.25f;  // EMA weight of each new FFT frame
    int    detect_every      = 6;      // run peak search every N frames
    double peak_halfwidth_hz = 150.0;  // a carrier is the max within ±this
    double neighbour_hz        = 2500.0; // drop peaks this close to ...
    float  neighbour_margin_db = 15.0f;  // ... a carrier this much stronger

    // Lower threshold near frequencies where a beacon is published (and near
    // enough to be heard): a weak carrier exactly where one is expected is far
    // more likely real than one anywhere else, so it is worth a channel.
    float  assist_snr_db = 7.0f;
    double assist_tol_hz = 50.0;
};

class CarrierDetector {
public:
    CarrierDetector(double sample_rate, DetectorConfig cfg = {});

    // Feed samples. Returns true when a new detection pass has completed and
    // carriers() holds fresh results.
    bool process(const cf *x, size_t n);

    const std::vector<Carrier> &carriers() const { return carriers_; }

    // Offsets (from the IQ centre) where the assisted threshold applies.
    void set_assist(const std::vector<double> &offsets_hz);

    // Averaged spectrum and floor estimate in dB, FFT-shifted so index 0 is
    // -fs/2. Reduced to at most `points` values by taking the max of each group
    // (so narrow carriers survive the reduction).
    void spectrum(size_t points, std::vector<float> &db, std::vector<float> &floor_db) const;

    double bin_hz() const { return fs_ / double(n_); }
    size_t fft_size() const { return n_; }
    bool   ready() const { return frames_ >= 3; }

private:
    void run_detection();
    void compute_floor();

    double fs_;
    DetectorConfig cfg_;
    size_t n_;
    FFT fft_;
    std::vector<float> window_;
    std::vector<cf> buf_;
    size_t fill_ = 0;
    std::vector<float> avg_;       // linear power, natural FFT order
    std::vector<float> db_;        // shifted dB
    std::vector<float> floor_db_;  // shifted dB
    uint64_t frames_ = 0;
    std::vector<Carrier> carriers_;
    std::vector<uint8_t> assist_mask_;  // per shifted bin; empty = none
};

}  // namespace ndb
