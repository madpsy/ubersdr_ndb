#include "ndb_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ndb {

std::string NdbDecoder::check_sample_rate(double fs)
{
    // The channelizer's subbands come out at 3 kHz x 4 (see channelizer.h),
    // and each channel decimates its subband to 4 kHz by an integer factor.
    const double out = fs >= 12000.0 ? Channelizer::out_rate_for(fs) : 0.0;
    if (out <= 0.0 || std::fmod(out, kAudioRate) != 0.0)
        return "sample rate " + std::to_string(int(fs)) +
               " Hz does not split into 3 kHz subbands; use iq48, iq96, iq192 or iq384";
    return "";
}

NdbDecoder::NdbDecoder(double center_hz, double sample_rate, DecoderConfig cfg)
    : center_hz_(center_hz), fs_(sample_rate), cfg_(std::move(cfg)), det_(sample_rate, cfg_.detector), bank_(sample_rate)
{
    for (double f : cfg_.pinned_hz) {
        double off = f - center_hz_;
        if (std::fabs(off) > 0.5 * fs_) {
            fprintf(stderr, "ndb: pinned %.1f Hz is outside the IQ passband (centre %.0f, ±%.0f Hz) — ignored\n",
                    f, center_hz_, 0.5 * fs_);
            continue;
        }
        add_channel(off, true);
    }
}

NdbChannel *NdbDecoder::add_channel(double offset_hz, bool pinned, int id)
{
    const bool visit = id != 0;
    channels_.push_back(std::make_unique<NdbChannel>(visit ? id : next_id_++, center_hz_, offset_hz, bank_, pinned, now_,
                                                     cfg_.ggmorse == DecoderConfig::Ggmorse::All));
    NdbChannel *ch = channels_.back().get();
    ch->on_decode = [this, ch](const std::string &s) {
        if (on_decode) on_decode(ch->id(), center_hz_ + ch->offset_hz(), s);
    };
    // Revisits are routine; only their outcome is logged.
    if (!visit)
        fprintf(stderr, "ndb: + ch%d %.1f Hz%s\n", ch->id(), center_hz_ + offset_hz, pinned ? " (pinned)" : "");
    return ch;
}

void NdbDecoder::remove_channel(NdbChannel *ch)
{
    ggm_since_.erase(ch->id());
    ggm_resting_.erase(ch->id());
    channels_.erase(std::find_if(channels_.begin(), channels_.end(),
                                 [&](const std::unique_ptr<NdbChannel> &c) { return c.get() == ch; }));
}

NdbChannel *NdbDecoder::channel(int id)
{
    for (auto &ch : channels_)
        if (ch->id() == id) return ch.get();
    return nullptr;
}

NdbDecoder::Tracked *NdbDecoder::tracked_for(int id)
{
    for (auto &t : tracked_)
        if (t.id == id) return &t;
    return nullptr;
}

bool NdbDecoder::accepted(const ChannelSnapshot &s) const
{
    if (s.ident.empty()) return false;
    return accept_ident ? accept_ident(center_hz_ + s.offset_hz, s.ident, s.ident_count) : s.ident_count >= 3;
}

void NdbDecoder::set_assist(const std::vector<double> &abs_hz)
{
    std::vector<double> off;
    for (double f : abs_hz)
        if (std::fabs(f - center_hz_) < 0.5 * fs_) off.push_back(f - center_hz_);
    det_.set_assist(off);
}

void NdbDecoder::set_known(const std::vector<double> &abs_hz)
{
    known_.clear();
    for (double f : abs_hz)
        if (std::fabs(f - center_hz_) < 0.5 * fs_) known_.push_back(f - center_hz_);
    std::sort(known_.begin(), known_.end());
    if (!cfg_.known_only || known_.empty()) return;
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (!(*it)->pinned() && !near_known((*it)->offset_hz())) {
            ggm_since_.erase((*it)->id());
            ggm_resting_.erase((*it)->id());
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    tracked_.erase(std::remove_if(tracked_.begin(), tracked_.end(),
                                  [&](const Tracked &t) { return !near_known(t.offset_hz); }),
                   tracked_.end());
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending &p) { return !near_known(p.offset_hz); }),
                   pending_.end());
}

bool NdbDecoder::near_known(double offset_hz) const
{
    auto it = std::lower_bound(known_.begin(), known_.end(), offset_hz - cfg_.known_tol_hz);
    return it != known_.end() && *it <= offset_hz + cfg_.known_tol_hz;
}

void NdbDecoder::process_iq(const int16_t *iq, size_t n_pairs)
{
    buf_.resize(n_pairs);
    constexpr float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < n_pairs; ++i)
        buf_[i] = cf(float(iq[2 * i]) * k, float(iq[2 * i + 1]) * k);

    samples_ += n_pairs;
    now_ = double(samples_) / fs_;

    if (det_.process(buf_.data(), n_pairs)) on_detection();
    if (channels_.empty()) return;
    bank_.process(buf_.data(), n_pairs);
    for (auto &ch : channels_) ch->process(bank_, now_);
}

