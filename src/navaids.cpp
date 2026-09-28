#include "navaids.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace ndb {

namespace {

// One CSV record, RFC 4180 quoting. OurAirports quotes every string field and
// names can contain commas, so a plain split is not enough.
std::vector<std::string> split_csv(const std::string &line)
{
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else q = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            q = true;
        } else if (c == ',') {
            out.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

constexpr double kDeg = 3.14159265358979323846 / 180.0;

}  // namespace

bool NavaidDb::load(const std::string &path)
{
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    auto hdr = split_csv(line);
    auto col = [&](const char *name) -> int {
        auto it = std::find(hdr.begin(), hdr.end(), name);
        return it == hdr.end() ? -1 : int(it - hdr.begin());
    };
    const int c_ident = col("ident"), c_name = col("name"), c_type = col("type"), c_freq = col("frequency_khz"),
              c_lat = col("latitude_deg"), c_lon = col("longitude_deg"), c_cc = col("iso_country"),
              c_pow = col("power");
    if (c_ident < 0 || c_type < 0 || c_freq < 0 || c_lat < 0 || c_lon < 0) return false;
    const int need = std::max({c_ident, c_name, c_type, c_freq, c_lat, c_lon, c_cc, c_pow});

    std::vector<Navaid> v;
    while (std::getline(f, line)) {
        auto r = split_csv(line);
        if (int(r.size()) <= need) continue;
        if (r[c_type] != "NDB" && r[c_type] != "NDB-DME") continue;
        if (r[c_freq].empty() || r[c_lat].empty() || r[c_lon].empty()) continue;
        Navaid n;
        n.ident = r[c_ident];
        n.name = c_name >= 0 ? r[c_name] : "";
        n.country = c_cc >= 0 ? r[c_cc] : "";
        n.power = c_pow >= 0 ? r[c_pow] : "";
        n.freq_hz = atof(r[c_freq].c_str()) * 1000.0;
        n.lat = atof(r[c_lat].c_str());
        n.lon = atof(r[c_lon].c_str());
        if (n.freq_hz <= 0) continue;
        v.push_back(std::move(n));
    }
    std::sort(v.begin(), v.end(), [](const Navaid &a, const Navaid &b) { return a.freq_hz < b.freq_hz; });
    navs_ = std::move(v);
    return true;
}

NavaidHit NavaidDb::locate(const Navaid &n) const
{
    NavaidHit h;
    h.nav = &n;
    if (!have_rx_) return h;
    // Haversine distance and initial great-circle bearing.
    const double p1 = rx_lat_ * kDeg, p2 = n.lat * kDeg, dl = (n.lon - rx_lon_) * kDeg;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    h.dist_km = 2.0 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    h.bearing_deg = std::fmod(std::atan2(y, x) / kDeg + 360.0, 360.0);
    return h;
}

std::vector<NavaidHit> NavaidDb::candidates(double freq_hz, double tol_hz) const
{
    std::vector<NavaidHit> out;
    auto lo = std::lower_bound(navs_.begin(), navs_.end(), freq_hz - tol_hz,
                               [](const Navaid &n, double f) { return n.freq_hz < f; });
    for (auto it = lo; it != navs_.end() && it->freq_hz <= freq_hz + tol_hz; ++it) out.push_back(locate(*it));
    std::sort(out.begin(), out.end(), [&](const NavaidHit &a, const NavaidHit &b) {
        if (have_rx_) return a.dist_km < b.dist_km;
        return std::fabs(a.nav->freq_hz - freq_hz) < std::fabs(b.nav->freq_hz - freq_hz);
    });
    return out;
}

NavaidHit NavaidDb::match(double freq_hz, const std::string &ident, bool &exact, double tol_hz) const
{
    exact = false;
    if (ident.empty()) return {};
    auto c = candidates(freq_hz, tol_hz);
    for (const auto &h : c)
        if (h.nav->ident == ident) { exact = true; return h; }
    for (const auto &h : c)
        if (h.nav->ident.size() == ident.size() + 1 && h.nav->ident.compare(0, ident.size(), ident) == 0) return h;
    return {};
}

}  // namespace ndb
