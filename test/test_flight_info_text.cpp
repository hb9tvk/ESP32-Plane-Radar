#include "services/flight_info_text.h"

#include <cassert>
#include <cstring>

using namespace services::flight_info::text;

static bool eq(const char* a, const char* b) { return strcmp(a, b) == 0; }

int main() {
  char out[32];

  foldToAscii("Aerom\xC3\xA9xico", out, sizeof(out));
  assert(eq(out, "Aeromexico"));
  foldToAscii("  Z\xC3\xBCrich   Airport ", out, sizeof(out));
  assert(eq(out, "Zurich Airport"));
  foldToAscii("LOT Polish Airlines \xC5\x81\xC3\xB3""d\xC5\xBA", out, sizeof(out));
  assert(eq(out, "LOT Polish Airlines Lodz"));
  foldToAscii("Stra\xC3\x9F" "e \xE2\x80\x93 X \xF0\x9F\x98\x80", out, sizeof(out));
  assert(eq(out, "Strasse - X"));
  foldToAscii("abcdefghij", out, 5);
  assert(eq(out, "abcd"));

  truncateWords("British Airways", 16, out, sizeof(out));
  assert(eq(out, "British Airways"));
  truncateWords("KLM Royal Dutch Airlines", 16, out, sizeof(out));
  assert(eq(out, "KLM Royal Dutch"));
  truncateWords("Swiss International Air Lines", 16, out, sizeof(out));
  assert(eq(out, "Swiss"));
  truncateWords("Supercalifragilisticexpialidocious", 16, out, sizeof(out));
  assert(eq(out, "Supercalifragili"));
  truncateWords("American Airlines", 16, out, sizeof(out));
  assert(eq(out, "American"));

  formatRoute("ZRH", "ORD", out, sizeof(out));
  assert(eq(out, "ZRH-ORD"));
  formatRoute("ZRH", "", out, sizeof(out));
  assert(eq(out, ""));
  formatRoute("LSZH", "KORD", out, sizeof(out));
  assert(eq(out, "LSZH-KORD"));

  // Out-and-back route ZRH -> DUB -> ZRH, aircraft near Zurich.
  const float zrh_lat = 47.46f, zrh_lon = 8.55f, dub_lat = 53.42f, dub_lon = -6.27f;
  const bool outbound = headingTowards(zrh_lat + 0.1f, zrh_lon, 300.0f, dub_lat, dub_lon);
  const bool inbound = headingTowards(zrh_lat + 0.1f, zrh_lon, 120.0f, dub_lat, dub_lon);
  assert(outbound && !inbound);
  formatLegRoute("ZRH", "DUB", "ZRH", outbound, out, sizeof(out));
  assert(eq(out, "ZRH-DUB"));
  formatLegRoute("ZRH", "DUB", "ZRH", inbound, out, sizeof(out));
  assert(eq(out, "DUB-ZRH"));
  formatLegRoute("MEL", "SIN", "LHR", false, out, sizeof(out));
  assert(eq(out, "SIN-LHR"));
  formatLegRoute("ZRH", "", "DUB", false, out, sizeof(out));
  assert(eq(out, "ZRH-DUB"));
  formatLegRoute("ZRH", "", "ZRH", false, out, sizeof(out));
  assert(eq(out, ""));

  formatType("Airbus", "A321-271NX", 18, out, sizeof(out));
  assert(eq(out, "Airbus A321-271NX"));
  formatType("Airbus Sas", "A320-214", 18, out, sizeof(out));
  assert(eq(out, "Airbus A320-214"));
  formatType("Boeing", "737NG 8AS/W", 18, out, sizeof(out));
  assert(eq(out, "Boeing 737NG 8AS/W"));
  formatType("Bombardier", "C Series 300", 18, out, sizeof(out));
  assert(eq(out, "C Series 300"));
  formatType("Boeing", "Stearman E75N1 PT-13D Kaydet", 18, out, sizeof(out));
  assert(eq(out, "Stearman E75N1"));
  formatType("Pilatus", "Pilatus PC-12/47E", 18, out, sizeof(out));
  assert(eq(out, "Pilatus PC-12/47E"));
  truncateWords("PA-28-140/150/160/180", 18, out, sizeof(out));
  assert(eq(out, "PA-28-140/150/160"));

  formatDescription("BOEING 737 MAX 8", 18, out, sizeof(out));
  assert(eq(out, "Boeing 737 MAX 8"));
  formatDescription("AIRBUS A-321neo", 18, out, sizeof(out));
  assert(eq(out, "Airbus A321neo"));
  formatDescription("AIRBUS A-350-1000", 18, out, sizeof(out));
  assert(eq(out, "Airbus A350-1000"));
  formatDescription("BOEING 787-9 Dreamliner", 18, out, sizeof(out));
  assert(eq(out, "Boeing 787-9"));
  formatDescription("AIRBUS HELICOPTERS EC-135/635", 18, out, sizeof(out));
  assert(eq(out, "EC-135/635"));
  formatDescription("PIPER PA-28-140/150/160/180", 18, out, sizeof(out));
  assert(eq(out, "Piper PA-28-140"));
  formatDescription("BRITTEN-NORMAN BN-2T Turbine Islander", 18, out, sizeof(out));
  assert(eq(out, "BN-2T Turbine"));
  formatDescription("ATR ATR-72-600", 18, out, sizeof(out));
  assert(eq(out, "ATR-72-600"));
  formatDescription("DE HAVILLAND CANADA DHC-8-400 Dash 8", 18, out, sizeof(out));
  assert(eq(out, "DHC-8-400 Dash 8"));
  formatDescription("CESSNA 152", 18, out, sizeof(out));
  assert(eq(out, "Cessna 152"));
  formatDescription("", 18, out, sizeof(out));
  assert(eq(out, ""));

  formatType("Airbus", "", 18, out, sizeof(out));
  assert(eq(out, ""));
  return 0;
}
