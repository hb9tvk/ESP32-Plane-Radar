#include "services/flight_info.h"

#include <WiFi.h>
#include <ESP_SSLClient.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cctype>
#include <cmath>
#include <cstring>

#include "config.h"
#include "firmware_version.h"
#include "services/flight_info_text.h"
#include "services/http_body_framing.h"
#include "services/http_response_head.h"

namespace services::flight_info {

namespace {

constexpr char kApiHost[] = "api.adsbdb.com";
// Same TLS sizing as the adsb.fi client: the server may send full-size
// records, and only one TLS session is ever open at a time.
constexpr int kBearSslReceiveBufferBytes = 16709;
constexpr int kBearSslTransmitBufferBytes = 1024;
constexpr int kConnectTimeoutMs = 5000;
constexpr unsigned long kResponseTimeoutMs = 5000;

constexpr size_t kRouteCacheSize = 64;

constexpr uint32_t kRouteFoundTtlMs = 2UL * 60UL * 60UL * 1000UL;      // 2 h
constexpr uint32_t kRouteNotFoundTtlMs = 30UL * 60UL * 1000UL;         // 30 min
constexpr uint32_t kErrorTtlMs = 60UL * 1000UL;
constexpr uint32_t kRateLimitBackoffMs = 60UL * 1000UL;

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.01745329252f;

enum class State : uint8_t { kEmpty, kFound, kNotFound, kError };

/** Parsed adsbdb flightroute. The midpoint is set only for routes with a stop. */
struct RouteData {
  char origin[5];
  char midpoint[5];
  char destination[5];
  float midpoint_lat;
  float midpoint_lon;
  char airline[sizeof(Info::airline)];
};

struct RouteEntry {
  char callsign[9];
  RouteData data;
  State state;
  uint32_t fetched_ms;
  uint32_t used_ms;
};

RouteEntry s_routes[kRouteCacheSize] = {};
SemaphoreHandle_t s_mutex = nullptr;
uint32_t s_backoff_until_ms = 0;
bool s_backoff_active = false;

struct Lock {
  Lock() { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { if (s_mutex) xSemaphoreGive(s_mutex); }
};

RouteEntry* findRouteLocked(const char* callsign) {
  for (RouteEntry& e : s_routes) {
    if (e.state != State::kEmpty && strcmp(e.callsign, callsign) == 0) return &e;
  }
  return nullptr;
}

/** Empty slot, else the least recently used one. */
template <class Entry, size_t N>
Entry* allocLocked(Entry (&cache)[N]) {
  Entry* victim = &cache[0];
  for (Entry& e : cache) {
    if (e.state == State::kEmpty) return &e;
    if (static_cast<int32_t>(victim->used_ms - e.used_ms) > 0) victim = &e;
  }
  return victim;
}

bool due(State state, uint32_t fetched_ms, uint32_t found_ttl,
         uint32_t not_found_ttl) {
  const uint32_t ttl = state == State::kFound      ? found_ttl
                       : state == State::kNotFound ? not_found_ttl
                                                   : kErrorTtlMs;
  return static_cast<uint32_t>(millis() - fetched_ms) >= ttl;
}

/** Callsign adsbdb can resolve: a real flight ID, not the hex/registration. */
bool routeCallsign(const services::adsb::Aircraft& p) {
  const size_t n = strlen(p.callsign);
  if (n < 3 || n > 8 || strcmp(p.callsign, p.hex) == 0) return false;
  for (size_t i = 0; i < n; ++i) {
    if (!isalnum(static_cast<unsigned char>(p.callsign[i]))) return false;
  }
  // General aviation transmits its registration ("HBXYZ" for HB-XYZ).
  char reg[sizeof(p.registration)] = {};
  size_t r = 0;
  for (const char* s = p.registration; *s && r + 1 < sizeof(reg); ++s) {
    if (*s != '-') reg[r++] = *s;
  }
  if (r > 0 && strcasecmp(reg, p.callsign) == 0) return false;
  // Airline flight numbers are a 3-letter ICAO prefix followed by a digit.
  return isalpha(static_cast<unsigned char>(p.callsign[0])) &&
         isalpha(static_cast<unsigned char>(p.callsign[1])) &&
         isalpha(static_cast<unsigned char>(p.callsign[2])) &&
         (n == 3 || isdigit(static_cast<unsigned char>(p.callsign[3])));
}

bool routeNeeded(const services::adsb::Aircraft& p) {
  if (!routeCallsign(p)) return false;
  Lock lock;
  const RouteEntry* e = findRouteLocked(p.callsign);
  return !e || due(e->state, e->fetched_ms, kRouteFoundTtlMs, kRouteNotFoundTtlMs);
}

void storeRoute(const char* callsign, State state, const RouteData* data) {
  Lock lock;
  RouteEntry* e = findRouteLocked(callsign);
  if (!e) {
    e = allocLocked(s_routes);
    snprintf(e->callsign, sizeof(e->callsign), "%s", callsign);
  }
  e->state = state;
  e->fetched_ms = e->used_ms = millis();
  if (data) e->data = *data;
  else memset(&e->data, 0, sizeof(e->data));
}

/** Socket reader for one keep-alive connection; deadline set per request. */
class SocketSource {
 public:
  explicit SocketSource(Client& stream) : stream_(&stream) {}
  void setDeadline(unsigned long deadline) { deadline_ = deadline; }