void NdbDecoder::on_detection()
{
    const auto &found = det_.carriers();

    for (auto &p : pending_) p.hit_this_pass = false;

    for (const auto &c : found) {
        // A tracked beacon's carrier: follow it. It needs no channel.
        Tracked *tr = nullptr;
        double tbest = cfg_.match_hz;
        for (auto &t : tracked_) {
            double d = std::fabs(t.offset_hz - c.offset_hz);
            if (d < tbest) { tbest = d; tr = &t; }
        }
        if (tr) {
            tr->seen = now_;
            tr->snr_db = c.snr_db;
            tr->offset_hz = c.offset_hz;
        }

        // Existing channel?
        NdbChannel *match = nullptr;
        double best = cfg_.match_hz;
        for (auto &ch : channels_) {
            double d = std::fabs(ch->offset_hz() - c.offset_hz);
            if (d < best) { best = d; match = ch.get(); }
        }
        if (match) {
            match->seen(c.snr_db, now_);
            // Pinned channels stay where they were asked to be.
            if (!match->pinned() && std::fabs(match->offset_hz() - c.offset_hz) > cfg_.retune_hz)
                match->retune(c.offset_hz);
            continue;
        }
        if (tr || !cfg_.auto_detect) continue;
        // Only carriers where a known beacon is published, unless asked for all.
        if (cfg_.known_only && !known_.empty() && !near_known(c.offset_hz)) continue;

        // Pending candidate?
        auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending &p) {
            return std::fabs(p.offset_hz - c.offset_hz) < cfg_.match_hz;
        });
        if (it == pending_.end()) {
            pending_.push_back({c.offset_hz, 1, true, c.snr_db});
        } else {
            it->offset_hz = c.offset_hz;
            it->hits++;
            it->hit_this_pass = true;
            it->snr_db = c.snr_db;
        }
    }

    // Promote candidates seen on enough consecutive passes, strongest first.
    // A full house recycles a channel that has had its trial and shown no
    // keying. A carrier that was itself recycled recently waits out its
    // cooldown either way, so the queue rotates rather than thrashing.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending &p) { return !p.hit_this_pass; }),
                   pending_.end());
    std::sort(pending_.begin(), pending_.end(), [](const Pending &a, const Pending &b) { return a.snr_db > b.snr_db; });
    const auto cutoff = now_ - cfg_.cooldown_s;
    recycled_.erase(std::remove_if(recycled_.begin(), recycled_.end(), [&](const Recycled &r) { return r.at < cutoff; }),
                    recycled_.end());

    // Identified beacons first: finish their visits, free the slots of any
    // newly identified, and open the revisits that are due.
    service_visits();
    release_identified();
    schedule_visits();

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->hits < cfg_.confirm_passes) { ++it; continue; }
        // Free slot or not: revisits free one every few minutes, and the
        // strongest unkeyed carrier would otherwise take each of them.
        if (recently_recycled(it->offset_hz)) { ++it; continue; }
        if (int(channels_.size()) >= cfg_.max_channels) {
            NdbChannel *victim = recyclable();
            if (!victim) break;
            fprintf(stderr, "ndb: ~ ch%d %.1f Hz recycled (no keying)\n", victim->id(), center_hz_ + victim->offset_hz());
            recycled_.push_back({victim->offset_hz(), now_});
            remove_channel(victim);
        }
        NdbChannel *ch = add_channel(it->offset_hz, false);
        ch->seen(it->snr_db, now_);
        it = pending_.erase(it);
    }

    assign_ggmorse();

    // Drop channels whose carrier has gone. One that has been identified is
    // kept through much longer fades: its ident tally is the accumulated work
    // of every clean copy so far, and a weak beacon dipping under the
    // detection threshold for a while has not gone anywhere.
    for (auto it = channels_.begin(); it != channels_.end();) {
        const auto &ch = *it;
        const double unseen = now_ - ch->last_seen_s();
        const double limit = unseen > cfg_.drop_after_s && !ch->snapshot(now_).ident.empty()
                                 ? cfg_.drop_identified_after_s : cfg_.drop_after_s;
        if (!ch->pinned() && unseen > limit) {
            fprintf(stderr, "ndb: - ch%d %.1f Hz (carrier lost)\n", ch->id(), center_hz_ + ch->offset_hz());
            ggm_since_.erase(ch->id());
            ggm_resting_.erase(ch->id());
            it = channels_.erase(it);
            continue;
        }
        ++it;
    }
}

