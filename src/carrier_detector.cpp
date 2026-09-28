#include "carrier_detector.h"

#include <algorithm>
#include <cmath>

namespace ndb {

CarrierDetector::CarrierDetector(double sample_rate, DetectorConfig cfg)
    : fs_(sample_rate),
      cfg_(cfg),
      // ~3 Hz bins: fine enough that a carrier is a clean line, and the noise
      // in each bin low enough that a weak one clears the floor after a few
      // frames of averaging. 96 kHz → 32768 points, 2.93 Hz, 0.34 s per frame.
      n_(next_pow2(size_t(sample_rate / 3.0))),
      fft_(n_),
      window_(n_),
      buf_(n_),
      avg_(n_, 0.0f),
      db_(n_, -200.0f),
      floor_db_(n_, -200.0f)
{
    for (size_t i = 0; i < n_; ++i)
        window_[i] = float(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n_)));
}

bool CarrierDetector::process(const cf *x, size_t n)
{
    bool detected = false;
    for (size_t i = 0; i < n; ++i) {
        buf_[fill_] = x[i] * window_[fill_];
        if (++fill_ < n_) continue;
        fill_ = 0;

        fft_.forward(buf_.data());
        const float a = frames_ == 0 ? 1.0f : cfg_.avg_alpha;
        for (size_t k = 0; k < n_; ++k) {
            float p = std::norm(buf_[k]);
            avg_[k] += a * (p - avg_[k]);
        }
        ++frames_;

        if (frames_ >= 3 && frames_ % uint64_t(cfg_.detect_every) == 0) {
            run_detection();
            detected = true;
        }
    }
    return detected;
}

// Noise floor: the 25th percentile of each ~1 kHz segment, interpolated
// linearly between segment centres. A percentile rather than a mean so that
// the carriers and sidebands inside a segment do not lift it; per-segment so
// that it follows the slope of the band (LF noise rises steeply towards the
// bottom, and the edges of the IQ passband roll off).
void CarrierDetector::compute_floor()
{
    const size_t half = n_ / 2;
    for (size_t k = 0; k < n_; ++k) {
        // FFT shift: index 0 of db_ is -fs/2.
        float p = avg_[(k + half) % n_];
        db_[k] = 10.0f * std::log10(p + 1e-20f);
    }

    const size_t seg = std::max<size_t>(32, size_t(1000.0 / bin_hz()));
    const size_t nseg = (n_ + seg - 1) / seg;
    std::vector<float> seg_floor(nseg), tmp;
    std::vector<double> seg_centre(nseg);
    for (size_t s = 0; s < nseg; ++s) {
        size_t a = s * seg, b = std::min(n_, a + seg);
        tmp.assign(db_.begin() + a, db_.begin() + b);
        auto q = tmp.begin() + tmp.size() / 4;
        std::nth_element(tmp.begin(), q, tmp.end());
        seg_floor[s] = *q;
        seg_centre[s] = 0.5 * double(a + b - 1);
    }
    for (size_t k = 0; k < n_; ++k) {
        double pos = double(k);
        if (pos <= seg_centre[0]) { floor_db_[k] = seg_floor[0]; continue; }
        if (pos >= seg_centre[nseg - 1]) { floor_db_[k] = seg_floor[nseg - 1]; continue; }
        size_t s = size_t((pos - seg_centre[0]) / double(seg));
        s = std::min(s, nseg - 2);
        double t = (pos - seg_centre[s]) / (seg_centre[s + 1] - seg_centre[s]);
        floor_db_[k] = float(seg_floor[s] + t * (seg_floor[s + 1] - seg_floor[s]));
    }
}

void CarrierDetector::set_assist(const std::vector<double> &offsets_hz)
{
    assist_mask_.assign(n_, 0);
    const double bin = bin_hz();
    const long half = long(n_ / 2), tol = std::max(1L, long(cfg_.assist_tol_hz / bin));
    for (double off : offsets_hz) {
        long k = long(std::lround(off / bin)) + half;
        for (long j = k - tol; j <= k + tol; ++j)
            if (j >= 0 && j < long(n_)) assist_mask_[j] = 1;
    }
}

