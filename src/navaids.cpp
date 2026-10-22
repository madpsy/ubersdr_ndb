#include "navaids.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>

namespace ndb {

namespace {

// The next CSV record, RFC 4180 quoting. OurAirports quotes every string field
// and names can contain commas, so a plain split is not enough; RXX's notes
// can contain line breaks, so a record can span lines.
bool next_record(std::istream &f, std::vector<std::string> &out)
{
    out.clear();
    std::string line, cur;
    bool q = false;
    if (!std::getline(f, line)) return false;
    for (;;) {
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
        if (!q || !std::getline(f, line)) break;
        cur += ' ';
    }
    out.push_back(std::move(cur));
    return true;
}

// Column index by header name, -1 if absent.
int column(const std::vector<std::string> &hdr, const char *name)
{
    auto it = std::find(hdr.begin(), hdr.end(), name);
    return it == hdr.end() ? -1 : int(it - hdr.begin());
}

// RXX gives countries as ITU / NDB List codes, the UI wants ISO 3166-1
// alpha-2 for its flags. Codes for parts of a country (ENG, SCT, COR, SAR,
// KAL ...) map to the country; unknown ones and oceans (XOE ...) to "".
const std::map<std::string, std::string> &itu_to_iso()
{
    static const std::map<std::string, std::string> m = {
        {"AFG", "AF"}, {"AFS", "ZA"}, {"AGL", "AO"}, {"ALG", "DZ"}, {"ALS", "US"}, {"ARG", "AR"}, {"ARM", "AM"},
        {"ARS", "SA"}, {"ATG", "AG"}, {"AUS", "AU"}, {"AUT", "AT"}, {"AZE", "AZ"}, {"AZR", "PT"}, {"BAL", "ES"},
        {"BDI", "BI"}, {"BGD", "BD"}, {"BHR", "BH"}, {"BIH", "BA"}, {"BLR", "BY"}, {"BOL", "BO"}, {"BOT", "BW"},
        {"BRA", "BR"}, {"BRM", "MM"}, {"BRU", "BN"}, {"BUL", "BG"}, {"CAN", "CA"}, {"CBG", "KH"}, {"CEU", "ES"},
        {"CHL", "CL"}, {"CHN", "CN"}, {"CKS", "CK"}, {"CLM", "CO"}, {"CLN", "LK"}, {"CNR", "ES"}, {"COD", "CD"},
        {"COR", "FR"}, {"CPV", "CV"}, {"CUB", "CU"}, {"CYM", "KY"}, {"CYP", "CY"}, {"CZE", "CZ"}, {"DEU", "DE"},
        {"DGA", "IO"}, {"DNK", "DK"}, {"EGY", "EG"}, {"ENG", "GB"}, {"EQA", "EC"}, {"ESP", "ES"}, {"ETH", "ET"},
        {"FIN", "FI"}, {"FJI", "FJ"}, {"FLK", "FK"}, {"FRA", "FR"}, {"FRO", "FO"}, {"FSM", "FM"}, {"GAL", "EC"},
        {"GDL", "GP"}, {"GEO", "GE"}, {"GRC", "GR"}, {"GRD", "GD"}, {"GRL", "GL"}, {"GSY", "GG"}, {"GUF", "GF"},
        {"GUM", "GU"}, {"HNG", "HU"}, {"HOL", "NL"}, {"HRV", "HR"}, {"HWA", "US"}, {"ICO", "CC"}, {"IND", "IN"},
        {"INS", "ID"}, {"IOM", "IM"}, {"IRL", "IE"}, {"IRN", "IR"}, {"ISL", "IS"}, {"ITA", "IT"}, {"JOR", "JO"},
        {"JPN", "JP"}, {"JSY", "JE"}, {"KAL", "RU"}, {"KAZ", "KZ"}, {"KEN", "KE"}, {"KGZ", "KG"}, {"KIR", "KI"},
        {"KOR", "KR"}, {"LAO", "LA"}, {"LBY", "LY"}, {"LCA", "LC"}, {"LTU", "LT"}, {"MAU", "MU"}, {"MDG", "MG"},
        {"MDW", "UM"}, {"MEL", "ES"}, {"MHL", "MH"}, {"MKD", "MK"}, {"MLA", "MY"}, {"MLT", "MT"}, {"MNE", "ME"},
        {"MNG", "MN"}, {"MOZ", "MZ"}, {"MRA", "MP"}, {"MRC", "MA"}, {"MTN", "MR"}, {"MWI", "MW"}, {"NCG", "NI"},
        {"NCL", "NC"}, {"NFK", "NF"}, {"NGR", "NE"}, {"NIR", "GB"}, {"NOR", "NO"}, {"NRU", "NR"}, {"NZL", "NZ"},
        {"OCE", "PF"}, {"OGA", "JP"}, {"ORK", "GB"}, {"PAK", "PK"}, {"PAQ", "CL"}, {"PHL", "PH"}, {"PLW", "PW"},
        {"POL", "PL"}, {"POR", "PT"}, {"PRG", "PY"}, {"PRU", "PE"}, {"PTR", "PR"}, {"QAT", "QA"}, {"REU", "RE"},
        {"ROU", "RO"}, {"RSE", "RU"}, {"RUS", "RU"}, {"SAR", "IT"}, {"SCN", "KN"}, {"SCT", "GB"}, {"SCY", "IT"},
        {"SDN", "SD"}, {"SHE", "GB"}, {"SLM", "SB"}, {"SMA", "AS"}, {"SMO", "WS"}, {"SPM", "PM"}, {"SRB", "RS"},
        {"SVB", "SJ"}, {"SVK", "SK"}, {"SVN", "SI"}, {"SWE", "SE"}, {"SWZ", "SZ"}, {"SYR", "SY"}, {"THA", "TH"},
        {"TJK", "TJ"}, {"TKM", "TM"}, {"TMP", "TL"}, {"TON", "TO"}, {"TRD", "TT"}, {"TUN", "TN"}, {"TUR", "TR"},
        {"TUV", "TV"}, {"TWN", "TW"}, {"TZA", "TZ"}, {"UAE", "AE"}, {"UGA", "UG"}, {"UKR", "UA"}, {"URG", "UY"},
        {"USA", "US"}, {"UZB", "UZ"}, {"VCT", "VC"}, {"VEN", "VE"}, {"VTN", "VN"}, {"VUT", "VU"}, {"WAL", "WF"},
        {"WLS", "GB"}, {"ZMB", "ZM"}, {"ZWE", "ZW"},
    };
    return m;
}

constexpr double kDeg = 3.14159265358979323846 / 180.0;

// Great-circle distance (haversine).
double km_between(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
}

}  // namespace

bool NavaidDb::load(const std::string &path)
{
    std::ifstream f(path);
    if (!f) return false;
    std::vector<std::string> hdr, r;
    if (!next_record(f, hdr)) return false;
    auto col = [&](const char *name) { return column(hdr, name); };
    const int c_ident = col("ident"), c_name = col("name"), c_type = col("type"), c_freq = col("frequency_khz"),
              c_lat = col("latitude_deg"), c_lon = col("longitude_deg"), c_cc = col("iso_country"),
              c_pow = col("power");
    if (c_ident < 0 || c_type < 0 || c_freq < 0 || c_lat < 0 || c_lon < 0) return false;
    const int need = std::max({c_ident, c_name, c_type, c_freq, c_lat, c_lon, c_cc, c_pow});

    std::vector<Navaid> v;
    while (next_record(f, r)) {
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

int NavaidDb::merge_rww(const std::string &path)
{
    std::ifstream f(path);
    if (!f) return -1;
    std::vector<std::string> hdr, r;
    if (!next_record(f, hdr)) return -1;
    const int c_khz = column(hdr, "KHz"), c_id = column(hdr, "ID"), c_type = column(hdr, "Type"),
              c_name = column(hdr, "'Name' and Location"), c_itu = column(hdr, "ITU"), c_lat = column(hdr, "Lat"),
              c_lon = column(hdr, "Lon"), c_act = column(hdr, "Active");
    if (c_khz < 0 || c_id < 0 || c_lat < 0 || c_lon < 0) return -1;
    const int need = std::max({c_khz, c_id, c_type, c_name, c_itu, c_lat, c_lon, c_act});

    std::vector<Navaid> add;
    while (next_record(f, r)) {
        if (int(r.size()) <= need) continue;
        if (c_type >= 0 && r[c_type] != "NDB") continue;
        if (c_act >= 0 && r[c_act] != "Y") continue;
        // Without a position it could be named but not located, and the
        // radius could not limit it; leave those out.
        if (r[c_id].empty() || r[c_lat].empty() || r[c_lon].empty()) continue;
        Navaid n;
        n.ident = r[c_id];
        n.freq_hz = atof(r[c_khz].c_str()) * 1000.0;
        n.lat = atof(r[c_lat].c_str());
        n.lon = atof(r[c_lon].c_str());
        if (n.freq_hz <= 0) continue;
        n.name = c_name >= 0 ? r[c_name] : "";
        if (c_itu >= 0) {
            auto it = itu_to_iso().find(r[c_itu]);
            if (it != itu_to_iso().end()) n.country = it->second;
        }
        // OurAirports' entry stands where it has the beacon: the same ident
        // within a kHz and 100 km. Not the ident and frequency alone — OL in
        // Salta, Argentina, on 340 kHz would hide Shannon's OL on 339.
        auto lo = std::lower_bound(navs_.begin(), navs_.end(), n.freq_hz - 1000.0,
                                   [](const Navaid &a, double fr) { return a.freq_hz < fr; });
        bool dup = false;
        for (auto it = lo; it != navs_.end() && it->freq_hz <= n.freq_hz + 1000.0 && !dup; ++it)
            dup = it->ident == n.ident && km_between(it->lat, it->lon, n.lat, n.lon) < 100.0;
        if (!dup) add.push_back(std::move(n));
    }
    const int added = int(add.size());
    for (auto &n : add) navs_.push_back(std::move(n));
    std::sort(navs_.begin(), navs_.end(), [](const Navaid &a, const Navaid &b) { return a.freq_hz < b.freq_hz; });
    return added;
}

NavaidHit NavaidDb::locate(const Navaid &n) const
{
    NavaidHit h;
    h.nav = &n;
    if (!have_rx_) return h;
    // Distance and initial great-circle bearing.
    const double p1 = rx_lat_ * kDeg, p2 = n.lat * kDeg, dl = (n.lon - rx_lon_) * kDeg;
    h.dist_km = km_between(rx_lat_, rx_lon_, n.lat, n.lon);
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
    for (auto it = lo; it != navs_.end() && it->freq_hz <= freq_hz + tol_hz; ++it) {
        auto h = locate(*it);
        if (max_km_ > 0 && h.dist_km > max_km_) continue;   // dist_km is -1 without a receiver
        out.push_back(h);
    }
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
