
#ifdef USERMOD_TM1637_CLOCK

#ifndef TM1637DISPLAYUSERMOD_FWDDECL
#define TM1637DISPLAYUSERMOD_FWDDECL
// Minimal stub for TM1637DisplayUsermod to allow pointer usage in this file
class TM1637DisplayUsermod {
public:
  void showMessage(const char*) {} // no-op stub
};
#endif
#include "wled.h"
#include "const.h"
#include "../weather_api/weather_light_patterns.h"
#ifdef USERMOD_WEATHER_API
  #include "../weather_api/usermod_weather_api.h"
#endif

/*
 * TM1637 Clock Weather LED Control Usermod
 * 
 * Applies WLED light patterns and color palettes based on real-time weather
 * conditions, temperature, and time of day.
 * 
 * Weather data is sourced from the WeatherApi usermod (USERMOD_WEATHER_API).
 * Display output (time, temp, conditions on a TM1637 4-digit display) is
 * handled separately by the tm1637_Display usermod.
 * 
 * This usermod focuses solely on LED pattern and palette selection based on
 * weather state, not on display rendering.
 */

// How often to re-apply weather-driven LED patterns (pattern modes can change
// based on time or sensor updates)
#define TM1637_PATTERN_APPLY_INTERVAL_MS 60000UL  // re-apply every 60 seconds

#ifndef TM1637DISPLAYUSERMOD_FWDDECL
#define TM1637DISPLAYUSERMOD_FWDDECL
class TM1637DisplayUsermod;
#endif
#include "../baseball_api/usermod_baseball_api.h"

class TM1637ClockUsermod : public Usermod {
  private:
    bool enabled = true;

    // Weather data received from WeatherApi usermod
    // Used only for LED pattern/palette selection, not for display
    int   weatherCondCode      = 1000;     // cached weather condition code
    float weatherTempCached    = 20.0f;    // cached temperature for LED pattern selection
    bool  weatherFetched       = false;    // true once weather data has been successfully received

    // Weather-driven LED pattern/palette configuration
    uint8_t weatherOverrideType  = 0;      // 0=none, 1=light pattern, 2=color palette
    uint8_t weatherPatternMode   = 6;      // 0=condition,1=temp,2=time,3=cond+temp,4=cond+time,5=temp+time,6=all
    uint8_t weatherPaletteMode   = 6;      // same indices as weatherPatternMode

    // Per-condition WLED preset overrides (0 = disabled, use weather pattern)
    // Index: 0=Clear, 1=Cloudy, 2=Fog, 3=Thunder, 4=Snow, 5=Rain
    uint8_t conditionPresetOverrides[6] = {0, 0, 0, 0, 0, 0};

    // Periodic pattern re-application timer
    unsigned long lastPatternApply = 0;
    
    static const char _name[];
    static const char _enabled[];
    static const char _overrideType[];
    static const char _lightMode[];
    static const char _paletteMode[];
    static const char _presetClear[];
    static const char _presetCloudy[];
    static const char _presetFog[];
    static const char _presetThunder[];
    static const char _presetSnow[];
    static const char _presetRain[];

    // Static instance pointer — set in setup() so subscription callbacks can access members.
    // Safe because TM1637ClockUsermod is a singleton (one instance via REGISTER_USERMOD).
    static TM1637ClockUsermod* _instance;
    // Pointer to TM1637 display usermod (set in setup if available)
    TM1637DisplayUsermod* tm1637Display = nullptr;


    // Pointer to Baseball API usermod (set in setup if available)
    UsermodBaseballAPI* baseballApi = nullptr;

    // Show a status message on the TM1637 display (4 chars max)
    void showStatusOnDisplay(const char* msg) {
      if (tm1637Display && msg) {
        tm1637Display->showMessage(msg);
      }
    }

    // Show a baseball score on the TM1637 display (favorite left, opponent right)
    void showBaseballScore(const String& fav, int favScore, const String& opp, int oppScore) {
      if (!tm1637Display) return;
      // Format: SS-SS (e.g., 3-2, 10-7)
      char buf[5];
      if (favScore < 10 && oppScore < 10) {
        snprintf(buf, sizeof(buf), "%d-%d", favScore, oppScore);
      } else {
        // For double-digit scores, show last digit only
        snprintf(buf, sizeof(buf), "%d-%d", favScore % 10, oppScore % 10);
      }
      tm1637Display->showMessage(buf);
    }

