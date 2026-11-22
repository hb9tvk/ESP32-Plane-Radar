#pragma once

// Route and airline from api.adsbdb.com, cached per callsign. (The detailed
// aircraft type comes from the adsb.fi feed's "desc" field.) Lookups run from the
// ADS-B task after a successful adsb.fi poll; the renderer only reads the
// cache, so a missing or failed lookup simply leaves the label on its
// callsign / registration fallback.

#include <cstddef>

#include "services/adsb_client.h"

namespace services::flight_info {

struct Info {
  char route[12];    // "ZRH-ORD"; empty when unknown
  char airline[17];  // "British Airways"; empty when unknown
};

/** Create the cache lock. Call once before lookup()/fetchPending(). */
void init();

/** Copy whatever is cached for this aircraft (fields empty when unknown). */
void lookup(const services::adsb::Aircraft& plane, Info* out);

/**
 * Fetch missing cache entries for the aircraft nearest to center, within a
 * bounded request count and time budget. Blocking; call from the ADS-B task
 * only, never concurrently with another TLS session. Returns requests sent.
 */
size_t fetchPending(double center_lat, double center_lon);

}  // namespace services::flight_info
