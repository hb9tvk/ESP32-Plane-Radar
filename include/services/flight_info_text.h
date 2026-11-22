#pragma once

// Pure text helpers for adsbdb.com route/airline/type strings. No Arduino
// dependency, so they are host-testable (test/test_flight_info_text.cpp).

#include <cmath>
#include <cstddef>
#include <cstring>

namespace services::flight_info::text {

/** U+0100..U+017F (Latin Extended-A) folded to a single ASCII base letter. */
constexpr char kLatinExtA[] =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
static_assert(sizeof(kLatinExtA) == 129, "Latin Extended-A table needs 128 entries");

/** U+00C0..U+00FF (Latin-1 letters) folded to ASCII (may be two letters). */
inline const char* foldLatin1(unsigned cp) {
  static const char* const kTable[64] = {
      "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
      "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
      "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};
  return (cp >= 0xC0 && cp <= 0xFF) ? kTable[cp - 0xC0] : "";
}

/**
 * Copy UTF-8 `in` to printable ASCII `out`: common Latin accents lose their
 * diacritics, typographic quotes/dashes become ASCII, anything else is
 * dropped. Whitespace runs collapse to one space; ends are trimmed.
 */
inline void foldToAscii(const char* in, char* out, size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  if (!in) return;
  size_t n = 0;
  bool pending_space = false;
  auto put = [&](const char* s) {
    for (; *s; ++s) {
      if (*s == ' ') {
        if (n > 0) pending_space = true;
        continue;
      }
      if (pending_space) {
        if (n + 1 >= out_len) return;
        out[n++] = ' ';
        pending_space = false;
      }
      if (n + 1 >= out_len) return;
      out[n++] = *s;
    }
  };
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in);
  while (*p) {
    const unsigned c = *p;
    unsigned cp = 0;
    int extra = 0;
    if (c < 0x80) {
      const char ch[2] = {(c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ', '\0'};
      put(ch);
      ++p;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F; extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F; extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07; extra = 3;
    } else {
      ++p;  // stray continuation byte
      continue;
    }
    ++p;
    bool valid = true;
    for (int i = 0; i < extra; ++i) {
      if ((*p & 0xC0) != 0x80) { valid = false; break; }
      cp = (cp << 6) | (*p & 0x3F);
      ++p;
    }
    if (!valid) continue;
    if (cp >= 0xC0 && cp <= 0xFF) {
      put(foldLatin1(cp));
    } else if (cp >= 0x100 && cp <= 0x17F) {
      const char ch[2] = {kLatinExtA[cp - 0x100], '\0'};
      put(ch);
    } else if (cp == 0x2018 || cp == 0x2019) {
      put("'");
    } else if (cp == 0x2013 || cp == 0x2014) {
      put("-");
    } else if (cp == 0xA0) {
      put(" ");
    }
  }
  out[n] = '\0';
}

/**
 * Copy `in` limited to `max_chars`. Longer text is cut at the last word or
 * '/' boundary that fits (so "KLM Royal Dutch Airlines" -> "KLM Royal
 * Dutch", "PA-28-140/150/160/180" -> "PA-28-140/150/160"), or hard-cut if
 * the first word alone is too long.
 */
inline void truncateWords(const char* in, size_t max_chars, char* out,
                          size_t out_len) {
  if (out_len == 0) return;
  if (max_chars >= out_len) max_chars = out_len - 1;
  size_t len = strlen(in);
  if (len <= max_chars) {
    memcpy(out, in, len + 1);
    return;
  }
  size_t cut = max_chars;
  if (in[cut] != ' ' && in[cut] != '/') {
    while (cut > 0 && in[cut] != ' ' && in[cut] != '/') --cut;
    if (cut == 0) cut = max_chars;
  }
  while (cut > 0 && (in[cut - 1] == ' ' || in[cut - 1] == '-' ||
                     in[cut - 1] == ',' || in[cut - 1] == '/')) {
    --cut;
  }
  memcpy(out, in, cut);
  out[cut] = '\0';
}

/** "ZRH-ORD"; empty unless both airport codes are known. */
inline void formatRoute(const char* origin, const char* destination, char* out,
                        size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  if (!origin || !destination || !origin[0] || !destination[0]) return;
  const size_t a = strlen(origin), b = strlen(destination);
  if (a + b + 2 > out_len) return;
  memcpy(out, origin, a);
  out[a] = '-';
  memcpy(out + a + 1, destination, b + 1);
}

/**
 * True when an aircraft at lat/lon on track_deg is flying towards target
 * (within 90 degrees of the direct bearing). Flat-earth bearing is plenty
 * for a towards/away decision.
 */
inline bool headingTowards(float lat, float lon, float track_deg,
                           float target_lat, float target_lon) {
  constexpr float kDegToRad = 0.01745329252f;
  const float dx = (target_lon - lon) * std::cos(lat * kDegToRad);
  const float dy = target_lat - lat;
  if (dx == 0.0f && dy == 0.0f) return false;
  const float bearing = std::atan2(dx, dy) / kDegToRad;  // 0 = north, 90 = east
  float diff = std::fmod(std::fabs(track_deg - bearing), 360.0f);
  if (diff > 180.0f) diff = 360.0f - diff;
  return diff < 90.0f;
}

/**
 * Route label for a route that may have a stop. adsbdb lists out-and-back
 * flights under one callsign as origin -> midpoint -> destination (e.g.
 * ZRH -> DUB -> ZRH), so with a midpoint the current leg is shown:
 * origin-midpoint while flying towards the midpoint, else midpoint-
 * destination. Empty when unknown or when both ends are the same airport.
 */
inline void formatLegRoute(const char* origin, const char* midpoint,
                           const char* destination, bool towards_midpoint,
                           char* out, size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  const char* from = origin;
  const char* to = destination;
  if (midpoint && midpoint[0]) {
    if (towards_midpoint) to = midpoint;
    else from = midpoint;
  }
  if (from && to && strcmp(from, to) == 0) return;
  formatRoute(from, to, out, out_len);
}

/** True for corporate suffix words dropped from manufacturer names. */
inline bool isCorporateSuffix(const char* word, size_t len) {
  static const char* const kSuffixes[] = {
      "SAS", "SA", "S.A.", "Inc", "Inc.", "Ltd", "Ltd.", "Corp", "Corp.",
      "Corporation", "Company", "Co", "Co.", "GmbH", "AG", "plc", "LLC"};
  for (const char* s : kSuffixes) {
    if (strlen(s) != len) continue;
    bool same = true;
    for (size_t i = 0; i < len && same; ++i) {
      char a = word[i], b = s[i];
      if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
      if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
      same = a == b;
    }
    if (same) return true;
  }
  return false;
}

/** Remove trailing corporate suffixes in place ("Airbus Sas" -> "Airbus"). */
inline void stripCorporateSuffixes(char* s) {
  for (;;) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == ',')) s[--len] = '\0';
    char* last_space = strrchr(s, ' ');
    if (!last_space) return;
    const char* word = last_space + 1;
    if (!isCorporateSuffix(word, strlen(word))) return;
    *last_space = '\0';
  }
}