    // Helper to parse lastScore string and extract team names and scores (single favorite)
    bool parseBaseballScore(const String& lastScore, const String& favTeam, String& fav, int& favScore, String& opp, int& oppScore) {
      // Example: "Yankees vs Red Sox: 3-2 (LIVE)"
      int vsIdx = lastScore.indexOf(" vs ");
      int colonIdx = lastScore.indexOf(": ");
      int dashIdx = lastScore.indexOf("-");
      if (vsIdx < 0 || colonIdx < 0 || dashIdx < 0) return false;
      String home = lastScore.substring(0, vsIdx);
      String away = lastScore.substring(vsIdx + 4, colonIdx);
      String scorePart = lastScore.substring(colonIdx + 2, lastScore.indexOf(" ", colonIdx + 2));
      int dash = scorePart.indexOf("-");
      if (dash < 0) return false;
      int homeScore = scorePart.substring(0, dash).toInt();
      int awayScore = scorePart.substring(dash + 1).toInt();
      // Determine which is favorite
      if (home.equalsIgnoreCase(favTeam)) {
        fav = home; favScore = homeScore; opp = away; oppScore = awayScore;
        return true;
      } else if (away.equalsIgnoreCase(favTeam)) {
        fav = away; favScore = awayScore; opp = home; oppScore = homeScore;
        return true;
      }
      return false;
    }

#ifdef USERMOD_WEATHER_API
    // Subscription callbacks for WeatherApiUsermod.
    // _onTempC caches the temperature for pattern selection.
    static void _onTempC(JsonVariant v) {
      if (!_instance) return;
      _instance->weatherTempCached = v.as<float>();
      { char tBuf[8]; dtostrf(_instance->weatherTempCached, 4, 1, tBuf);
        DEBUG_PRINTF("TM1637 Clock: weather temp_c=%s\n", tBuf); }
    }

    // _onCondCode fires when a new weather condition code arrives.
    // Triggers immediate LED pattern re-apply if overrides are enabled.
    static void _onCondCode(JsonVariant v) {
      if (!_instance) return;
      _instance->weatherCondCode = v.as<int>();
      _instance->weatherFetched  = true;
      DEBUG_PRINTF("TM1637 Clock: weather condition_code=%d\n", _instance->weatherCondCode);
      if (_instance->weatherOverrideType > 0) {
        _instance->applyWeatherLightPattern();
        _instance->lastPatternApply = millis();
      }
    }
#endif

    // Returns severity 1 (mild) to 3 (severe) for a WeatherAPI condition code
    static uint8_t conditionSeverity(int code) {
      // Clear
      if (code == 1000) return 1;
      // Cloudy: partly=1, cloudy=2, overcast=3
      if (code == 1003) return 1;
      if (code == 1006) return 2;
      if (code == 1009) return 3;
      // Fog: mist=1, fog=2, freezing fog=3
      if (code == 1030) return 1;
      if (code == 1135) return 2;
      if (code == 1147) return 3;
      // Thunder: possible=1, light with thunder=2, heavy with thunder=3
      if (code == 1087) return 1;
      if (code == 1273 || code == 1279) return 2;
      if (code == 1276 || code == 1282) return 3;
      // Snow/Ice: light=1, moderate=2, heavy/blizzard=3
      if (code == 1210 || code == 1213 || code == 1255) return 1;
      if (code == 1114 || code == 1216 || code == 1219 || code == 1237 ||
          code == 1258 || code == 1261) return 2;
      if (code == 1117 || code == 1222 || code == 1225 || code == 1264) return 3;
      // Rain: light/patchy=1, moderate=2, heavy/torrential=3
      if (code == 1063 || code == 1069 || code == 1072 || code == 1150 ||
          code == 1153 || code == 1180 || code == 1183 || code == 1198 ||
          code == 1204 || code == 1240 || code == 1249) return 1;
      if (code == 1186 || code == 1189 || code == 1201 || code == 1207 ||
          code == 1243 || code == 1252) return 2;
      if (code == 1171 || code == 1192 || code == 1195 || code == 1246) return 3;
      return 1;  // default mild
    }

