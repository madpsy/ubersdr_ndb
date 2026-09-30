// mqtt.cpp — see mqtt.h.

#include "mqtt.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ndb {

namespace {

constexpr const char *kUserAgent = "ubersdr_ndb";
constexpr const char *kModel = "NDB beacon decoder";   // on the Home Assistant device card
constexpr const char *kIngestPort = "6926";             // UberSDR's mqtt.addon_ingest.port default

// Well inside the receiver's offline_after_sec (300 s by default), past which
// it marks the addon offline and every entity goes unavailable.
constexpr double kSummaryEveryS = 30.0;
constexpr double kSummaryMinGapS = 5.0;    // closest two summaries come, when events prompt them
constexpr double kProbeEveryS = 30.0;
constexpr double kHoldS = 15.0;            // after a 429 or 503
constexpr long kTimeoutS = 5;

double mono_now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

size_t curl_write(char *p, size_t sz, size_t n, void *ud)
{
    // The replies read here are a few hundred bytes; past that is not worth holding.
    auto *out = static_cast<std::string *>(ud);
    if (out->size() < 16384) out->append(p, std::min(sz * n, 16384 - out->size()));
    return sz * n;
}

// Aborts a transfer in progress on shutdown, so stop() does not wait out a
// timeout on a receiver that has gone quiet.
int curl_progress(void *ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const std::atomic<bool> *>(ud)->load() ? 0 : 1;
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    return s;
}

// The raw scalar after "key": in a flat JSON object — a string without its
// quotes, or a number or literal as written. Enough for /health.
std::string field(const std::string &body, const char *key)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return "";
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return "";
    while (++p < body.size() && body[p] == ' ') {}
    if (p >= body.size()) return "";
    if (body[p] == '"') {
        size_t e = body.find('"', p + 1);
        return e == std::string::npos ? "" : body.substr(p + 1, e - p - 1);
    }
    size_t e = body.find_first_of(",}\n", p);
    return trim(body.substr(p, e == std::string::npos ? std::string::npos : e - p));
}

int int_field(const std::string &body, const char *key, int dflt)
{
    std::string v = field(body, key);
    return v.empty() ? dflt : atoi(v.c_str());
}

