
#pragma once
#include "wled.h"

/*
 * Weather Light Patterns
 * ---------------------
 * Three independent pattern setters plus combination helpers.
 *
 *   setLightsByCondition(conditionCode)  - WeatherAPI condition code  -> effect/color
 *   setLightsByTemperature(tempC)        - Celsius temperature         -> color gradient
 *   setLightsByTime(hour24)              - Hour of day (0-23)          -> color temperature
 *
 * Combinations (all apply in order: time base -> temp overlay -> condition):
 *   setLightsByConditionAndTemperature(code, tempC)
 *   setLightsByConditionAndTime(code, hour24)
 *   setLightsByTemperatureAndTime(tempC, hour24)
 *   setLightsByAll(code, tempC, hour24)
 *
 * All functions operate on the main WLED segment.
 * Call stateChanged() / colorUpdated() after if you need the web UI to reflect changes immediately.
 */

// ---------------------------------------------------------------------------
// Helper: apply mode + colors to the main segment in one call
// ---------------------------------------------------------------------------
inline static void _applyPattern(uint8_t fxMode, uint32_t color0,
                           uint32_t color1 = 0x000000,
                           uint8_t  spd    = 128,
                           uint8_t  inten  = 128,
                           uint8_t  pal    = 0)
{
  Segment& seg = strip.getMainSegment();
  seg.setMode(fxMode)
     .setPalette(pal)
     .setColor(0, color0)
     .setColor(1, color1);
  seg.speed     = spd;
  seg.intensity = inten;
}