  int read() {
    if (pos_ >= len_ && !refill()) return -1;
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = len_ = 0;
    while (static_cast<long>(millis() - deadline_) < 0) {
      const int available = stream_->available();
      if (available > 0) {
        const int want = available > static_cast<int>(sizeof(buffer_))
                             ? static_cast<int>(sizeof(buffer_)) : available;
        const int got = stream_->readBytes(buffer_, want);
        if (got > 0) {
          len_ = static_cast<size_t>(got);
          return true;
        }
      }
      if (!stream_->connected() && stream_->available() <= 0) break;
      delay(1);
    }
    return false;
  }

  Client* stream_;
  unsigned long deadline_ = 0;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
};

using BodyReader = services::http::BodyFramer<SocketSource>;

/** Pick IATA code, falling back to ICAO (both ASCII). */
void airportCode(JsonObjectConst airport, char* out, size_t out_len) {
  out[0] = '\0';
  for (const char* key : {"iata_code", "icao_code"}) {
    const char* code = airport[key].as<const char*>();
    if (code && code[0]) {
      text::foldToAscii(code, out, out_len);
      if (out[0]) return;
    }
  }
}

void parseRoute(const JsonDocument& doc, RouteData* out) {
  memset(out, 0, sizeof(*out));
  JsonObjectConst fr = doc["response"]["flightroute"];
  airportCode(fr["origin"], out->origin, sizeof(out->origin));
  airportCode(fr["destination"], out->destination, sizeof(out->destination));
  JsonObjectConst mid = fr["midpoint"];
  if (!mid.isNull() && mid["latitude"].is<float>() && mid["longitude"].is<float>()) {
    airportCode(mid, out->midpoint, sizeof(out->midpoint));
    out->midpoint_lat = mid["latitude"].as<float>();
    out->midpoint_lon = mid["longitude"].as<float>();
  }
  char name[64];
  text::foldToAscii(fr["airline"]["name"].as<const char*>(), name, sizeof(name));
  text::truncateWords(name, config::kLabelAirlineMaxChars, out->airline,
                      sizeof(out->airline));
}

/** Label route for the leg the aircraft is on now. */
void legRoute(const RouteData& d, const services::adsb::Aircraft& plane,
              char* out, size_t out_len) {
  const bool towards = d.midpoint[0] &&
      text::headingTowards(plane.lat, plane.lon, plane.track_deg,
                           d.midpoint_lat, d.midpoint_lon);
  text::formatLegRoute(d.origin, d.midpoint, d.destination, towards, out, out_len);
}

enum class Outcome : uint8_t { kFound, kNotFound, kRateLimited, kFailed };

/**
 * One GET on the open connection. `reusable` reports whether the connection
 * is still positioned at a message boundary and may carry another request.
 */
Outcome request(ESP_SSLClient& client, SocketSource& source, const char* path,
                RouteData* data, bool* reusable) {
  *reusable = false;
  char req[200];
  const int n = snprintf(req, sizeof(req),
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: keep-alive\r\n"
      "User-Agent: PlaneRadar-%s\r\n\r\n",
      path, kApiHost, firmware::kVersion);
  if (n <= 0 || n >= static_cast<int>(sizeof(req))) return Outcome::kFailed;
  if (client.write(reinterpret_cast<const uint8_t*>(req), n) !=
      static_cast<size_t>(n)) {
    return Outcome::kFailed;
  }

  source.setDeadline(millis() + kResponseTimeoutMs);
  services::http::ResponseHead head;
  if (!services::http::readResponseHead(source, &head)) return Outcome::kFailed;

  const services::http::BodyFraming framing =
      head.chunked ? services::http::BodyFraming::kChunked
                   : services::http::BodyFraming::kIdentity;
  BodyReader body(source, framing, head.content_length);
  const bool framed = head.chunked || head.content_length >= 0;

  Outcome outcome = Outcome::kFailed;
  if (head.status == 200) {
    JsonDocument filter;
    JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
    for (const char* side : {"origin", "midpoint", "destination"}) {
      fr[side]["iata_code"] = true;
      fr[side]["icao_code"] = true;
    }
    fr["midpoint"]["latitude"] = true;
    fr["midpoint"]["longitude"] = true;
    fr["airline"]["name"] = true;
    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (!err) {
      parseRoute(doc, data);
      const bool route = data->origin[0] && data->destination[0];
      outcome = (route || data->airline[0]) ? Outcome::kFound : Outcome::kNotFound;
    }
  } else if (head.status == 404) {
    outcome = Outcome::kNotFound;  // {"response":"unknown callsign"}
  } else if (head.status == 429) {
    outcome = Outcome::kRateLimited;
  }

  if (framed) body.drain();
  *reusable = framed && !head.close && !body.framingError() &&
              !body.truncated() && body.complete() &&
              outcome != Outcome::kRateLimited;
  return outcome;
}

}  // namespace

void init() {
  if (s_mutex == nullptr) s_mutex = xSemaphoreCreateMutex();
}

void lookup(const services::adsb::Aircraft& plane, Info* out) {
  out->route[0] = out->airline[0] = '\0';
  if (!config::kFlightInfoEnabled) return;
  const uint32_t now = millis();
  Lock lock;
  if (plane.callsign[0]) {
    if (RouteEntry* e = findRouteLocked(plane.callsign)) {
      if (e->state == State::kFound) {
        legRoute(e->data, plane, out->route, sizeof(out->route));
        memcpy(out->airline, e->data.airline, sizeof(out->airline));
      }
      e->used_ms = now;
    }
  }
}

size_t fetchPending(double center_lat, double center_lon) {
  if (!config::kFlightInfoEnabled) return 0;
  if (s_backoff_active &&
      static_cast<int32_t>(millis() - s_backoff_until_ms) < 0) {
    return 0;
  }
  s_backoff_active = false;

  // Static: keeps 64 aircraft off the ADS-B task stack.
  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  static float dist[services::adsb::kMaxAircraft];
  const size_t count =
      services::adsb::snapshotAircraft(planes, services::adsb::kMaxAircraft, nullptr);
  const float cos_lat = cosf(static_cast<float>(center_lat) * kDegToRad);
  for (size_t i = 0; i < count; ++i) {
    const float dx = static_cast<float>(planes[i].lon - center_lon) * kKmPerDeg * cos_lat;
    const float dy = static_cast<float>(planes[i].lat - center_lat) * kKmPerDeg;
    dist[i] = dx * dx + dy * dy;
  }

  // Nearest aircraft first: those are the labels most likely to be shown.
  int jobs[config::kFlightInfoMaxRequestsPerCycle];
  size_t job_count = 0;
  bool used[services::adsb::kMaxAircraft] = {};
  while (job_count < config::kFlightInfoMaxRequestsPerCycle) {
    int best = -1;
    for (size_t i = 0; i < count; ++i) {
      if (!used[i] && (best < 0 || dist[i] < dist[best])) best = static_cast<int>(i);
    }
    if (best < 0) break;
    used[best] = true;
    if (routeNeeded(planes[best])) jobs[job_count++] = best;
  }
  if (job_count == 0) return 0;

  const uint32_t started = millis();
  WiFiClient plain;
  ESP_SSLClient client;
  client.setClient(&plain);
  client.setInsecure();  // Same certificate policy as the adsb.fi client.
  client.setBufferSizes(kBearSslReceiveBufferBytes, kBearSslTransmitBufferBytes);
  client.setHandshakeTimeout(5);
  client.setDebugLevel(0);
  if (client.connect(kApiHost, 443, kConnectTimeoutMs) == 0) {
    Serial.println("flight info: TLS connection failed");
    // Mark the first job as an error so a dead API is retried slowly.
    storeRoute(planes[jobs[0]].callsign, State::kError, nullptr);
    client.stop();
    return 0;
  }

  SocketSource source(client);
  size_t sent = 0;
  for (size_t j = 0; j < job_count; ++j) {
    if (j > 0 && static_cast<uint32_t>(millis() - started) >= config::kFlightInfoBudgetMs) {
      break;
    }
    const services::adsb::Aircraft& p = planes[jobs[j]];
    char path[32];
    snprintf(path, sizeof(path), "/v0/callsign/%s", p.callsign);
    RouteData data = {};
    bool reusable = false;
    const Outcome outcome = request(client, source, path, &data, &reusable);
    ++sent;
    if (outcome == Outcome::kRateLimited) {
      s_backoff_until_ms = millis() + kRateLimitBackoffMs;
      s_backoff_active = true;
      Serial.println("flight info: rate limited, pausing lookups for 60 s");
      break;
    }
    // A failure on a reused connection is usually the server closing an idle
    // keep-alive; leave that entry unmarked so the next cycle retries it.
    if (outcome != Outcome::kFailed || j == 0) {
      const State state = outcome == Outcome::kFound      ? State::kFound
                          : outcome == Outcome::kNotFound ? State::kNotFound
                                                          : State::kError;
      storeRoute(p.callsign, state, outcome == Outcome::kFound ? &data : nullptr);
    }
    if (outcome == Outcome::kFound) {
      char route[sizeof(Info::route)];
      legRoute(data, p, route, sizeof(route));
      Serial.printf("flight info: %s %s>%s%s%s (shown %s) %s\r\n", p.callsign,
                    data.origin, data.midpoint, data.midpoint[0] ? ">" : "",
                    data.destination, route[0] ? route : "callsign", data.airline);
    }
    if (!reusable) break;
  }
  client.stop();
  return sent;
}

}  // namespace services::flight_info