// For this file's own strings (entity declarations), which are ASCII.
std::string q(const std::string &s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// ---------------------------------------------------------------------------
// Home Assistant
// ---------------------------------------------------------------------------

// All read the one retained summary, told apart by entity_key, so one publish
// updates every entity and Home Assistant has values the moment it subscribes.
// A template with nothing to show renders '' rather than a zero, which Home
// Assistant keeps as "unknown". In declaration order: when the receiver allows
// fewer than there are, the first are kept.
struct Entity {
    const char *key, *component, *name, *value, *attrs, *unit, *device_class, *state_class, *icon;
    bool diagnostic;
};

const Entity kEntities[] = {
    {"receiving", "binary_sensor", "Receiving", "{{ 'ON' if value_json.receiving else 'OFF' }}",
     "{{ {'streams': value_json.streams} | tojson }}", nullptr, "connectivity", nullptr, nullptr, false},
    {"heard_now", "sensor", "Beacons Heard Now", "{{ value_json.counts.now }}",
     "{{ {'beacons': value_json.heard_now} | tojson }}", nullptr, nullptr, "measurement", "mdi:radio-tower", false},
    {"heard_hour", "sensor", "Beacons Heard (1 h)", "{{ value_json.counts.hour }}", nullptr, nullptr, nullptr,
     "measurement", "mdi:radio-tower", false},
    {"heard_day", "sensor", "Beacons Heard (24 h)", "{{ value_json.counts.day }}", nullptr, nullptr, nullptr,
     "measurement", "mdi:radio-tower", false},
    {"new_day", "sensor", "New Beacons (24 h)", "{{ value_json.counts.new_day }}", nullptr, nullptr, nullptr,
     "measurement", "mdi:new-box", false},
    {"logged", "sensor", "Beacons Logged", "{{ value_json.counts.logged }}", nullptr, nullptr, nullptr,
     "measurement", "mdi:format-list-bulleted", false},
    {"strongest", "sensor", "Strongest Beacon",
     "{{ value_json.strongest.ident if value_json.strongest else '' }}",
     "{{ value_json.strongest | tojson if value_json.strongest else '{}' }}", nullptr, nullptr, nullptr,
     "mdi:signal", false},
    {"strongest_snr", "sensor", "Strongest SNR",
     "{{ value_json.strongest.snr_db if value_json.strongest else '' }}", nullptr, "dB", nullptr, "measurement",
     "mdi:signal", false},
    {"farthest_now", "sensor", "Farthest Beacon Now",
     "{{ value_json.farthest_now.dist_km if value_json.farthest_now else '' }}",
     "{{ value_json.farthest_now | tojson if value_json.farthest_now else '{}' }}", "km", "distance",
     "measurement", "mdi:map-marker-distance", false},
    {"farthest_day", "sensor", "Farthest Beacon (24 h)",
     "{{ value_json.farthest_day.dist_km if value_json.farthest_day else '' }}",
     "{{ value_json.farthest_day | tojson if value_json.farthest_day else '{}' }}", "km", "distance",
     "measurement", "mdi:map-marker-distance", false},
    {"last_identified", "sensor", "Last Identified",
     "{{ value_json.last_identified.ident if value_json.last_identified else '' }}",
     "{{ value_json.last_identified | tojson if value_json.last_identified else '{}' }}", nullptr, nullptr,
     nullptr, "mdi:tag-outline", false},
    {"last_new", "sensor", "Last New Beacon",
     "{{ value_json.last_new.ident if value_json.last_new else '' }}",
     "{{ value_json.last_new | tojson if value_json.last_new else '{}' }}", nullptr, nullptr, nullptr,
     "mdi:new-box", false},
    {"decoding", "sensor", "Carriers Decoding", "{{ value_json.carriers.decoding }}", nullptr, nullptr, nullptr,
     "measurement", "mdi:waveform", false},
    {"tracking", "sensor", "Beacons Tracked", "{{ value_json.carriers.tracking }}", nullptr, nullptr, nullptr,
     "measurement", "mdi:radar", false},
    {"streams_connected", "sensor", "Streams Connected", "{{ value_json.streams_connected }}", nullptr, nullptr,
     nullptr, "measurement", "mdi:lan-connect", true},
    {"navaids", "sensor", "Beacon Database", "{{ value_json.navaids_loaded }}", nullptr, nullptr, nullptr,
     nullptr, "mdi:database", true},
    {"started", "sensor", "Started", "{{ value_json.started_utc }}", nullptr, nullptr, "timestamp", nullptr,
     nullptr, true},
};

std::string entity_json(const Entity &e, const std::string &version)
{
    std::string j = "{\"sub_topic\":\"summary\",\"entity_key\":" + q(e.key) + ",\"component\":" + q(e.component) +
                    ",\"name\":" + q(e.name) + ",\"value_template\":" + q(e.value);
    if (e.attrs) j += ",\"json_attributes_template\":" + q(e.attrs);
    if (e.unit) j += ",\"unit_of_measurement\":" + q(e.unit);
    if (e.device_class) j += ",\"device_class\":" + q(e.device_class);
    if (e.state_class) j += ",\"state_class\":" + q(e.state_class);
    if (e.icon) j += ",\"icon\":" + q(e.icon);
    if (e.diagnostic) j += ",\"entity_category\":\"diagnostic\"";
    if (!version.empty()) j += ",\"addon_version\":" + q(version);
    return j + ",\"addon_model\":" + q(kModel) + "}";
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::string MqttPublisher::ingest_url_for(const std::string &ubersdr_url)
{
    if (const char *v = getenv("UBERSDR_INGEST_URL"); v && *v) {
        std::string s = v;
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }
    // Always plain http: the port is only on the sdr-network, whatever the
    // receiver's public URL is.
    std::string s = ubersdr_url;
    if (size_t p = s.find("://"); p != std::string::npos) s = s.substr(p + 3);
    s = s.substr(0, s.find_first_of("/?#"));
    if (size_t p = s.rfind('@'); p != std::string::npos) s = s.substr(p + 1);
    if (!s.empty() && s[0] == '[') s = s.substr(0, s.find(']') + 1);   // [v6]:port
    else s = s.substr(0, s.find(':'));
    if (s.empty()) s = "ubersdr";
    return "http://" + s + ":" + kIngestPort;
}

MqttPublisher::MqttPublisher(MqttConfig cfg, SummaryProvider summary)
    : cfg_(std::move(cfg)), summary_(std::move(summary))
{
}

MqttPublisher::~MqttPublisher() { stop(); }

void MqttPublisher::start()
{
    if (!cfg_.enabled || running_.exchange(true)) return;
    fprintf(stderr, "mqtt: ingest endpoint %s\n", cfg_.ingest_url.c_str());
    thread_ = std::thread([this] { run(); });
}

void MqttPublisher::stop()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.exchange(false)) return;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MqttPublisher::event(std::string json)
{
    if (!cfg_.enabled) return;
    std::lock_guard<std::mutex> lk(mu_);
    queue_.emplace_back(++queued_, std::move(json));
    while (queue_.size() > kQueueMax) {
        queue_.pop_front();
        ++lost_;
    }
}

void MqttPublisher::run()
{
    while (running_) {
        const double now = mono_now();
        if (!available_ && now >= next_probe_) {
            next_probe_ = now + kProbeEveryS;
            if (probe()) {
                event_tokens_ = std::max(4.0, rate_limit_ / 4.0);
                event_refill_at_ = now;
                next_summary_ = now;
            }
        }
        // Each step only while the receiver is taking publishes; what did not
        // go out goes next time.
        auto open = [this] { return available_ && mono_now() >= hold_until_; };
        if (open()) publish_events(now);
        if (open() && now >= next_summary_) publish_summary(now);

        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::seconds(1), [this] { return !running_.load(); });
    }
}