// ---------------------------------------------------------------------------
// 1. Set lights based on weather condition code
//    Every WeatherAPI condition code gets a distinct effect + colors.
//    Within each category, speed/intensity scale with severity.
// ---------------------------------------------------------------------------
inline void setLightsByCondition(int conditionCode)
{
  switch (conditionCode) {

    // --- Clear ---
    case 1000: // Clear / Sunny
      _applyPattern(FX_MODE_BREATH, 0xFFD700, 0xFFAA00, 80, 140);
      break;

    // --- Cloudy ---
    case 1003: // Partly cloudy
      _applyPattern(FX_MODE_FADE, 0xD0E8FF, 0x80A0C0, 50, 80);
      break;
    case 1006: // Cloudy
      _applyPattern(FX_MODE_FADE, 0xB0C8E0, 0x506070, 40, 90);
      break;
    case 1009: // Overcast
      _applyPattern(FX_MODE_STATIC, 0x8899AA);
      break;

    // --- Mist / Fog ---
    case 1030: // Mist
      _applyPattern(FX_MODE_BREATH, 0xCCDDEE, 0x556677, 35, 50);
      break;
    case 1135: // Fog
      _applyPattern(FX_MODE_BREATH, 0xAABBCC, 0x445566, 25, 45);
      break;
    case 1147: // Freezing fog
      _applyPattern(FX_MODE_BREATH, 0x99BBDD, 0x224466, 20, 40);
      break;

    // --- Patchy possibilities ---
    case 1063: // Patchy rain possible
      _applyPattern(FX_MODE_BREATH, 0x4488CC, 0x002244, 60, 80);
      break;
    case 1066: // Patchy snow possible
      _applyPattern(FX_MODE_TWINKLECAT, 0xDDEEFF, 0x6688AA, 50, 80);
      break;
    case 1069: // Patchy sleet possible
      _applyPattern(FX_MODE_TWINKLECAT, 0xAABBDD, 0x334466, 55, 90);
      break;
    case 1072: // Patchy freezing drizzle possible
      _applyPattern(FX_MODE_BREATH, 0x6699CC, 0x001133, 50, 70);
      break;

    // --- Thunderstorms (severity: possible < light+thunder < heavy+thunder) ---
    case 1087: // Thundery outbreaks possible
      _applyPattern(FX_MODE_LIGHTNING, 0xFFFFFF, 0x000033, 60, 80);
      break;
    case 1273: // Patchy light rain with thunder
      _applyPattern(FX_MODE_LIGHTNING, 0xCCDDFF, 0x000022, 90, 120);
      break;
    case 1276: // Moderate or heavy rain with thunder
      _applyPattern(FX_MODE_LIGHTNING, 0xFFFFFF, 0x000044, 130, 180);
      break;
    case 1279: // Patchy light snow with thunder
      _applyPattern(FX_MODE_LIGHTNING, 0xDDEEFF, 0x001133, 90, 120);
      break;
    case 1282: // Moderate or heavy snow with thunder
      _applyPattern(FX_MODE_LIGHTNING, 0xEEFFFF, 0x002244, 130, 180);
      break;

    // --- Blowing snow / Blizzard ---
    case 1114: // Blowing snow
      _applyPattern(FX_MODE_RUNNING_LIGHTS, 0xDDEEFF, 0x6688AA, 120, 160);
      break;
    case 1117: // Blizzard
      _applyPattern(FX_MODE_RUNNING_LIGHTS, 0xEEF4FF, 0x8899BB, 200, 220);
      break;

    // --- Drizzle ---
    case 1150: // Patchy light drizzle
      _applyPattern(FX_MODE_DRIP, 0x4477BB, 0x001122, 50, 60);
      break;
    case 1153: // Light drizzle
      _applyPattern(FX_MODE_DRIP, 0x3366AA, 0x001122, 65, 80);
      break;
    case 1168: // Freezing drizzle
      _applyPattern(FX_MODE_DRIP, 0x5588CC, 0x001133, 75, 95);
      break;
    case 1171: // Heavy freezing drizzle
      _applyPattern(FX_MODE_DRIP, 0x3377CC, 0x001144, 110, 140);
      break;

    // --- Rain ---
    case 1180: // Patchy light rain
      _applyPattern(FX_MODE_DRIP, 0x2255AA, 0x001133, 65, 90);
      break;
    case 1183: // Light rain
      _applyPattern(FX_MODE_DRIP, 0x1144AA, 0x001133, 80, 105);
      break;
    case 1186: // Moderate rain at times
      _applyPattern(FX_MODE_DRIP, 0x0044BB, 0x001144, 95, 125);
      break;
    case 1189: // Moderate rain
      _applyPattern(FX_MODE_DRIP, 0x0033AA, 0x001144, 110, 140);
      break;
    case 1192: // Heavy rain at times
      _applyPattern(FX_MODE_DRIP, 0x0022AA, 0x001155, 130, 160);
      break;
    case 1195: // Heavy rain
      _applyPattern(FX_MODE_DRIP, 0x0011AA, 0x000044, 155, 185);
      break;
    case 1198: // Light freezing rain
      _applyPattern(FX_MODE_DRIP, 0x3366BB, 0x001133, 85, 115);
      break;
    case 1201: // Moderate or heavy freezing rain
      _applyPattern(FX_MODE_DRIP, 0x1144BB, 0x001144, 130, 160);
      break;

    // --- Sleet ---
    case 1204: // Light sleet
      _applyPattern(FX_MODE_TWINKLECAT, 0x7799BB, 0x223355, 70, 100);
      break;
    case 1207: // Moderate or heavy sleet
      _applyPattern(FX_MODE_TWINKLECAT, 0x5577AA, 0x112244, 110, 150);
      break;
    case 1249: // Light sleet showers
      _applyPattern(FX_MODE_TWINKLECAT, 0x7799CC, 0x223366, 75, 105);
      break;
    case 1252: // Moderate or heavy sleet showers
      _applyPattern(FX_MODE_TWINKLECAT, 0x5577BB, 0x112255, 115, 155);
      break;

    // --- Snow ---
    case 1210: // Patchy light snow
      _applyPattern(FX_MODE_TWINKLECAT, 0xEEF4FF, 0x99AABB, 40, 80);
      break;
    case 1213: // Light snow
      _applyPattern(FX_MODE_TWINKLECAT, 0xEEF4FF, 0x8899BB, 60, 105);
      break;
    case 1216: // Patchy moderate snow
      _applyPattern(FX_MODE_TWINKLECAT, 0xDDEEFF, 0x7788AA, 75, 125);
      break;
    case 1219: // Moderate snow
      _applyPattern(FX_MODE_TWINKLECAT, 0xDDEEFF, 0x6677AA, 90, 145);
      break;
    case 1222: // Patchy heavy snow
      _applyPattern(FX_MODE_TWINKLECAT, 0xCCDDFF, 0x556699, 110, 165);
      break;
    case 1225: // Heavy snow
      _applyPattern(FX_MODE_TWINKLECAT, 0xCCDDFF, 0x445588, 130, 185);
      break;
    case 1237: // Ice pellets
      _applyPattern(FX_MODE_SPARKLE, 0xAABBDD, 0x334466, 90, 150);
      break;
    case 1255: // Light snow showers
      _applyPattern(FX_MODE_TWINKLECAT, 0xEEF4FF, 0x8899BB, 65, 115);
      break;
    case 1258: // Moderate or heavy snow showers
      _applyPattern(FX_MODE_TWINKLECAT, 0xDDEEFF, 0x6677AA, 100, 155);
      break;
    case 1261: // Light showers of ice pellets
      _applyPattern(FX_MODE_SPARKLE, 0xBBCCEE, 0x445577, 80, 130);
      break;
    case 1264: // Moderate or heavy showers of ice pellets
      _applyPattern(FX_MODE_SPARKLE, 0xAABBDD, 0x334466, 120, 175);
      break;

    // --- Rain showers ---
    case 1240: // Light rain shower
      _applyPattern(FX_MODE_DRIP, 0x2255BB, 0x001133, 85, 115);
      break;
    case 1243: // Moderate or heavy rain shower
      _applyPattern(FX_MODE_DRIP, 0x0044CC, 0x001144, 130, 160);
      break;
    case 1246: // Torrential rain shower
      _applyPattern(FX_MODE_DRIP, 0x0022DD, 0x000055, 190, 210);
      break;

    default:
      _applyPattern(FX_MODE_STATIC, 0x4466AA);
      break;
  }
}

