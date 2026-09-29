// ndb_decoder.h — the multi-NDB decoder: one wideband IQ stream in, a set of
// per-beacon channels out.
//
// The CarrierDetector finds carriers; this class turns its detection passes
// into channel lifecycle:
//
//   - a carrier seen on `confirm_passes` consecutive passes gets a channel;
//   - a channel's carrier estimate is followed if it drifts;
//   - a channel whose carrier has not been seen for `drop_after_s` is removed,
//     unless it was pinned from the command line;
//   - a channel whose ident is accepted gives its slot up. A beacon sends
//     nothing but its ident, so once that is known there is nothing more to
//     decode: the beacon is tracked by its carrier alone, and revisited with a
//     short decode every `revisit_s` to confirm it is still what is heard.
//
// Not thread-safe on its own: the caller serialises process() against
// snapshot()/spectrum() (main.cpp holds one mutex around both).

#pragma once

#include "carrier_detector.h"
#include "ndb_channel.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndb {

struct DecoderConfig {
    DetectorConfig detector;
    int    max_channels   = 8;      // identified beacons don't hold one (see revisit_s)
    int    confirm_passes = 2;
    double match_hz       = 20.0;   // detection within this of a channel is that channel
    double retune_hz      = 1.0;    // follow the carrier if it moves more than this
    double drop_after_s   = 120.0;
    double drop_identified_after_s = 1800.0;  // an identified beacon survives fades this long
    // Identified beacons: tracked by carrier, revisited to re-copy the ident.
    // A due revisit takes the next free slot ahead of waiting carriers, and
    // once revisit_grace_s overdue takes one from an unidentified channel.
    // A visit ends on visit_copies fresh copies of the ident, or gives up
    // after visit_timeout_s (a miss; tried again after visit_retry_s). A
    // beacon missed lost_misses visits running, or not copied for
    // lost_after_s, is forgotten.
    double revisit_s       = 900.0;
    double revisit_grace_s = 300.0;
    double visit_timeout_s = 180.0;
    double visit_retry_s   = 300.0;
    int    visit_copies    = 2;     // also main's --ident-copies for a first identification
    int    lost_misses     = 3;
    double lost_after_s    = 7200.0;
    // With every slot taken, a channel that has shown no keying for this long
    // gives its slot to the next waiting carrier, which then waits out a
    // cooldown before it can take one back. So all carriers get a turn, and
    // unkeyed spurs can't hold slots against real beacons.
    // A channel that has shown keying at any point keeps its slot through
    // silences up to active_hold_s (weak beacons fade in and out); only one
    // that has never keyed is judged on the short trial.
    double trial_s    = 180.0;
    double active_hold_s = 1800.0;
    double cooldown_s = 600.0;
    bool   auto_detect    = true;
    // Only give a channel to a carrier within known_tol_hz of a known
    // (published) beacon frequency, once set_known() has supplied the list.
    // Pinned frequencies are exempt.
    bool   known_only     = true;
    double known_tol_hz   = 300.0;
    // ggmorse as a second decoder. It costs ~10x the keying decoder, so by
    // default (Auto) a small pool of instances goes to the channels a second
    // opinion can help: unidentified, but showing some tone keying.
    enum class Ggmorse { Off, Auto, All };
    Ggmorse ggmorse     = Ggmorse::Auto;
    int    ggm_slots    = 6;        // Auto: instances at once, per stream
    // Keying evidence: a clear ident cycle (FoldDecoder), or contrast above
    // what noise reaches (~12-13, as the best of several lanes and tones) but
    // short of the keying decoder's 15.
    float  ggm_min_contrast_db = 13.5f;
    float  ggm_min_periodicity = 0.2f;
    double ggm_min_age_s = 20.0;    // let the keying decoder measure first
    double ggm_hold_s   = 300.0;    // give up on a channel after this long
    double ggm_cooldown_s = 900.0;  // before the same channel can have one again
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
    size_t waiting() const { return pending_.size(); }   // detected carriers without a slot
    size_t tracking() const { return tracked_.size(); }  // identified beacons watched by carrier

    // Absolute frequencies of published beacons worth a lower detection
    // threshold (see DetectorConfig::assist_snr_db).
    void set_assist(const std::vector<double> &abs_hz);

    // Frequencies (absolute Hz) of the published beacons that count as known:
    // with known_only, only carriers near one of these get a channel, and
    // existing unpinned channels that aren't are dropped.
    void set_known(const std::vector<double> &abs_hz);

    // Live decode feed: channel id, its frequency, and the new text.
    std::function<void(int, double, const std::string &)> on_decode;

    // Whether an ident is good enough to give the channel's slot up: called
    // with the absolute frequency, the ident and its copy count. Unset, any
    // ident with 3 copies is. Called under the caller's stream lock.
    std::function<bool(double, const std::string &, int)> accept_ident;

    // Seconds since its last copy within which a tracked beacon's ident
    // still counts as current (see ChannelSnapshot::ident_fresh).
    double fresh_s() const { return cfg_.revisit_s + cfg_.revisit_grace_s + cfg_.visit_timeout_s; }

    // Human-readable reason this sample rate cannot be used, or "" if it can.
    static std::string check_sample_rate(double fs);

private:
    void on_detection();
    void assign_ggmorse();
    std::map<int, double> ggm_since_;     // channel id -> when ggmorse was attached
    std::map<int, double> ggm_resting_;   // channel id -> when it was released unsuccessfully
    NdbChannel *add_channel(double offset_hz, bool pinned, int id = 0);
    void remove_channel(NdbChannel *ch);
    bool accepted(const ChannelSnapshot &s) const;
    void service_visits();
    void release_identified();
    void schedule_visits();

    // An identified beacon without a slot, watched through the detector.
    struct Tracked {
        int         id;             // the channel id it had, kept for its visits
        double      offset_hz;
        std::string ident;
        int         copies;         // best tally so far
        double      verified;       // stream time of the last copy
        double      seen;           // the detector last saw its carrier
        float       snr_db;
        int         misses = 0;     // visits running that copied nothing
        double      next_visit;
        bool        visiting = false;   // channel `id` is open for it now
        double      visit_start = 0.0;
        double      created;        // its first channel was opened
        ChannelSnapshot last;       // as it was when last decoded, for display
    };
    std::vector<Tracked> tracked_;
    Tracked *tracked_for(int id);
    NdbChannel *channel(int id);

    double center_hz_;
    double fs_;
    DecoderConfig cfg_;
    CarrierDetector det_;
    std::vector<std::unique_ptr<NdbChannel>> channels_;
    struct Pending { double offset_hz; int hits; bool hit_this_pass; float snr_db; };
    std::vector<Pending> pending_;
    struct Recycled { double offset_hz; double at; };
    std::vector<Recycled> recycled_;
    bool recently_recycled(double offset_hz) const;
    NdbChannel *recyclable();
    bool near_known(double offset_hz) const;
    std::vector<double> known_;   // sorted offsets; empty = no list yet (nothing filtered)
    int next_id_ = 1;
    double now_ = 0.0;
    uint64_t samples_ = 0;
    std::vector<cf> buf_;
};

}  // namespace ndb
