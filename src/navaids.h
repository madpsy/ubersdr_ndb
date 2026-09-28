// navaids.h — the OurAirports NDB list, for naming what the decoder hears.
//
// Source: https://davidmegginson.github.io/ourairports-data/navaids.csv
// (public domain). The Docker build downloads it into the image; ./build.sh
// fetches it next to the binary. Only NDB and NDB-DME rows are kept.
//
// Two uses:
//   - candidates(): every published NDB within a few hundred Hz of a carrier,
//     nearest the receiver first — a best guess before any ident is copied;
//   - match(): the candidate whose ident agrees with the decoded one, which
//     turns a decode into a named, located beacon.

#pragma once

#include <string>
#include <vector>

namespace ndb {

struct Navaid {
    std::string ident;
    std::string name;
    std::string country;   // ISO 3166-1 alpha-2
    double freq_hz = 0.0;
    double lat = 0.0, lon = 0.0;
    std::string power;     // OurAirports' coarse class: LOW / MEDIUM / HIGH / ""
};

struct NavaidHit {
    const Navaid *nav = nullptr;
    double dist_km = -1.0;     // -1 when the receiver position is unknown
    double bearing_deg = -1.0;
};

class NavaidDb {
public:
    // Returns false (and leaves the db empty) if the file cannot be read.
    bool load(const std::string &path);

    size_t size() const { return navs_.size(); }
    const std::vector<Navaid> &all() const { return navs_; }

    void set_receiver(double lat, double lon) { rx_lat_ = lat; rx_lon_ = lon; have_rx_ = true; }

    // Only beacons within this distance of the receiver can be candidates or
    // matches (<= 0: no limit; ignored until the receiver is known). An ident
    // shared with a beacon on the far side of the world is far more likely a
    // misread than a copy of it.
    void set_max_km(double km) { max_km_ = km; }
    double max_km() const { return max_km_; }
    bool have_receiver() const { return have_rx_; }
    double rx_lat() const { return rx_lat_; }
    double rx_lon() const { return rx_lon_; }

    NavaidHit locate(const Navaid &n) const;

    // Published NDBs within tol_hz of freq_hz and within max_km, nearest first
    // (or closest in frequency first when the receiver position is unknown).
    std::vector<NavaidHit> candidates(double freq_hz, double tol_hz = 600.0) const;

    // The nearest candidate whose ident equals `ident`, or — if none — whose
    // ident starts with it and is one character longer (ggmorse often drops a
    // slow ident's last letter). `exact` says which. nav == nullptr if neither.
    NavaidHit match(double freq_hz, const std::string &ident, bool &exact, double tol_hz = 600.0) const;

private:
    std::vector<Navaid> navs_;  // sorted by frequency
    double rx_lat_ = 0.0, rx_lon_ = 0.0;
    bool have_rx_ = false;
    double max_km_ = 0.0;
};

}  // namespace ndb