// ---------------------------------------------------------------------------
// 2. Set lights based on outside temperature (Celsius)
//    Cold -> blue  |  Mild -> green  |  Warm -> orange  |  Hot -> red
// ---------------------------------------------------------------------------
inline void setLightsByTemperature(float tempC)
{
  uint32_t color;
  uint8_t  spd   = 90;
  uint8_t  inten = 140;

  if (tempC <= 0.0f) {
    // Freezing - icy blue, slow pulse
    color = 0x0088FF;
    _applyPattern(FX_MODE_BREATH, color, 0x003366, 50, 100);

  } else if (tempC <= 10.0f) {
    // Cold - cyan-blue, calm running lights
    color = 0x00CCFF;
    _applyPattern(FX_MODE_RUNNING_LIGHTS, color, 0x004466, spd, inten);

  } else if (tempC <= 18.0f) {
    // Cool - green-cyan, static
    color = 0x00FF88;
    _applyPattern(FX_MODE_STATIC, color);

  } else if (tempC <= 24.0f) {
    // Mild / comfortable - warm white, gentle breath
    color = 0xFFEEAA;
    _applyPattern(FX_MODE_BREATH, color, 0xCC9900, 70, 120);

  } else if (tempC <= 32.0f) {
    // Warm - orange, slow comet
    color = 0xFF6600;
    _applyPattern(FX_MODE_COMET, color, 0x441100, spd, inten);

  } else {
    // Hot - deep red, fire flicker
    color = 0xFF1100;
    _applyPattern(FX_MODE_FIRE_FLICKER, color, 0x440000, 90, 130);
  }
}

// ---------------------------------------------------------------------------
// 3. Set lights based on time of day (hour, 0-23)
//    Night deep blue -> dawn pink -> day bright white -> dusk orange -> night
// ---------------------------------------------------------------------------
inline void setLightsByTime(int hour24)
{
  if (hour24 >= 23 || hour24 < 5) {
    // Deep night (11pm - 4:59am) - very dim deep blue
    _applyPattern(FX_MODE_STATIC, 0x000033);
    strip.getMainSegment().opacity = 40;

  } else if (hour24 < 7) {
    // Dawn (5am - 6:59am) - soft pink/orange sunrise
    _applyPattern(FX_MODE_SUNRISE, 0xFF6633, 0x220011, 60, 128);

  } else if (hour24 < 11) {
    // Morning (7am - 10:59am) - warm white building up
    _applyPattern(FX_MODE_BREATH, 0xFFDDAA, 0xFFAA44, 50, 100);

  } else if (hour24 < 16) {
    // Midday (11am - 3:59pm) - bright cool white
    _applyPattern(FX_MODE_STATIC, 0xFFFFEE);
    strip.getMainSegment().opacity = 255;

  } else if (hour24 < 19) {
    // Afternoon (4pm - 6:59pm) - warm golden
    _applyPattern(FX_MODE_STATIC, 0xFFCC44);

  } else if (hour24 < 21) {
    // Dusk (7pm - 8:59pm) - orange-red sunset
    _applyPattern(FX_MODE_SUNRISE, 0xFF3300, 0x110000, 40, 180);

  } else {
    // Evening (9pm - 10:59pm) - dim warm amber winding down
    _applyPattern(FX_MODE_FADE, 0xFF6600, 0x110000, 30, 80);
  }
}

// ---------------------------------------------------------------------------
// 4. Combination: condition + temperature
//    Uses condition for the effect, temperature to tint the primary color.
// ---------------------------------------------------------------------------
inline void setLightsByConditionAndTemperature(int conditionCode, float tempC)
{
  // Start with condition-based effect
  setLightsByCondition(conditionCode);

  // Blend the temperature tint into color slot 1 as a hint
  uint32_t tempTint;
  if      (tempC <= 0.0f)  tempTint = 0x0044AA;  // icy blue tint
  else if (tempC <= 10.0f) tempTint = 0x0088CC;  // cool tint
  else if (tempC <= 18.0f) tempTint = 0x44CC88;  // mild tint
  else if (tempC <= 24.0f) tempTint = 0xFFDD88;  // comfortable tint
  else if (tempC <= 32.0f) tempTint = 0xFF8800;  // warm tint
  else                     tempTint = 0xFF2200;  // hot tint

  strip.getMainSegment().setColor(1, tempTint);
}

