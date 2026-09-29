#include "fold_decoder.h"

#include "keying_decoder.h"

#include <algorithm>
#include <cmath>

namespace ndb {

namespace {

constexpr double kStepMs     = 10.0;   // envelope sample spacing
constexpr size_t kWindow     = 6000;   // 60 s per copy
constexpr int    kMinLag     = 200;    // 2 s: shortest cycle looked for
constexpr int    kMaxLag     = 3000;   // 30 s: ICAO's longest
constexpr int    kMinRepeats = 3;
constexpr int    kSmooth[]   = {1, 3, 5, 7};   // profile smoothing tried, samples
constexpr double kMinUnitMs  = 50.0;   // 24 wpm
constexpr double kMaxUnitMs  = 250.0;  // ~5 wpm
// Timing fit, as |log(measured / nearest of 1 or 3 units)|: the mean over
// every mark and space, and the worst one. Clean beacons fit to ~0.05 mean
// and ~0.15 worst; noise fits no unit (~0.5 and ~1.4).
constexpr double kMaxMeanErr  = 0.12;
constexpr double kMaxWorstErr = 0.35;

struct Run { bool on; double ms; };

// Read one averaged ident cycle. Returns "" unless it is clean keying.
std::string read_cycle(const std::vector<float> &prof, double &err_out)
{
    const size_t n = prof.size();
    std::vector<float> s(prof);
    auto pct = [&](double p) {
        auto it = s.begin() + long(p * double(n - 1));
        std::nth_element(s.begin(), it, s.end());
        return *it;
    };
    const float lo = pct(0.20), hi = pct(0.95);
    if (!(hi > lo)) return "";
    const float thr = lo + 0.45f * (hi - lo);

    // Runs around the circle, starting at a mark's leading edge.
    size_t start = n;
    for (size_t i = 0; i < n; ++i)
        if (prof[i] > thr && !(prof[(i + n - 1) % n] > thr)) {
            start = i;
            break;
        }
    if (start == n) return "";
    std::vector<Run> runs;
    for (size_t k = 0; k < n; ++k) {
        const bool on = prof[(start + k) % n] > thr;
        if (runs.empty() || runs.back().on != on) runs.push_back({on, 0.0});
        runs.back().ms += kStepMs;
    }
    // Rotate so the longest space (the gap between idents) comes last.
    size_t gap = 0;
    for (size_t i = 0; i < runs.size(); ++i)
        if (!runs[i].on && (runs[gap].on || runs[i].ms > runs[gap].ms)) gap = i;
    std::rotate(runs.begin(), runs.begin() + long(gap) + 1, runs.end());
    size_t marks = 0;
    for (const auto &r : runs) marks += r.on;
    if (marks < 2) return "";

    // Unit length: the one every mark and space (but the gap) fits best to
    // one or three units.
    auto fit = [](double ms, double u) {
        return std::min(std::fabs(std::log(ms / u)), std::fabs(std::log(ms / (3.0 * u))));
    };
    double best_u = 0.0, best_mean = 1e9, best_worst = 0.0;
    for (double u = kMinUnitMs; u <= kMaxUnitMs; u += 1.0) {
        double sum = 0.0, worst = 0.0;
        for (size_t i = 0; i + 1 < runs.size(); ++i) {
            const double e = fit(runs[i].ms, u);
            sum += e;
            worst = std::max(worst, e);
        }
        const double mean = sum / double(runs.size() - 1);
        if (mean < best_mean) best_u = u, best_mean = mean, best_worst = worst;
    }
    err_out = best_mean;
    if (best_mean > kMaxMeanErr || best_worst > kMaxWorstErr || runs.back().ms < 5.0 * best_u) return "";

    const auto &table = morse_table();
    std::string out, code;
    auto letter = [&]() {
        auto it = table.find(code);
        code.clear();
        if (it == table.end()) return false;
        out += it->second;
        return true;
    };
    for (size_t i = 0; i + 1 < runs.size(); ++i) {
        if (runs[i].on) code += runs[i].ms < 2.0 * best_u ? '.' : '-';
        else if (runs[i].ms >= 2.0 * best_u && !letter()) return "";
    }
    if (!letter()) return "";
    return out;
}

}  // namespace

void FoldDecoder::push(float e)
{
    buf_.push_back(e);
    if (buf_.size() < kWindow) return;
    analyse();
    buf_.clear();
}

void FoldDecoder::analyse()
{
    const size_t n = buf_.size();
    double mean = 0.0;
    for (float v : buf_) mean += v;
    mean /= double(n);
    std::vector<float> y(n);
    double den = 0.0;
    for (size_t i = 0; i < n; ++i) {
        y[i] = float(buf_[i] - mean);
        den += double(y[i]) * y[i];
    }
    r_ = 0.0f;
    cycle_s_ = 0.0;
    if (den <= 0.0) return;

    // Autocorrelation over the lags a cycle can have, scaled for the overlap
    // shrinking as the lag grows.
    const int max_lag = std::min<int>(kMaxLag, int(n / 2));
    std::vector<double> r(size_t(max_lag) + 2, 0.0);
    for (int L = kMinLag - 1; L <= max_lag + 1 && size_t(L) < n; ++L) {
        double acc = 0.0;
        for (size_t i = 0; i + size_t(L) < n; ++i) acc += double(y[i]) * y[i + size_t(L)];
        if (size_t(L) < r.size()) r[size_t(L)] = acc / den * double(n) / double(n - size_t(L));
    }
    int lag = kMinLag;
    for (int L = kMinLag; L <= max_lag; ++L)
        if (r[size_t(L)] > r[size_t(lag)]) lag = L;
    // A cycle also matches at twice (three times...) itself; prefer the
    // shortest lag that matches nearly as well.
    for (int d : {4, 3, 2}) {
        const int l2 = lag / d;
        if (l2 - 3 < kMinLag) continue;
        int k = l2 - 3;
        for (int L = l2 - 3; L <= l2 + 3; ++L)
            if (r[size_t(L)] > r[size_t(k)]) k = L;
        if (r[size_t(k)] >= 0.8 * r[size_t(lag)]) {
            lag = k;
            break;
        }
    }
    r_ = float(r[size_t(lag)]);
    double period = lag;
    const double y0 = r[size_t(lag) - 1], y1 = r[size_t(lag)], y2 = r[size_t(lag) + 1];
    const double d2 = y0 - 2.0 * y1 + y2;
    if (lag < max_lag && d2 < 0.0) period += std::clamp(0.5 * (y0 - y2) / d2, -0.5, 0.5);
    cycle_s_ = period * kStepMs / 1000.0;
    if (r_ < kMinPeriodicity) return;

    const size_t len = size_t(period);
    const int reps = int(double(n - 1 - len) / period) + 1;
    if (reps < kMinRepeats) return;
    std::vector<float> prof(len, 0.0f);
    for (int k = 0; k < reps; ++k) {
        const size_t o = size_t(std::lround(double(k) * period));
        for (size_t j = 0; j < len && o + j < n; ++j) prof[j] += buf_[o + j];
    }

    // Smooth (circularly) by each width in turn; keep the cleanest reading.
    std::string best;
    double best_err = 1e9;
    std::vector<float> sm(len);
    for (int w : kSmooth) {
        for (size_t j = 0; j < len; ++j) {
            float acc = 0.0f;
            for (int k = -(w / 2); k <= w / 2; ++k) acc += prof[(j + len + size_t(k + int(len))) % len];
            sm[j] = acc;
        }
        double err = 1e9;
        std::string txt = read_cycle(sm, err);
        if (!txt.empty() && err < best_err) best = txt, best_err = err;
    }
    if (!best.empty() && on_text) on_text(best + " ");
}

}  // namespace ndb