/**
 * Detailed type line, e.g. manufacturer "Airbus", type "A321-271NX" ->
 * "Airbus A321-271NX". If that exceeds max_chars, the type alone is used
 * (word-truncated). Inputs must already be ASCII.
 */
inline void formatType(const char* manufacturer, const char* type,
                       size_t max_chars, char* out, size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  if (!type || !type[0]) return;
  char maker[40] = {};
  if (manufacturer) {
    const size_t m = strlen(manufacturer);
    const size_t copy = m < sizeof(maker) - 1 ? m : sizeof(maker) - 1;
    memcpy(maker, manufacturer, copy);
    maker[copy] = '\0';
    stripCorporateSuffixes(maker);
  }
  const size_t ml = strlen(maker), tl = strlen(type);
  // Skip the manufacturer when the type already names it.
  const bool has_maker = ml > 0 && strncmp(type, maker, ml) != 0;
  if (max_chars >= out_len) max_chars = out_len - 1;
  if (has_maker && ml + 1 < max_chars) {
    // Manufacturer + model, the model shortened at a word or '/' boundary,
    // provided the kept part still has a model number ("Boeing 787-9", not
    // "Bombardier C").
    char model[32];
    truncateWords(type, max_chars - ml - 1, model, sizeof(model));
    const size_t kept = strlen(model);
    const bool boundary = kept == tl || type[kept] == ' ' || type[kept] == '/';
    bool digit = false;
    for (const char* p = model; *p; ++p) digit |= (*p >= '0' && *p <= '9');
    if (kept > 0 && boundary && digit) {
      memcpy(out, maker, ml);
      out[ml] = ' ';
      memcpy(out + ml + 1, model, kept + 1);
      return;
    }
  }
  truncateWords(type, max_chars, out, out_len);
}