// ---------------------------------------------------------------------------
// 5. Combination: condition + time of day
//    Uses condition effect but modulates brightness and secondary color by time.
// ---------------------------------------------------------------------------
inline void setLightsByConditionAndTime(int conditionCode, int hour24)
{
  setLightsByCondition(conditionCode);

  // Dim the segment at night, brighten during the day
  uint8_t brightness;
  if      (hour24 >= 23 || hour24 < 5) brightness = 30;
  else if (hour24 < 7)                 brightness = 80;
  else if (hour24 < 19)                brightness = 220;
  else if (hour24 < 21)                brightness = 140;
  else                                 brightness = 60;

  strip.getMainSegment().opacity = brightness;
}

// ---------------------------------------------------------------------------
// 6. Combination: temperature + time of day
//    Uses time-of-day as the base, overlays temperature as the primary color.
// ---------------------------------------------------------------------------
inline void setLightsByTemperatureAndTime(float tempC, int hour24)
{
  setLightsByTime(hour24);

  // Override primary color with temperature color, keeping time effect
  uint32_t tempColor;
  if      (tempC <= 0.0f)  tempColor = 0x0088FF;
  else if (tempC <= 10.0f) tempColor = 0x00CCFF;
  else if (tempC <= 18.0f) tempColor = 0x00FF88;
  else if (tempC <= 24.0f) tempColor = 0xFFEEAA;
  else if (tempC <= 32.0f) tempColor = 0xFF6600;
  else                     tempColor = 0xFF1100;

  strip.getMainSegment().setColor(0, tempColor);
}

// ---------------------------------------------------------------------------
// 7. Full combination: condition + temperature + time of day
//    Time sets brightness envelope, condition sets effect, temperature tints.
// ---------------------------------------------------------------------------
inline void setLightsByAll(int conditionCode, float tempC, int hour24)
{
  // Layer 1: apply condition effect + colors
  setLightsByCondition(conditionCode);

  // Layer 2: tint color slot 1 with temperature
  uint32_t tempTint;
  if      (tempC <= 0.0f)  tempTint = 0x0044AA;
  else if (tempC <= 10.0f) tempTint = 0x0088CC;
  else if (tempC <= 18.0f) tempTint = 0x44CC88;
  else if (tempC <= 24.0f) tempTint = 0xFFDD88;
  else if (tempC <= 32.0f) tempTint = 0xFF8800;
  else                     tempTint = 0xFF2200;

  strip.getMainSegment().setColor(1, tempTint);

  // Layer 3: time-of-day brightness envelope
  uint8_t bri;
  if      (hour24 >= 23 || hour24 < 5) bri = 30;
  else if (hour24 < 7)                 bri = 80;
  else if (hour24 < 11)                bri = 160;
  else if (hour24 < 16)                bri = 255;
  else if (hour24 < 19)                bri = 200;
  else if (hour24 < 21)                bri = 120;
  else                                 bri = 60;

  strip.getMainSegment().opacity = bri;
}


// ===========================================================================
// Condition utility functions (7b-7d)
// Shared helpers for all usermods that consume WeatherAPI condition codes.
// These are pure functions with no external dependencies.
// ===========================================================================

// ---------------------------------------------------------------------------
// 7b. Condition severity: 1=mild, 2=moderate, 3=severe
//     Used to append a digit after the 3-letter abbreviation on a 4-digit
//     display (e.g. "rAn2" = moderate rain).
// ---------------------------------------------------------------------------
inline uint8_t conditionSeverity(int code) {
  if (code == 1000) return 1;  // Clear
  if (code == 1003) return 1;  if (code == 1006) return 2;  if (code == 1009) return 3;  // Cloudy
  if (code == 1030) return 1;  if (code == 1135) return 2;  if (code == 1147) return 3;  // Fog
  if (code == 1087) return 1;  // Thunder possible
  if (code == 1273 || code == 1279) return 2;
  if (code == 1276 || code == 1282) return 3;
  if (code == 1210 || code == 1213 || code == 1255) return 1;  // Snow light
  if (code == 1114 || code == 1216 || code == 1219 || code == 1237 ||
      code == 1258 || code == 1261) return 2;                  // Snow moderate
  if (code == 1117 || code == 1222 || code == 1225 || code == 1264) return 3;  // Snow heavy
  if (code == 1063 || code == 1069 || code == 1072 || code == 1150 ||
      code == 1153 || code == 1180 || code == 1183 || code == 1198 ||
      code == 1204 || code == 1240 || code == 1249) return 1;  // Rain light
  if (code == 1186 || code == 1189 || code == 1201 || code == 1207 ||
      code == 1243 || code == 1252) return 2;                  // Rain moderate
  if (code == 1171 || code == 1192 || code == 1195 || code == 1246) return 3;  // Rain heavy
  return 1;  // default mild
}

