// mqtt.h — publishing to MQTT through UberSDR's addon ingest port.
//
// UberSDR runs an HTTP listener on the sdr-network (port 6926 by default) that
// lets an addon publish through the receiver's own MQTT connection, and declare
// Home Assistant entities, with no broker address or credential: it knows who
// is calling from the TCP source address and places every topic under that
// addon's own namespace. See addon_mqtt.md in the ka9q_ubersdr repository;
// this follows Mqtt.cpp in ubersdr-ntp.
//
// Nothing to configure. With no ingest port to reach (MQTT off on the
// receiver), or one that does not recognise this container as an installed
// addon, it says so once, stays dormant, and asks again every 30 s. No failure
// here is fatal, and it runs on a thread of its own.
//
// Two topics, under ubersdr/metrics/addons/<name>/ by default:
//
//   summary   retained, every 30 s and soon after any event: what main.cpp's
//             summary provider returns, plus an "mqtt" object of this
//             publisher's own counters. Every Home Assistant entity reads it.
//   events    not retained: each event handed to event(), once, in order.
//
// What is published is identified beacons only — the heard log, not the live
// Morse copy. Decoded text never goes to MQTT.
//
// Budget: the receiver allows rate_limit publishes a minute (default 120, read
// from /health). The summary is at most 2 a minute plus one per burst of
// events, 5 s apart at the closest; events draw on a bucket a quarter of the
// limit deep and wait for it rather than being dropped.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ndb {

struct MqttConfig {
    bool        enabled = true;
    std::string ingest_url = "http://ubersdr:6926";
    std::string version;        // shown on the Home Assistant device card
};

class MqttPublisher {
public:
    // Returns the summary as a JSON object. Called on the publishing thread.
    using SummaryProvider = std::function<std::string()>;

    MqttPublisher(MqttConfig cfg, SummaryProvider summary);
    ~MqttPublisher();
    MqttPublisher(const MqttPublisher &) = delete;
    MqttPublisher &operator=(const MqttPublisher &) = delete;

    void start();
    void stop();

    // Queue one event (a JSON object) for the events topic. Any thread. Kept
    // while the receiver is away, up to kQueueMax; past that the oldest go,
    // and are counted in the summary.
    void event(std::string json);

    // Where the ingest port is: UBERSDR_INGEST_URL when set, else port 6926
    // on the host of the UberSDR URL this addon already talks to.
    static std::string ingest_url_for(const std::string &ubersdr_url);

private:
    enum class Result { Ok, Refused, Unreachable, Held };
    static constexpr size_t kQueueMax = 256;

    void run();
    bool probe();
    void declare_entities();
    Result publish(const std::string &sub_topic, const std::string &body, bool retain);
    long request(const char *method, const std::string &path, const std::string *body, std::string &resp,
                 std::string &err);
    void unavailable(const std::string &why);
    void publish_events(double now);
    void publish_summary(double now);

    MqttConfig cfg_;
    SummaryProvider summary_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mu_;                 // guards queue_, lost_ and the wait below
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, std::string>> queue_;   // numbered, so a send can tell it is still the front
    uint64_t queued_ = 0;
    uint64_t lost_ = 0;

    // The publishing thread's alone.
    bool available_ = false;
    bool warned_ = false;
    bool ha_discovery_ = false;
    int rate_limit_ = 120;
    int max_entities_ = 20;
    std::string addon_;
    double next_probe_ = 0.0;
    double next_summary_ = 0.0;
    double last_summary_ = -1e9;
    double hold_until_ = 0.0;       // after a 429 or 503, nothing until then
    double event_tokens_ = 0.0;
    double event_refill_at_ = 0.0;
    uint64_t events_sent_ = 0;
};

}  // namespace ndb