    // Map a WeatherAPI condition code to a condition category index (0-5)
    static uint8_t conditionToCategory(int code) {
      if (code == 1000) return 0;  // Clear/Sunny
      if (code <= 1009) return 1;  // Cloudy/Overcast
      if (code == 1030 || code == 1135 || code == 1147) return 2;  // Fog/Mist
      if (code == 1087 || code >= 1273) return 3;  // Thunder/Storm
      if ((code >= 1114 && code <= 1117) || (code >= 1210 && code <= 1264)) return 4;  // Snow/Ice
      return 5;  // Rain/Drizzle (default)
    }

    // Returns a human-readable description for a WeatherAPI condition code.
    // Covers all 49 documented codes; returns "Unknown" for unrecognised values.
    static const char* conditionDescription(int code) {
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

  public:
    void setup() override {
#ifdef USERMOD_WEATHER_API
      _instance = this;
      auto* wapi = (WeatherApiUsermod*)UsermodManager::lookup(USERMOD_ID_WEATHER_API);
      if (wapi) {
        wapi->subscribe("current.temp_c",         _onTempC);
        wapi->subscribe("current.condition.code", _onCondCode);
        DEBUG_PRINTLN(F("TM1637 Clock: subscribed to WeatherApi usermod"));
      } else {
        DEBUG_PRINTLN(F("TM1637 Clock: WeatherApi usermod not found"));
      }
#endif

      // Try to find TM1637 display usermod (replace USERMOD_ID_TM1637_DISPLAY with actual ID)
      tm1637Display = (TM1637DisplayUsermod*)UsermodManager::lookup(USERMOD_ID_TM1637_DISPLAY);
      if (!tm1637Display) {
        DEBUG_PRINTLN(F("TM1637 Clock: TM1637 display usermod not found"));
      }

      // Try to find Baseball API usermod (replace USERMOD_ID_BASEBALL_API with actual ID)
      baseballApi = (UsermodBaseballAPI*)UsermodManager::lookup(USERMOD_ID_BASEBALL_API);
      if (!baseballApi) {
        DEBUG_PRINTLN(F("TM1637 Clock: Baseball API usermod not found"));
      }
    }

    void loop() override {
      static unsigned long lastWeatherCycleEnd = 0;
      static bool baseballShown = false;

      if (!enabled) {
        showStatusOnDisplay("OFF");
        return;
      }

      // Check WiFi
      if (WiFi.status() != WL_CONNECTED) {
        showStatusOnDisplay("WIFI");
        return;
      }

      // Check time (assume time is valid if year > 2020)
      time_t nowT = time(nullptr);
      struct tm* tmNow = localtime(&nowT);
      if (!tmNow || tmNow->tm_year + 1900 < 2021) {
        showStatusOnDisplay("TIME");
        return;
      }

      // Check weather
      if (!weatherFetched) {
        showStatusOnDisplay("WEAT");
        return;
      }

      // All good, clear status (optionally show temp or nothing)
      showStatusOnDisplay("");

      unsigned long nowMs = millis();
      // Re-apply weather-driven LED patterns periodically (every 60s)
      if (weatherOverrideType > 0 && weatherFetched &&
          (nowMs - lastPatternApply > TM1637_PATTERN_APPLY_INTERVAL_MS)) {
        applyWeatherLightPattern();
        lastPatternApply = nowMs;
      }

      // After weather display cycle, show baseball score for 3 seconds if live
      // Assume weather display cycle is 6 seconds (2x3s: temp + condition)
      if (tm1637Display && baseballApi && baseballApi->isGameLive() && baseballApi->getLastScore().length() > 0) {
        if (!baseballShown && (nowMs - lastWeatherCycleEnd > 6000)) {
          // Parse and display score
          String fav, opp;
          int favScore = 0, oppScore = 0;
          if (parseBaseballScore(baseballApi->getLastScore(), baseballApi->getFavoriteTeam(), fav, favScore, opp, oppScore)) {
            showBaseballScore(fav, favScore, opp, oppScore);
            baseballShown = true;
            lastWeatherCycleEnd = nowMs;
          }
        } else if (baseballShown && (nowMs - lastWeatherCycleEnd > 9000)) {
          // 3 seconds passed, reset
          baseballShown = false;
          lastWeatherCycleEnd = nowMs;
        }
      } else {
        baseballShown = false;
        lastWeatherCycleEnd = nowMs;
      }
    }



  public:
    uint16_t getId() override {
      return USERMOD_ID_TM1637_CLOCK;
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      
      JsonArray tm1637 = user.createNestedArray(FPSTR(_name));
      
      if (enabled) {
        if (weatherFetched) {
          char tBuf[8]; dtostrf(weatherTempCached, 4, 1, tBuf);
          char wBuf[48];
          snprintf(wBuf, sizeof(wBuf), "LED: %s C  %s", tBuf, conditionDescription(weatherCondCode));
          tm1637.add(wBuf);
        } else if (WiFi.status() == WL_CONNECTED) {
          tm1637.add("Weather: fetching...");
        } else {
          tm1637.add("Weather: no WiFi");
        }
      } else {
        tm1637.add("Disabled");
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]        = enabled;
      top[FPSTR(_overrideType)]   = weatherOverrideType;
      top[FPSTR(_lightMode)]      = weatherPatternMode;
      top[FPSTR(_paletteMode)]    = weatherPaletteMode;
      top[FPSTR(_presetClear)]    = conditionPresetOverrides[0];
      top[FPSTR(_presetCloudy)]   = conditionPresetOverrides[1];
      top[FPSTR(_presetFog)]      = conditionPresetOverrides[2];
      top[FPSTR(_presetThunder)]  = conditionPresetOverrides[3];
      top[FPSTR(_presetSnow)]     = conditionPresetOverrides[4];
      top[FPSTR(_presetRain)]     = conditionPresetOverrides[5];
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;

      bool configComplete = true;
      configComplete &= getJsonValue(top[FPSTR(_enabled)],        enabled);
      configComplete &= getJsonValue(top[FPSTR(_overrideType)],    weatherOverrideType,    (uint8_t)0);
      configComplete &= getJsonValue(top[FPSTR(_lightMode)],       weatherPatternMode,     (uint8_t)6);
      configComplete &= getJsonValue(top[FPSTR(_paletteMode)],     weatherPaletteMode,     (uint8_t)6);
      getJsonValue(top[FPSTR(_presetClear)],   conditionPresetOverrides[0], (uint8_t)0);
      getJsonValue(top[FPSTR(_presetCloudy)],  conditionPresetOverrides[1], (uint8_t)0);
      getJsonValue(top[FPSTR(_presetFog)],     conditionPresetOverrides[2], (uint8_t)0);
      getJsonValue(top[FPSTR(_presetThunder)], conditionPresetOverrides[3], (uint8_t)0);
      getJsonValue(top[FPSTR(_presetSnow)],    conditionPresetOverrides[4], (uint8_t)0);
      getJsonValue(top[FPSTR(_presetRain)],    conditionPresetOverrides[5], (uint8_t)0);

      DEBUG_PRINTLN(F("TM1637 Clock: readFromConfig"));
      DEBUG_PRINTF("  enabled=%d  overrideType=%d  patternMode=%d  paletteMode=%d\n",
        enabled, weatherOverrideType, weatherPatternMode, weatherPaletteMode);
      DEBUG_PRINTF("  presets: clear=%d cloudy=%d fog=%d thunder=%d snow=%d rain=%d\n",
        conditionPresetOverrides[0], conditionPresetOverrides[1], conditionPresetOverrides[2],
        conditionPresetOverrides[3], conditionPresetOverrides[4], conditionPresetOverrides[5]);

      // Re-apply light/palette override immediately so the new settings take
      // effect as soon as the user saves, without waiting for the next weather
      // fetch or the 60-second re-apply tick.
      if (weatherOverrideType > 0 && weatherFetched) {
        applyWeatherLightPattern();
        lastPatternApply = millis();
      }

      return configComplete;
    }

    void appendConfigData() override {
      // Weather override hint
      oappend(SET_F("addInfo('TM1637Clock:enabled',1,'Enable weather-driven LED control');"));
      // Override type: hidden dropdown as data store, replaced with radio buttons via JS
      oappend(SET_F("dd=addDropdown('TM1637Clock','override-type');"));
      oappend(SET_F("addOption(dd,'No override',0);"));
      oappend(SET_F("addOption(dd,'Light pattern',1);"));
      oappend(SET_F("addOption(dd,'Color palette',2);"));
      // Light pattern mode dropdown (shown when override-type == 1)
      oappend(SET_F("dd=addDropdown('TM1637Clock','light-mode');"));
      oappend(SET_F("addOption(dd,'Condition only',0);"));
      oappend(SET_F("addOption(dd,'Temperature only',1);"));
      oappend(SET_F("addOption(dd,'Time of day only',2);"));
      oappend(SET_F("addOption(dd,'Condition + Temperature',3);"));
      oappend(SET_F("addOption(dd,'Condition + Time of day',4);"));
      oappend(SET_F("addOption(dd,'Temperature + Time of day',5);"));
      oappend(SET_F("addOption(dd,'All (Condition + Temp + Time)',6);"));
      // Condition preset overrides (shown when override-type == 1)
      oappend(SET_F("addInfo('TM1637Clock:preset-clear',1,'WLED preset # for Clear/Sunny (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-cloudy',1,'WLED preset # for Cloudy/Overcast (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-fog',1,'WLED preset # for Fog/Mist (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-thunder',1,'WLED preset # for Thunder/Storm (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-snow',1,'WLED preset # for Snow/Ice (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-rain',1,'WLED preset # for Rain/Drizzle (0=off)');"));
      // Color palette mode dropdown (shown when override-type == 2)
      oappend(SET_F("dd=addDropdown('TM1637Clock','palette-mode');"));
      oappend(SET_F("addOption(dd,'Condition only',0);"));
      oappend(SET_F("addOption(dd,'Temperature only',1);"));
      oappend(SET_F("addOption(dd,'Time of day only',2);"));
      oappend(SET_F("addOption(dd,'Condition + Temperature',3);"));
      oappend(SET_F("addOption(dd,'Condition + Time of day',4);"));
      oappend(SET_F("addOption(dd,'Temperature + Time of day',5);"));
      oappend(SET_F("addOption(dd,'All (Condition + Temp + Time)',6);"));
      // Replace override-type dropdown with radio buttons; wire show/hide for dependent fields
      oappend(SET_F("(function(){"));
      oappend(SET_F("var sel=document.getElementById('TM1637Clock_override-type');if(!sel)return;"));
      oappend(SET_F("var par=sel.parentNode;var div=document.createElement('div');div.style.margin='4px 0';"));
      oappend(SET_F("[['No override','0'],['Light pattern','1'],['Color palette','2']].forEach(function(o){"));
      oappend(SET_F("var l=document.createElement('label');l.style.cssText='margin-right:14px;cursor:pointer;';"));
      oappend(SET_F("var r=document.createElement('input');r.type='radio';r.name='TM1637Clock_ov_r';r.value=o[1];"));
      oappend(SET_F("if(sel.value===o[1])r.checked=true;"));
      oappend(SET_F("r.addEventListener('change',function(){sel.value=this.value;upd();});"));
      oappend(SET_F("l.appendChild(r);l.appendChild(document.createTextNode(' '+o[0]));div.appendChild(l);});"));
      oappend(SET_F("par.insertBefore(div,sel.nextSibling);sel.style.display='none';"));
      oappend(SET_F("var lmIds=['TM1637Clock_light-mode','TM1637Clock_preset-clear','TM1637Clock_preset-cloudy',"));
      oappend(SET_F("'TM1637Clock_preset-fog','TM1637Clock_preset-thunder','TM1637Clock_preset-snow','TM1637Clock_preset-rain'];"));
      oappend(SET_F("function upd(){var v=sel.value;"));
      oappend(SET_F("lmIds.forEach(function(i){var e=document.getElementById(i);if(e&&e.parentNode)e.parentNode.style.display=(v==='1')?'':'none';});"));
      oappend(SET_F("var pm=document.getElementById('TM1637Clock_palette-mode');if(pm&&pm.parentNode)pm.parentNode.style.display=(v==='2')?'':'none';}"));
      oappend(SET_F("upd();})();"));
     
    }

    void applyWeatherLightPattern() {
      int h = hour(localTime);

      if (weatherOverrideType == 1) {
        // Condition preset override takes priority over weather pattern mode
        uint8_t cat = conditionToCategory(weatherCondCode);
        DEBUG_PRINTF("TM1637 Clock: applyWeatherLightPattern condCode=%d cat=%d preset=%d patternMode=%d\n",
          weatherCondCode, cat, conditionPresetOverrides[cat], weatherPatternMode);
        if (conditionPresetOverrides[cat] > 0) {
          DEBUG_PRINTF("TM1637 Clock: applying preset %d for category %d\n", conditionPresetOverrides[cat], cat);
          applyPreset(conditionPresetOverrides[cat]);
          // preset handles its own state update; still apply palette below if requested
        } else {
          { char tBuf[8]; dtostrf(weatherTempCached, 4, 1, tBuf);
            DEBUG_PRINTF("TM1637 Clock: applying weather pattern mode %d  hour=%d  temp=%s\n", weatherPatternMode, h, tBuf); }
          switch (weatherPatternMode) {
            case 0: setLightsByCondition(weatherCondCode);                              break;
            case 1: setLightsByTemperature(weatherTempCached);                         break;
            case 2: setLightsByTime(h);                                                break;
            case 3: setLightsByConditionAndTemperature(weatherCondCode, weatherTempCached); break;
            case 4: setLightsByConditionAndTime(weatherCondCode, h);                   break;
            case 5: setLightsByTemperatureAndTime(weatherTempCached, h);               break;
            default: setLightsByAll(weatherCondCode, weatherTempCached, h);            break;
          }
        }
      }
      // Apply weather-driven palette if enabled
      if (weatherOverrideType == 2) {
        uint8_t palIdx;
        switch (weatherPaletteMode) {
          case 0: palIdx = getPaletteByCondition(weatherCondCode);                          break;
          case 1: palIdx = getPaletteByTemperature(weatherTempCached);                     break;
          case 2: palIdx = getPaletteByTime(h);                                            break;
          case 3: palIdx = getPaletteByConditionAndTemperature(weatherCondCode, weatherTempCached); break;
          case 4: palIdx = getPaletteByConditionAndTime(weatherCondCode, h);               break;
          case 5: palIdx = getPaletteByTemperatureAndTime(weatherTempCached, h);           break;
          default: palIdx = getPaletteByAll(weatherCondCode, weatherTempCached, h);        break;
        }
        strip.getMainSegment().setPalette(palIdx);
        DEBUG_PRINTF("TM1637 Clock: applied palette %d (mode %d)\n", palIdx, weatherPaletteMode);
      }

      stateChanged = true;
      strip.trigger();
    }

    void enable(bool en) { enabled = en; }
    bool isEnabled() { return enabled; }
};

// Static member definitions
TM1637ClockUsermod* TM1637ClockUsermod::_instance = nullptr;
const char TM1637ClockUsermod::_name[]       PROGMEM = "TM1637Clock";
const char TM1637ClockUsermod::_enabled[]    PROGMEM = "enabled";
const char TM1637ClockUsermod::_overrideType[]    PROGMEM = "override-type";
const char TM1637ClockUsermod::_lightMode[]       PROGMEM = "light-mode";
const char TM1637ClockUsermod::_paletteMode[]     PROGMEM = "palette-mode";
const char TM1637ClockUsermod::_presetClear[]   PROGMEM = "preset-clear";
const char TM1637ClockUsermod::_presetCloudy[]  PROGMEM = "preset-cloudy";
const char TM1637ClockUsermod::_presetFog[]     PROGMEM = "preset-fog";
const char TM1637ClockUsermod::_presetThunder[] PROGMEM = "preset-thunder";
const char TM1637ClockUsermod::_presetSnow[]    PROGMEM = "preset-snow";
const char TM1637ClockUsermod::_presetRain[]    PROGMEM = "preset-rain";

// Create usermod instance and register it
static TM1637ClockUsermod tm1637Clock;
REGISTER_USERMOD(tm1637Clock);

#endif // USERMOD_TM1637_CLOCK