void CarrierDetector::run_detection()
{
    compute_floor();

    const double bin = bin_hz();
    const long half = long(n_ / 2);
    const long W = std::max(2L, long(cfg_.peak_halfwidth_hz / bin));
    const long edge = long(cfg_.edge_fraction * fs_ / bin);
    // Narrowness test: a carrier's power is in a couple of bins (Hann main
    // lobe ±2 bins). Measure the shoulders 20-40 Hz out, which excludes the
    // main lobe but is well inside the nearest tone sideband. A wideband
    // signal (DGPS MSK, broadcast AM programme) has no such cliff.
    const long sh0 = std::max(3L, long(20.0 / bin));
    const long sh1 = std::max(sh0 + 2, long(40.0 / bin));

    auto snr_at = [&](long k) -> float {
        if (k < 0 || k >= long(n_)) return -100.0f;
        return db_[k] - floor_db_[k];
    };

    std::vector<Carrier> found;
    for (long k = half - edge; k <= half + edge; ++k) {
        if (k - W < 0 || k + W >= long(n_)) continue;
        float snr = db_[k] - floor_db_[k];
        const float thresh = (!assist_mask_.empty() && assist_mask_[k]) ? cfg_.assist_snr_db : cfg_.snr_threshold_db;
        if (snr < thresh) continue;
        bool is_max = true;
        for (long j = k - W; j <= k + W && is_max; ++j)
            if (j != k && (db_[j] > db_[k] || (db_[j] == db_[k] && j < k))) is_max = false;
        if (!is_max) continue;

        double shoulder = 0.0;
        int nsh = 0;
        for (long j = sh0; j <= sh1; ++j) {
            shoulder += db_[k - j] + db_[k + j];
            nsh += 2;
        }
        shoulder /= nsh;
        if (db_[k] - shoulder < 6.0) continue;

        // Parabolic interpolation on the dB values for a sub-bin estimate.
        double y0 = db_[k - 1], y1 = db_[k], y2 = db_[k + 1];
        double den = y0 - 2.0 * y1 + y2;
        double frac = (den != 0.0) ? 0.5 * (y0 - y2) / den : 0.0;
        frac = std::clamp(frac, -0.5, 0.5);

        Carrier c;
        c.offset_hz = (double(k - half) + frac) * bin;
        c.level_db = db_[k];
        c.snr_db = snr;
        found.push_back(c);
    }

    // Strongest first, then drop anything that looks like one sideband of a
    // stronger carrier: 250-1300 Hz away, with a matching peak on the mirror
    // side of that carrier. Tones are 400 or 1020 Hz nominally but vary.
    std::sort(found.begin(), found.end(),
              [](const Carrier &a, const Carrier &b) { return a.level_db > b.level_db; });
    std::vector<Carrier> accepted;
    for (const auto &c : found) {
        bool sideband = false;
        for (const auto &s : accepted) {
            double d = c.offset_hz - s.offset_hz;
            double ad = std::fabs(d);
            // Close to a much stronger carrier: whatever this is — sideband
            // harmonic, spur, or a genuine weak beacon — its channel would
            // only decode the strong one, whose carrier or tone sidebands fall
            // inside its ±1.3 kHz passband or the transition band above it.
            if (ad <= cfg_.neighbour_hz && s.level_db - c.level_db >= cfg_.neighbour_margin_db) { sideband = true; break; }
            if (ad < 250.0 || ad > 1300.0) continue;
            long mirror = long(std::lround((s.offset_hz - d) / bin)) + half;
            float best = -100.0f;
            for (long j = mirror - 3; j <= mirror + 3; ++j) best = std::max(best, snr_at(j));
            if (best >= std::min(cfg_.snr_threshold_db - 4.0f, c.snr_db - 3.0f)) { sideband = true; break; }
        }
        if (!sideband) accepted.push_back(c);
    }
    std::sort(accepted.begin(), accepted.end(),
              [](const Carrier &a, const Carrier &b) { return a.offset_hz < b.offset_hz; });
    carriers_ = std::move(accepted);
}

void CarrierDetector::spectrum(size_t points, std::vector<float> &db, std::vector<float> &floor_db) const
{
    const size_t group = std::max<size_t>(1, (n_ + points - 1) / points);
    const size_t m = n_ / group;
    db.assign(m, -200.0f);
    floor_db.assign(m, -200.0f);
    for (size_t i = 0; i < m; ++i) {
        float mx = -200.0f, fl = 0.0f;
        for (size_t j = 0; j < group; ++j) {
            mx = std::max(mx, db_[i * group + j]);
            fl += floor_db_[i * group + j];
        }
        db[i] = mx;
        floor_db[i] = fl / float(group);
    }
}

}  // namespace ndb
