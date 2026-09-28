#include "ndb_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ndb {

std::string NdbDecoder::check_sample_rate(double fs)
{
    // Each channel decimates to 16 kHz and then 4 kHz by integer factors.
    if (fs < 16000.0 || std::fmod(fs, 16000.0) != 0.0)
        return "sample rate " + std::to_string(int(fs)) +
               " Hz is not a multiple of 16 kHz; use iq48, iq96, iq192 or iq384";
    return "";
}

NdbDecoder::NdbDecoder(double center_hz, double sample_rate, DecoderConfig cfg)
    : center_hz_(center_hz), fs_(sample_rate), cfg_(std::move(cfg)), det_(sample_rate, cfg_.detector)
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

NdbChannel *NdbDecoder::add_channel(double offset_hz, bool pinned)
{
    channels_.push_back(std::make_unique<NdbChannel>(next_id_++, center_hz_, offset_hz, fs_, pinned, now_));
    NdbChannel *ch = channels_.back().get();
    ch->on_decode = [this, ch](const std::string &s) {
        if (on_decode) on_decode(ch->id(), center_hz_ + ch->offset_hz(), s);
    };
    fprintf(stderr, "ndb: + ch%d %.1f Hz%s\n", channels_.back()->id(), center_hz_ + offset_hz,
            pinned ? " (pinned)" : "");
    return channels_.back().get();
}

void NdbDecoder::set_assist(const std::vector<double> &abs_hz)
{
    std::vector<double> off;
    for (double f : abs_hz)
        if (std::fabs(f - center_hz_) < 0.5 * fs_) off.push_back(f - center_hz_);
    det_.set_assist(off);
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
    for (auto &ch : channels_) ch->process(buf_.data(), n_pairs, now_);
}

void NdbDecoder::on_detection()
{
    const auto &found = det_.carriers();

    for (auto &p : pending_) p.hit_this_pass = false;

    for (const auto &c : found) {
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
        if (!cfg_.auto_detect) continue;

        // Pending candidate?
        auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending &p) {
            return std::fabs(p.offset_hz - c.offset_hz) < cfg_.match_hz;
        });
        if (it == pending_.end()) {
            pending_.push_back({c.offset_hz, 1, true});
        } else {
            it->offset_hz = c.offset_hz;
            it->hits++;
            it->hit_this_pass = true;
        }
    }

    // Promote candidates seen on enough consecutive passes, up to the cap.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!it->hit_this_pass) { it = pending_.erase(it); continue; }
        if (it->hits >= cfg_.confirm_passes && int(channels_.size()) < cfg_.max_channels) {
            NdbChannel *ch = add_channel(it->offset_hz, false);
            for (const auto &c : found)
                if (std::fabs(c.offset_hz - it->offset_hz) < 0.5) ch->seen(c.snr_db, now_);
            it = pending_.erase(it);
            continue;
        }
        ++it;
    }

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
            it = channels_.erase(it);
            continue;
        }
        ++it;
    }
}

std::vector<ChannelSnapshot> NdbDecoder::snapshot() const
{
    std::vector<ChannelSnapshot> out;
    out.reserve(channels_.size());
    for (const auto &ch : channels_) out.push_back(ch->snapshot(now_));
    std::sort(out.begin(), out.end(),
              [](const ChannelSnapshot &a, const ChannelSnapshot &b) { return a.freq_hz < b.freq_hz; });
    return out;
}

void NdbDecoder::spectrum(size_t points, std::vector<float> &db, std::vector<float> &floor_db) const
{
    det_.spectrum(points, db, floor_db);
}

}  // namespace ndb