// ---------------------------------------------------------------------------
// The ingest port
// ---------------------------------------------------------------------------

long MqttPublisher::request(const char *method, const std::string &path, const std::string *body,
                            std::string &resp, std::string &err)
{
    CURL *c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return -1; }
    curl_slist *h = curl_slist_append(nullptr, "Content-Type: application/json");
    const std::string url = cfg_.ingest_url + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(body->size()));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, kTimeoutS);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kTimeoutS);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &running_);
    long code = -1;
    const CURLcode res = curl_easy_perform(c);
    if (res == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else err = curl_easy_strerror(res);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

void MqttPublisher::unavailable(const std::string &why)
{
    available_ = false;
    next_probe_ = mono_now() + kProbeEveryS;
    if (!running_) return;   // a transfer cut short by stop() is not the receiver's doing
    if (!warned_) {
        warned_ = true;
        fprintf(stderr, "mqtt: %s — continuing without MQTT, and asking again every %.0f s\n", why.c_str(),
                kProbeEveryS);
    }
}

bool MqttPublisher::probe()
{
    std::string resp, err;
    const long code = request("GET", "/health", nullptr, resp, err);
    if (code < 0) { unavailable("ingest port unreachable (" + err + ")"); return false; }
    if (code == 403) {
        unavailable("the receiver does not recognise this container as an installed addon");
        return false;
    }
    if (code != 200) { unavailable("ingest health answered HTTP " + std::to_string(code)); return false; }

    addon_ = field(resp, "addon");
    ha_discovery_ = field(resp, "ha_discovery") == "true";
    rate_limit_ = std::max(1, int_field(resp, "rate_limit", 120));
    max_entities_ = std::max(0, int_field(resp, "max_entities", 20));
    available_ = true;
    warned_ = false;
    fprintf(stderr, "mqtt: publishing as addon \"%s\" (broker %s, Home Assistant discovery %s, %d publishes/min)\n",
            addon_.c_str(), field(resp, "mqtt_connected") == "true" ? "connected" : "not connected",
            ha_discovery_ ? "on" : "off", rate_limit_);
    const int offline_after = int_field(resp, "offline_after_sec", 300);
    if (offline_after > 0 && offline_after < 2 * kSummaryEveryS)
        fprintf(stderr, "mqtt: warning: the receiver marks an addon offline after %d s without a publish, "
                        "and this publishes every %.0f s: entities will flap\n", offline_after, kSummaryEveryS);
    if (ha_discovery_) declare_entities();
    return true;
}

// Idempotent on the receiver's side, so this runs on every (re)connection,
// which also covers discovery being turned on after this started.
void MqttPublisher::declare_entities()
{
    const size_t all = sizeof kEntities / sizeof kEntities[0];
    const size_t n = std::min(all, size_t(max_entities_));
    size_t declared = 0;
    for (size_t i = 0; i < n && running_; ++i) {
        const std::string body = entity_json(kEntities[i], cfg_.version);
        std::string resp, err;
        const long code = request("POST", "/discovery", &body, resp, err);
        if (code < 0) { unavailable(std::string("declaring ") + kEntities[i].key + ": " + err); return; }
        if (code == 503) {
            fprintf(stderr, "mqtt: Home Assistant discovery is off on the receiver; publishing data only\n");
            return;
        }
        if (code >= 400) {
            fprintf(stderr, "mqtt: declaring %s refused (HTTP %ld): %s\n", kEntities[i].key, code,
                    trim(resp).c_str());
            continue;
        }
        ++declared;
    }
    fprintf(stderr, "mqtt: declared %zu of %zu Home Assistant entities%s\n", declared, all,
            n < all ? " (the receiver's limit)" : "");
}

MqttPublisher::Result MqttPublisher::publish(const std::string &sub_topic, const std::string &body, bool retain)
{
    std::string resp, err;
    const long code = request("POST", "/publish/" + sub_topic + (retain ? "?retain=true" : ""), &body, resp, err);
    if (code < 0) {
        // The receiver may be restarting: dormant until the next probe, which
        // re-declares.
        unavailable("publishing " + sub_topic + " failed (" + err + ")");
        return Result::Unreachable;
    }
    if (code < 300) return Result::Ok;
    if (code == 503 || code == 429) {
        // The receiver's broker down (transient, and in its own log), or over
        // the rate limit: held rather than retried every second.
        hold_until_ = mono_now() + kHoldS;
        if (code == 429) fprintf(stderr, "mqtt: rate limited publishing %s; holding for %.0f s\n", sub_topic.c_str(), kHoldS);
        return Result::Held;
    }
    if (code == 403) {
        unavailable("the receiver no longer recognises this container as an addon");
        return Result::Unreachable;
    }
    fprintf(stderr, "mqtt: publishing %s refused (HTTP %ld): %s\n", sub_topic.c_str(), code, trim(resp).c_str());
    return Result::Refused;
}

// ---------------------------------------------------------------------------
// What is published
// ---------------------------------------------------------------------------

// Every event, once, in order: one that does not fit the bucket waits for it,
// and one the receiver could not pass on is sent again after the hold.
void MqttPublisher::publish_events(double now)
{
    const double cap = std::max(4.0, rate_limit_ / 4.0);
    event_tokens_ = std::min(cap, event_tokens_ + (now - event_refill_at_) * cap / 60.0);
    event_refill_at_ = now;

    bool sent = false;
    while (event_tokens_ >= 1.0 && running_) {
        std::pair<uint64_t, std::string> ev;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.empty()) break;
            ev = queue_.front();
        }
        event_tokens_ -= 1.0;
        const Result r = publish("events", ev.second, false);
        if (r != Result::Ok && r != Result::Refused) break;   // try it again later
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Still the front unless event() overflowed the queue meanwhile.
            if (!queue_.empty() && queue_.front().first == ev.first) queue_.pop_front();
        }
        ++events_sent_;
        sent = true;
    }
    // Let the retained state catch up, soon but not once per event of a burst.
    if (sent) next_summary_ = std::min(next_summary_, std::max(now, last_summary_ + kSummaryMinGapS));
}

void MqttPublisher::publish_summary(double now)
{
    std::string j = summary_();
    size_t pending;
    uint64_t lost;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending = queue_.size();
        lost = lost_;
    }
    // The provider returns an object: this publisher's counters go in as its
    // last member.
    const size_t close = j.rfind('}');
    if (close == std::string::npos || close == 0) return;
    j.insert(close, std::string(j[close - 1] == '{' ? "" : ",") + "\"mqtt\":{\"events_sent\":" +
                        std::to_string(events_sent_) + ",\"events_pending\":" + std::to_string(pending) +
                        ",\"events_lost\":" + std::to_string(lost) + "}");
    const Result r = publish("summary", j, true);
    if (r == Result::Ok || r == Result::Refused) {
        last_summary_ = now;
        next_summary_ = now + kSummaryEveryS;
    }
}

}  // namespace ndb