// End revisits that have copied the ident, found a different one, or run out
// of time, and forget tracked beacons that have gone.
void NdbDecoder::service_visits()
{
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        Tracked &t = *it;
        const double f = center_hz_ + t.offset_hz;
        if (!t.visiting) {
            const bool gone = now_ - t.seen > cfg_.drop_identified_after_s;
            if (t.misses >= cfg_.lost_misses || now_ - t.verified > cfg_.lost_after_s || gone) {
                fprintf(stderr, "ndb: - ch%d %.1f Hz %s lost (%s)\n", t.id, f, t.ident.c_str(),
                        gone ? "carrier gone" : t.misses >= cfg_.lost_misses ? "revisits copied nothing"
                                                                              : "not copied for too long");
                it = tracked_.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        NdbChannel *ch = channel(t.id);
        if (!ch) {
            // The carrier faded mid-visit (or the known list changed): not a miss.
            t.visiting = false;
            t.next_visit = now_ + cfg_.visit_retry_s;
            ++it;
            continue;
        }
        const auto s = ch->snapshot(now_);
        const int n = ch->copies(t.ident, now_);
        if (n >= cfg_.visit_copies) {
            fprintf(stderr, "ndb: = ch%d %.1f Hz %s reconfirmed in %.0f s\n", t.id, f, t.ident.c_str(),
                    now_ - t.visit_start);
            t.verified = now_;
            t.misses = 0;
            t.copies = std::max(t.copies, n);
            t.visiting = false;
            t.next_visit = now_ + cfg_.revisit_s;
            t.last = s;
            remove_channel(ch);
        } else if (!s.ident.empty() && s.ident != t.ident && accepted(s)) {
            // Another beacon has the frequency now. The channel carries on as
            // an ordinary one, and release_identified() tracks it afresh.
            fprintf(stderr, "ndb: ! ch%d %.1f Hz is now %s, not %s\n", t.id, f, s.ident.c_str(), t.ident.c_str());
            it = tracked_.erase(it);
            continue;
        } else if (now_ - t.visit_start > cfg_.visit_timeout_s) {
            ++t.misses;
            fprintf(stderr, "ndb: ? ch%d %.1f Hz %s not copied on revisit (%d/%d)\n", t.id, f, t.ident.c_str(),
                    t.misses, cfg_.lost_misses);
            t.visiting = false;
            t.next_visit = now_ + cfg_.visit_retry_s;
            remove_channel(ch);
        }
        ++it;
    }
}

// An identified beacon has nothing more to decode: give its slot up and track
// its carrier instead. Pinned channels keep theirs, as asked.
void NdbDecoder::release_identified()
{
    for (size_t i = 0; i < channels_.size();) {
        NdbChannel *ch = channels_[i].get();
        if (ch->pinned() || tracked_for(ch->id())) { ++i; continue; }
        const auto s = ch->snapshot(now_);
        if (!accepted(s)) { ++i; continue; }
        Tracked t;
        t.id = ch->id();
        t.offset_hz = ch->offset_hz();
        t.ident = s.ident;
        t.copies = s.ident_count;
        t.verified = now_ - std::max(0.0, s.ident_age_s);
        t.seen = ch->last_seen_s();
        t.snr_db = s.snr_db;
        t.next_visit = t.verified + cfg_.revisit_s;
        t.created = ch->created_s();
        t.last = s;
        fprintf(stderr, "ndb: > ch%d %.1f Hz %s identified, slot released\n", t.id, center_hz_ + t.offset_hz,
                t.ident.c_str());
        tracked_.push_back(std::move(t));
        remove_channel(ch);
    }
}

// Open the revisits that are due, most overdue first. A free slot goes to a
// revisit before any waiting carrier; with none free, one overdue by
// revisit_grace_s takes the slot of the unidentified channel that has held
// one longest. Only a carrier the detector sees now is worth a visit.
void NdbDecoder::schedule_visits()
{
    std::vector<Tracked *> due;
    for (auto &t : tracked_)
        if (!t.visiting && now_ >= t.next_visit && now_ - t.seen < 60.0) due.push_back(&t);
    std::sort(due.begin(), due.end(), [](const Tracked *a, const Tracked *b) { return a->next_visit < b->next_visit; });
    for (Tracked *t : due) {
        if (int(channels_.size()) >= cfg_.max_channels) {
            if (now_ - t->next_visit < cfg_.revisit_grace_s) break;   // the rest are less overdue
            NdbChannel *victim = nullptr;
            for (auto &ch : channels_)
                if (!ch->pinned() && !tracked_for(ch->id()) && (!victim || ch->created_s() < victim->created_s()))
                    victim = ch.get();
            if (!victim) break;
            fprintf(stderr, "ndb: ~ ch%d %.1f Hz recycled (revisit of %s overdue)\n", victim->id(),
                    center_hz_ + victim->offset_hz(), t->ident.c_str());
            recycled_.push_back({victim->offset_hz(), now_});
            remove_channel(victim);
        }
        add_channel(t->offset_hz, false, t->id)->seen(t->snr_db, now_);
        t->visiting = true;
        t->visit_start = now_;
    }
}

// Auto mode: hand the ggmorse pool to the channels a second decoder can help.
void NdbDecoder::assign_ggmorse()
{
    if (cfg_.ggmorse != DecoderConfig::Ggmorse::Auto) return;
    struct Cand { NdbChannel *ch; float contrast; };
    std::vector<Cand> cands;
    int in_use = 0;
    for (auto &chp : channels_) {
        NdbChannel *ch = chp.get();
        const auto s = ch->snapshot(now_);
        if (ch->ggmorse()) {
            const double held = now_ - ggm_since_[ch->id()];
            const bool done = !s.ident.empty();
            const bool faded = s.contrast_db < cfg_.ggm_min_contrast_db - 2.0f && s.periodicity < cfg_.ggm_min_periodicity;
            const bool gave_up = held > cfg_.ggm_hold_s || faded;
            if (done || gave_up) {
                ch->set_ggmorse(false);
                ggm_since_.erase(ch->id());
                if (!done) ggm_resting_[ch->id()] = now_;
                continue;
            }
            ++in_use;
            continue;
        }
        const bool evidence = s.contrast_db >= cfg_.ggm_min_contrast_db || s.periodicity >= cfg_.ggm_min_periodicity;
        if (!s.ident.empty() || s.age_s < cfg_.ggm_min_age_s || !evidence) continue;
        auto r = ggm_resting_.find(ch->id());
        if (r != ggm_resting_.end()) {
            if (now_ - r->second < cfg_.ggm_cooldown_s) continue;
            ggm_resting_.erase(r);
        }
        cands.push_back({ch, s.contrast_db});
    }
    // Most keying-like first.
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.contrast > b.contrast; });
    for (const auto &c : cands) {
        if (in_use >= cfg_.ggm_slots) break;
        c.ch->set_ggmorse(true);
        ggm_since_[c.ch->id()] = now_;
        ++in_use;
    }
}

