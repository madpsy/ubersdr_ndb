// ndb_decoder.h — the multi-NDB decoder: one wideband IQ stream in, a set of
// per-beacon channels out.
//
// The CarrierDetector finds carriers; this class turns its detection passes
// into channel lifecycle:
//
//   - a carrier seen on `confirm_passes` consecutive passes gets a channel;
//   - a channel's carrier estimate is followed if it drifts;
//   - a channel whose carrier has not been seen for `drop_after_s` is removed,
//     unless it was pinned from the command line.
//
// Not thread-safe on its own: the caller serialises process() against
// snapshot()/spectrum() (main.cpp holds one mutex around both).

#pragma once

#include "carrier_detector.h"
#include "ndb_channel.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ndb {

struct DecoderConfig {
    DetectorConfig detector;
    int    max_channels   = 24;
    int    confirm_passes = 2;
    double match_hz       = 20.0;   // detection within this of a channel is that channel
    double retune_hz      = 1.0;    // follow the carrier if it moves more than this
    double drop_after_s   = 120.0;
    double drop_identified_after_s = 1800.0;  // an identified beacon survives fades this long
    bool   auto_detect    = true;
    std::vector<double> pinned_hz;  // absolute frequencies always decoded
};

class NdbDecoder {
public:
    NdbDecoder(double center_hz, double sample_rate, DecoderConfig cfg);

    // Interleaved int16 I/Q, as the UberSDR iq modes deliver it.
    void process_iq(const int16_t *iq, size_t n_pairs);

    std::vector<ChannelSnapshot> snapshot() const;
    void spectrum(size_t points, std::vector<float> &db, std::vector<float> &floor_db) const;

    double center_hz() const { return center_hz_; }
    double sample_rate() const { return fs_; }
    double stream_time() const { return now_; }
    const std::vector<Carrier> &carriers() const { return det_.carriers(); }

    // Absolute frequencies of published beacons worth a lower detection
    // threshold (see DetectorConfig::assist_snr_db).
    void set_assist(const std::vector<double> &abs_hz);

    // Live decode feed: channel id, its frequency, and the new text.
    std::function<void(int, double, const std::string &)> on_decode;

    // Human-readable reason this sample rate cannot be used, or "" if it can.
    static std::string check_sample_rate(double fs);

private:
    void on_detection();
    NdbChannel *add_channel(double offset_hz, bool pinned);

    double center_hz_;
    double fs_;
    DecoderConfig cfg_;
    CarrierDetector det_;
    std::vector<std::unique_ptr<NdbChannel>> channels_;
    struct Pending { double offset_hz; int hits; bool hit_this_pass; };
    std::vector<Pending> pending_;
    int next_id_ = 1;
    double now_ = 0.0;
    uint64_t samples_ = 0;
    std::vector<cf> buf_;
};

}  // namespace ndb