// ---------------------------------------------------------------------------
// 7c. Human-readable condition description (all 49 WeatherAPI codes)
// ---------------------------------------------------------------------------
inline const char* conditionDescription(int code) {
  switch (code) {
    case 1000: return "Clear";
    case 1003: return "Partly cloudy";
    case 1006: return "Cloudy";
    case 1009: return "Overcast";
    case 1030: return "Mist";
    case 1063: return "Patchy rain possible";
    case 1066: return "Patchy snow possible";
    case 1069: return "Patchy sleet possible";
    case 1072: return "Patchy freezing drizzle possible";
    case 1087: return "Thundery outbreaks possible";
    case 1114: return "Blowing snow";
    case 1117: return "Blizzard";
    case 1135: return "Fog";
    case 1147: return "Freezing fog";
    case 1150: return "Patchy light drizzle";
    case 1153: return "Light drizzle";
    case 1168: return "Freezing drizzle";
    case 1171: return "Heavy freezing drizzle";
    case 1180: return "Patchy light rain";
    case 1183: return "Light rain";
    case 1186: return "Moderate rain at times";
    case 1189: return "Moderate rain";
    case 1192: return "Heavy rain at times";
    case 1195: return "Heavy rain";
    case 1198: return "Light freezing rain";
    case 1201: return "Moderate or heavy freezing rain";
    case 1204: return "Light sleet";
    case 1207: return "Moderate or heavy sleet";
    case 1210: return "Patchy light snow";
    case 1213: return "Light snow";
    case 1216: return "Patchy moderate snow";
    case 1219: return "Moderate snow";
    case 1222: return "Patchy heavy snow";
    case 1225: return "Heavy snow";
    case 1237: return "Ice pellets";
    case 1240: return "Light rain shower";
    case 1243: return "Moderate or heavy rain shower";
    case 1246: return "Torrential rain shower";
    case 1249: return "Light sleet showers";
    case 1252: return "Moderate or heavy sleet showers";
    case 1255: return "Light snow showers";
    case 1258: return "Moderate or heavy snow showers";
    case 1261: return "Light showers of ice pellets";
    case 1264: return "Moderate or heavy showers of ice pellets";
    case 1273: return "Patchy light rain with thunder";
    case 1276: return "Moderate or heavy rain with thunder";
    case 1279: return "Patchy light snow with thunder";
    case 1282: return "Moderate or heavy snow with thunder";
    default:   return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// 7d. Condition category index: 0=Clear, 1=Cloudy, 2=Fog, 3=Thunder, 4=Snow, 5=Rain
//     Used to map a condition code to a per-category preset or LED override.
//     Note: code 1066 (Patchy snow possible) maps to Snow (4), not Rain.
// ---------------------------------------------------------------------------
inline uint8_t conditionToCategory(int code) {
  if (code == 1000) return 0;  // Clear
  if (code <= 1009) return 1;  // Cloudy/Overcast
  if (code == 1030 || code == 1135 || code == 1147) return 2;  // Fog/Mist
  if (code == 1087 || code >= 1273) return 3;  // Thunder/Storm
  if (code == 1066 ||
      (code >= 1114 && code <= 1117) ||
      (code >= 1210 && code <= 1264)) return 4;  // Snow/Ice
  return 5;  // Rain/Drizzle (default)
}

// ===========================================================================
// Palette selection functions (8-14)
// ===========================================================================
//
// Returns a WLED built-in palette index (uint8_t, 0-71) matched to weather
// condition, temperature, time of day, or combinations of the three.
//
// Pass the returned index to _applyPattern(..., pal) or call
// strip.getMainSegment().setPalette(idx) directly.
//
// Quick reference — selected built-in palette indices:
//   7  Cloud        8  Lava         9  Ocean       10  Forest
//  13  Sunset      14  Rivendell   15  Breeze      20  Pastel
//  21  Sunset 2    23  Vintage     24  Departure   26  Beech
//  35  Fire        36  Icefire     39  Autumn      44  Orange & Teal
//  45  Tiamat      46  April Night 47  Orangery    49  Sakura
//  50  Aurora      51  Atlantica   55  Aurora 2    58  Toxy Reaf
//  60  Semi Blue   65  Lite Light  68  Red Shift
// ---------------------------------------------------------------------------

// Named palette index constants (WLP_ prefix avoids collision with WLED macros)
#define WLP_PAL_CLOUD        7   // Cloud
#define WLP_PAL_LAVA         8   // Lava
#define WLP_PAL_OCEAN        9   // Ocean
#define WLP_PAL_FOREST       10  // Forest
#define WLP_PAL_SUNSET       13  // Sunset - warm orange gradient
#define WLP_PAL_RIVENDELL    14  // Rivendell - dim warm amber
#define WLP_PAL_BREEZE       15  // Breeze - cool blue-grey
#define WLP_PAL_PASTEL       20  // Pastel - soft morning colours
#define WLP_PAL_SUNSET2      21  // Sunset 2
#define WLP_PAL_VINTAGE      23  // Vintage - dim warm evening
#define WLP_PAL_DEPARTURE    24  // Departure - orange-red transition
#define WLP_PAL_BEECH        26  // Beech - neutral warm comfort
#define WLP_PAL_FIRE_PAL     35  // Fire - hot red-orange
#define WLP_PAL_ICEFIRE      36  // Icefire - stark cold blue/white
#define WLP_PAL_AUTUMN       39  // Autumn - orange-brown foliage
#define WLP_PAL_ORANGE_TEAL  44  // Orange & Teal
#define WLP_PAL_TIAMAT       45  // Tiamat - dark electric purple
#define WLP_PAL_APRIL_NIGHT  46  // April Night - deep inky blue
#define WLP_PAL_ORANGERY     47  // Orangery - warm orange-gold
#define WLP_PAL_SAKURA       49  // Sakura - pink/gold dawn
#define WLP_PAL_AURORA       50  // Aurora - cool blue-green
#define WLP_PAL_ATLANTICA    51  // Atlantica - deep cool blues
#define WLP_PAL_AURORA2      55  // Aurora 2
#define WLP_PAL_TOXY_REAF    58  // Toxy Reaf - cool green-cyan
#define WLP_PAL_SEMI_BLUE    60  // Semi Blue - pale misty cool
#define WLP_PAL_LITE_LIGHT   65  // Lite Light - bright near-white
#define WLP_PAL_RED_SHIFT    68  // Red Shift - hot red, dark background

// ---------------------------------------------------------------------------
// 8. Get palette index based on weather condition code
//    Every WeatherAPI condition code is explicitly mapped.
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByCondition(int conditionCode)
{
  switch (conditionCode) {

    // --- Clear ---
    case 1000: return WLP_PAL_ORANGERY;      // warm orange-gold

    // --- Cloudy ---
    case 1003: return WLP_PAL_PASTEL;        // partly cloudy: soft morning
    case 1006: return WLP_PAL_BREEZE;        // cloudy: cool blue-grey
    case 1009: return WLP_PAL_CLOUD;         // overcast: cloud palette

    // --- Mist / Fog ---
    case 1030: return WLP_PAL_SEMI_BLUE;     // mist: pale misty cool
    case 1135: return WLP_PAL_SEMI_BLUE;     // fog: pale misty cool
    case 1147: return WLP_PAL_ATLANTICA;     // freezing fog: deep cool blues

    // --- Patchy possibilities ---
    case 1063: return WLP_PAL_OCEAN;         // patchy rain possible
    case 1066: return WLP_PAL_AURORA;        // patchy snow possible
    case 1069: return WLP_PAL_AURORA2;       // patchy sleet possible
    case 1072: return WLP_PAL_ATLANTICA;     // patchy freezing drizzle possible

    // --- Thunderstorms ---
    case 1087: return WLP_PAL_TIAMAT;        // thundery outbreaks possible
    case 1273: return WLP_PAL_TIAMAT;        // patchy light rain with thunder
    case 1276: return WLP_PAL_APRIL_NIGHT;   // moderate/heavy rain with thunder
    case 1279: return WLP_PAL_TIAMAT;        // patchy light snow with thunder
    case 1282: return WLP_PAL_APRIL_NIGHT;   // moderate/heavy snow with thunder

    // --- Blowing snow / Blizzard ---
    case 1114: return WLP_PAL_AURORA2;       // blowing snow
    case 1117: return WLP_PAL_ICEFIRE;       // blizzard

    // --- Drizzle ---
    case 1150: return WLP_PAL_OCEAN;         // patchy light drizzle
    case 1153: return WLP_PAL_OCEAN;         // light drizzle
    case 1168: return WLP_PAL_ATLANTICA;     // freezing drizzle
    case 1171: return WLP_PAL_APRIL_NIGHT;   // heavy freezing drizzle

    // --- Rain ---
    case 1180: return WLP_PAL_OCEAN;         // patchy light rain
    case 1183: return WLP_PAL_OCEAN;         // light rain
    case 1186: return WLP_PAL_ATLANTICA;     // moderate rain at times
    case 1189: return WLP_PAL_ATLANTICA;     // moderate rain
    case 1192: return WLP_PAL_APRIL_NIGHT;   // heavy rain at times
    case 1195: return WLP_PAL_APRIL_NIGHT;   // heavy rain
    case 1198: return WLP_PAL_ATLANTICA;     // light freezing rain
    case 1201: return WLP_PAL_APRIL_NIGHT;   // moderate/heavy freezing rain

    // --- Sleet ---
    case 1204: return WLP_PAL_BREEZE;        // light sleet
    case 1207: return WLP_PAL_AURORA2;       // moderate/heavy sleet
    case 1249: return WLP_PAL_BREEZE;        // light sleet showers
    case 1252: return WLP_PAL_AURORA2;       // moderate/heavy sleet showers

    // --- Snow ---
    case 1210: return WLP_PAL_AURORA;        // patchy light snow
    case 1213: return WLP_PAL_AURORA;        // light snow
    case 1216: return WLP_PAL_AURORA2;       // patchy moderate snow
    case 1219: return WLP_PAL_AURORA2;       // moderate snow
    case 1222: return WLP_PAL_ICEFIRE;       // patchy heavy snow
    case 1225: return WLP_PAL_ICEFIRE;       // heavy snow
    case 1237: return WLP_PAL_ATLANTICA;     // ice pellets
    case 1255: return WLP_PAL_AURORA;        // light snow showers
    case 1258: return WLP_PAL_ICEFIRE;       // moderate/heavy snow showers
    case 1261: return WLP_PAL_ATLANTICA;     // light ice pellet showers
    case 1264: return WLP_PAL_ICEFIRE;       // moderate/heavy ice pellet showers

    // --- Rain showers ---
    case 1240: return WLP_PAL_OCEAN;         // light rain shower
    case 1243: return WLP_PAL_ATLANTICA;     // moderate/heavy rain shower
    case 1246: return WLP_PAL_APRIL_NIGHT;   // torrential rain shower

    default:   return WLP_PAL_OCEAN;         // fallback
  }
}

// ---------------------------------------------------------------------------
// 9. Get palette index based on temperature (Celsius)
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByTemperature(float tempC)
{
  if (tempC <= 0.0f)  return WLP_PAL_ICEFIRE;    // 36: stark cold
  if (tempC <= 10.0f) return WLP_PAL_ATLANTICA;  // 51: deep cool blues
  if (tempC <= 18.0f) return WLP_PAL_TOXY_REAF;  // 58: cool green-cyan
  if (tempC <= 24.0f) return WLP_PAL_BEECH;      // 26: neutral warm comfort
  if (tempC <= 32.0f) return WLP_PAL_SUNSET;     // 13: warm orange gradient
  return               WLP_PAL_FIRE_PAL;         // 35: hot red-orange
}

// ---------------------------------------------------------------------------
// 10. Get palette index based on time of day (hour, 0-23)
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByTime(int hour24)
{
  if (hour24 >= 23 || hour24 < 5) return WLP_PAL_APRIL_NIGHT; // 46: deep night
  if (hour24 < 7)                 return WLP_PAL_SAKURA;      // 49: pink/gold dawn
  if (hour24 < 11)                return WLP_PAL_PASTEL;      // 20: soft morning
  if (hour24 < 16)                return WLP_PAL_LITE_LIGHT;  // 65: bright midday
  if (hour24 < 19)                return WLP_PAL_ORANGERY;    // 47: golden afternoon
  if (hour24 < 21)                return WLP_PAL_DEPARTURE;   // 24: orange-red dusk
  return                           WLP_PAL_VINTAGE;           // 23: dim warm evening
}

// ---------------------------------------------------------------------------
// 11. Combination: condition + temperature
//     Condition is the primary selector; extreme temps (<=0 or >32°C) override.
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByConditionAndTemperature(int conditionCode, float tempC)
{
  // Snow/ice conditions always use cold palettes regardless of temp
  if ((conditionCode >= 1114 && conditionCode <= 1117) ||
      (conditionCode >= 1210 && conditionCode <= 1264)) {
    return (tempC <= 0.0f) ? WLP_PAL_ICEFIRE : WLP_PAL_ATLANTICA;
  }
  // Thunderstorm: cold storm = darker, warmer storm = electric
  if (conditionCode == 1087 || conditionCode >= 1273) {
    return (tempC <= 10.0f) ? WLP_PAL_APRIL_NIGHT : WLP_PAL_TIAMAT;
  }
  // Extreme temperatures override condition
  if (tempC <= 0.0f)  return WLP_PAL_ICEFIRE;
  if (tempC > 32.0f)  return WLP_PAL_FIRE_PAL;
  // Clear sky modulated by temperature feel
  if (conditionCode == 1000) {
    if (tempC > 24.0f)  return WLP_PAL_SUNSET;    // warm clear day
    if (tempC <= 18.0f) return WLP_PAL_AURORA;    // cool clear day (50)
    return               WLP_PAL_ORANGERY;        // comfortable clear day
  }
  // Default: use condition palette
  return getPaletteByCondition(conditionCode);
}

// ---------------------------------------------------------------------------
// 12. Combination: condition + time of day
//     Night always dominates; dawn/dusk apply transitional palettes.
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByConditionAndTime(int conditionCode, int hour24)
{
  bool isNight   = (hour24 >= 23 || hour24 < 5);
  bool isEvening = (hour24 >= 21 && hour24 < 23);
  bool isDusk    = (hour24 >= 19 && hour24 < 21);
  bool isDawn    = (hour24 >= 5  && hour24 < 7);

  if (isNight) {
    // Snowy nights still look icy rather than inky dark
    if ((conditionCode >= 1114 && conditionCode <= 1117) ||
        (conditionCode >= 1210 && conditionCode <= 1264)) return WLP_PAL_SEMI_BLUE;
    return WLP_PAL_APRIL_NIGHT;
  }
  if (isEvening) {
    return (conditionCode == 1000) ? WLP_PAL_VINTAGE : WLP_PAL_RIVENDELL;
  }
  if (isDusk) {
    return (conditionCode == 1000) ? WLP_PAL_DEPARTURE : WLP_PAL_AUTUMN;
  }
  if (isDawn) {
    return (conditionCode == 1000) ? WLP_PAL_SAKURA : WLP_PAL_ORANGE_TEAL;
  }
  // Daytime: use condition palette
  return getPaletteByCondition(conditionCode);
}

// ---------------------------------------------------------------------------
// 13. Combination: temperature + time of day
//     Night darkens the palette; dawn/dusk use transitional aurora/sunset blends.
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByTemperatureAndTime(float tempC, int hour24)
{
  bool isNight = (hour24 >= 23 || hour24 < 5);
  bool isDay   = (hour24 >= 7  && hour24 < 19);

  if (isNight) {
    if (tempC <= 0.0f)  return WLP_PAL_ICEFIRE;      // freezing night
    if (tempC <= 10.0f) return WLP_PAL_SEMI_BLUE;    // cold night
    if (tempC <= 18.0f) return WLP_PAL_APRIL_NIGHT;  // cool night
    if (tempC <= 24.0f) return WLP_PAL_RIVENDELL;    // mild night
    if (tempC <= 32.0f) return WLP_PAL_VINTAGE;      // warm night
    return               WLP_PAL_RED_SHIFT;          // hot night
  }
  if (!isDay) {
    // Dawn (5-6) or dusk/evening (19-22): transitional blends
    if (tempC <= 0.0f)  return WLP_PAL_AURORA2;      // 55: cold transition
    if (tempC <= 10.0f) return WLP_PAL_AURORA;       // 50: cool transition
    if (tempC <= 18.0f) return WLP_PAL_BEECH;        // 26: mild transition
    if (tempC <= 24.0f) return WLP_PAL_SUNSET2;      // 21: comfortable transition
    if (tempC <= 32.0f) return WLP_PAL_DEPARTURE;    // 24: warm transition
    return               WLP_PAL_AUTUMN;             // 39: hot transition
  }
  // Daytime: pure temperature palette
  return getPaletteByTemperature(tempC);
}

// ---------------------------------------------------------------------------
// 14. Full combination: condition + temperature + time of day
//     Priority order: night > extreme temp > transitional period > daytime.
// ---------------------------------------------------------------------------
inline uint8_t getPaletteByAll(int conditionCode, float tempC, int hour24)
{
  // Night always dominates
  if (hour24 >= 23 || hour24 < 5)
    return getPaletteByConditionAndTime(conditionCode, hour24);

  // Extreme temperatures override condition even during the day
  if (tempC <= 0.0f)  return WLP_PAL_ICEFIRE;
  if (tempC > 32.0f)  return WLP_PAL_FIRE_PAL;

  // Dawn or dusk/evening: blend condition with time context
  if (hour24 < 7 || hour24 >= 19)
    return getPaletteByConditionAndTime(conditionCode, hour24);

  // Daytime: condition + temperature combination
  return getPaletteByConditionAndTemperature(conditionCode, tempC);
}