bool NdbDecoder::recently_recycled(double offset_hz) const
{
    for (const auto &r : recycled_)
        if (std::fabs(r.offset_hz - offset_hz) < cfg_.match_hz) return true;
    return false;
}

// The channel that has gone longest without keying, if any is due: not
// pinned, not identified, and either never keyed in its trial_s, or silent
// for active_hold_s since it last did.
NdbChannel *NdbDecoder::recyclable()
{
    NdbChannel *best = nullptr;
    double best_idle = 0.0;
    for (auto &ch : channels_) {
        if (ch->pinned() || tracked_for(ch->id()) || now_ - ch->created_s() < cfg_.trial_s) continue;
        const bool ever = ch->last_active_s() >= 0;
        const double idle = now_ - (ever ? ch->last_active_s() : ch->created_s());
        if (idle < (ever ? cfg_.active_hold_s : cfg_.trial_s) || !ch->snapshot(now_).ident.empty()) continue;
        if (idle > best_idle) { best_idle = idle; best = ch.get(); }
    }
    return best;
}

std::vector<ChannelSnapshot> NdbDecoder::snapshot() const
{
    std::vector<ChannelSnapshot> out;
    out.reserve(channels_.size() + tracked_.size());
    auto fresh = [&](ChannelSnapshot &s, const Tracked &t) {
        s.ident = t.ident;
        s.ident_count = std::max(s.ident_count, t.copies);
        s.ident_age_s = now_ - t.verified;
        s.ident_fresh = s.ident_age_s < fresh_s();
        s.age_s = now_ - t.created;
    };
    for (const auto &ch : channels_) {
        out.push_back(ch->snapshot(now_));
        auto &s = out.back();
        for (const auto &t : tracked_) {
            if (t.id != ch->id()) continue;
            // A revisit: show the remembered ident while it re-copies it.
            s.visit = true;
            if (s.ident.empty() || s.ident == t.ident) fresh(s, t);
        }
    }
    for (const auto &t : tracked_) {
        if (t.visiting) continue;
        ChannelSnapshot s = t.last;
        s.id = t.id;
        s.offset_hz = t.offset_hz;
        s.freq_hz = center_hz_ + t.offset_hz;
        s.snr_db = t.snr_db;
        s.ggmorse = false;
        s.tracking = true;
        s.last_seen_s = now_ - t.seen;
        s.last_text_s = now_ - t.verified;
        s.ident_count = 0;
        fresh(s, t);
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const ChannelSnapshot &a, const ChannelSnapshot &b) { return a.freq_hz < b.freq_hz; });
    return out;
}

void NdbDecoder::spectrum(size_t points, std::vector<float> &db, std::vector<float> &floor_db) const
{
    det_.spectrum(points, db, floor_db);
}

}  // namespace ndb
