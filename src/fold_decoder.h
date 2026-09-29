// fold_decoder.h — copy a weak NDB by averaging its ident over many repeats.
//
// An NDB sends the same ident on a fixed cycle (every 4-10 s, typically),
// forever. KeyingDecoder reads each repeat on its own, so it needs every one
// to stand out of the noise: 15 dB of on/off contrast. Averaging n repeats
// cuts the noise by sqrt(n), and 60 s holds 6-15 of them, so a beacon too
// weak to copy once can still be copied from the average.
//
// Each 60 s of tone envelope (KeyingDecoder's 20 ms envelope, every 10 ms):
//
//   - the cycle: the lag, 2-30 s, where the envelope best matches itself
//     (autocorrelation), preferring a shorter lag that divides it and matches
//     nearly as well. Noise matches itself at no lag (r ~0.05); a beacon at
//     its cycle does, strongly (0.3 at the edge of copy, ~1 when strong);
//   - the average: the window cut into cycles and averaged;
//   - the copy: that average smoothed (the smoothing that fits best, up to
//     70 ms), split into marks and spaces at 45% from floor to peak, starting
//     after the longest space (the gap between idents), and read with the unit
//     length that best fits every mark and space to one or three units.
//
// A copy is only given out when it looks like machine keying: the cycle is
// clear (r >= 0.2), every mark and space is within ~40% of one or three units,
// on average within 12%, and the gap between idents is at least 5 units.
// Noise, folded, gives nothing that passes. Each window is a new copy, from
// audio no other copy saw, so copies from here count in the ident tally like
// any other.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ndb {

class FoldDecoder {
public:
    // One tone envelope sample every 10 ms.
    void push(float e);
    // Start again (the envelope before this is unrelated to what follows).
    void reset() { buf_.clear(); }

    // An ident copied from the average, as letters then a space.
    std::function<void(const std::string &)> on_text;

    static constexpr float kMinPeriodicity = 0.2f;   // below this, no cycle (noise ~0.05)

    float periodicity() const { return r_; }   // of the last window: autocorrelation at the cycle
    double cycle_s() const { return cycle_s_; }

private:
    void analyse();

    std::vector<float> buf_;
    float r_ = 0.0f;
    double cycle_s_ = 0.0;
};

}  // namespace ndb