/** True for an all-capitals word of letters/hyphens ("BOEING", "BRITTEN-NORMAN"). */
inline bool isCapsWord(const char* w, size_t len) {
  bool letter = false;
  for (size_t i = 0; i < len; ++i) {
    if (w[i] >= 'A' && w[i] <= 'Z') letter = true;
    else if (w[i] != '-') return false;
  }
  return letter;
}

/**
 * adsb.fi "desc" (ICAO type designator description, e.g. "BOEING 737 MAX 8",
 * "AIRBUS A-321neo", "BRITTEN-NORMAN BN-2T Turbine Islander") -> label text.
 * Leading all-capitals words are the manufacturer: they are title-cased
 * (segments of 3 letters or fewer, like "ATR" or "MAX", stay capitals). For
 * Airbus, "A-321neo" becomes "A321neo". If manufacturer + model exceed
 * max_chars, the model alone is kept (see formatType).
 */
inline void formatDescription(const char* desc, size_t max_chars, char* out,
                              size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  char buf[64];
  foldToAscii(desc, buf, sizeof(buf));
  if (!buf[0]) return;

  // Split at the first word that is not an all-capitals word; always leave
  // at least one word as the model.
  size_t split = 0;
  for (size_t i = 0;;) {
    while (buf[i] == ' ') ++i;
    if (!buf[i]) break;
    size_t j = i;
    while (buf[j] && buf[j] != ' ') ++j;
    if (!buf[j] || !isCapsWord(buf + i, j - i)) break;
    split = j;
    i = j;
  }
  char maker[40] = {};
  const char* model = buf;
  if (split > 0) {
    const size_t ml = split < sizeof(maker) - 1 ? split : sizeof(maker) - 1;
    memcpy(maker, buf, ml);
    model = buf + split;
    while (*model == ' ') ++model;
    // Title-case each hyphen-separated segment longer than three letters.
    for (size_t s = 0; s < ml;) {
      size_t e = s;
      while (e < ml && maker[e] != ' ' && maker[e] != '-') ++e;
      if (e - s > 3 || (e - s == 2 && maker[s] == 'D' && maker[s + 1] == 'E')) {
        for (size_t k = s + 1; k < e; ++k) maker[k] = static_cast<char>(maker[k] - 'A' + 'a');
      }
      s = e + 1;
    }
  }
  char model_buf[64];
  const size_t model_len = strlen(model);
  memcpy(model_buf, model, model_len + 1);
  if (strcmp(maker, "Airbus") == 0 && model_buf[0] == 'A' && model_buf[1] == '-' &&
      model_buf[2] >= '0' && model_buf[2] <= '9') {
    memmove(model_buf + 1, model_buf + 2, model_len - 1);  // "A-321neo" -> "A321neo"
  }
  formatType(maker, model_buf, max_chars, out, out_len);
}

}  // namespace services::flight_info::text
