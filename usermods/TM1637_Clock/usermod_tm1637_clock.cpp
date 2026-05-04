
#ifdef USERMOD_TM1637_CLOCK
#include "wled.h"
#include "const.h"
#include "../weather_api/weather_light_patterns.h"
#include "../tm1637_Display/usermod_tm1637_display.h"
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

    // Pointer to Baseball API usermod (set in setup if available)
    UsermodBaseballAPI* baseballApi = nullptr;

    // Show a baseball score on the TM1637 display (favorite left, opponent right)
    void showBaseballScore(const String& fav, int favScore, const String& opp, int oppScore) {
      // Format: D-D (single digit per side, fits 4-char TM1637)
      char buf[5];
      uint8_t favDigit = (uint8_t)(((favScore < 0) ? 0 : favScore) % 10);
      uint8_t oppDigit = (uint8_t)(((oppScore < 0) ? 0 : oppScore) % 10);
      snprintf(buf, sizeof(buf), "%u-%u", favDigit, oppDigit);
      DEBUG_PRINTF("TM1637 Clock: MLB showBaseballScore fav=%s(%d) opp=%s(%d) text=%s\n",
        fav.c_str(), favScore, opp.c_str(), oppScore, buf);
      tm1637DisplayShowMessage(buf, 3000);
    }

    // Helper to parse lastScore string and extract team names and scores.
    // Supports both:
    //  1) "Yankees vs Red Sox: 3-2 (LIVE)"
    //  2) "Yankees 3 @ Red Sox 2"
    // If favorite team does not match either side (e.g. stored as numeric style ID),
    // falls back to displaying away-vs-home scores.
    bool parseBaseballScore(const String& lastScore, const String& favTeam, String& fav, int& favScore, String& opp, int& oppScore) {
      // Legacy format: "Home vs Away: H-A (STATE)"
      int vsIdx = lastScore.indexOf(" vs ");
      int colonIdx = lastScore.indexOf(": ");
      int dashIdx = lastScore.indexOf("-");
      if (vsIdx >= 0 && colonIdx >= 0 && dashIdx >= 0) {
        String home = lastScore.substring(0, vsIdx);
        String away = lastScore.substring(vsIdx + 4, colonIdx);
        int scoreEnd = lastScore.indexOf(" ", colonIdx + 2);
        if (scoreEnd < 0) scoreEnd = lastScore.length();
        String scorePart = lastScore.substring(colonIdx + 2, scoreEnd);
        int dash = scorePart.indexOf("-");
        if (dash >= 0) {
          int homeScore = scorePart.substring(0, dash).toInt();
          int awayScore = scorePart.substring(dash + 1).toInt();
          if (home.equalsIgnoreCase(favTeam)) {
            fav = home; favScore = homeScore; opp = away; oppScore = awayScore;
            return true;
          }
          if (away.equalsIgnoreCase(favTeam)) {
            fav = away; favScore = awayScore; opp = home; oppScore = homeScore;
            return true;
          }
          // Fallback when favorite team does not match by name
          fav = away; favScore = awayScore; opp = home; oppScore = homeScore;
          return true;
        }
      }

      // New format: "Away A @ Home H"
      int atIdx = lastScore.indexOf(" @ ");
      if (atIdx < 0) return false;

      String awayPart = lastScore.substring(0, atIdx);
      String homePart = lastScore.substring(atIdx + 3);
      int awaySplit = awayPart.lastIndexOf(' ');
      int homeSplit = homePart.lastIndexOf(' ');
      if (awaySplit <= 0 || homeSplit <= 0) return false;

      String away = awayPart.substring(0, awaySplit);
      int awayScore = awayPart.substring(awaySplit + 1).toInt();
      String home = homePart.substring(0, homeSplit);
      int homeScore = homePart.substring(homeSplit + 1).toInt();

      if (home.equalsIgnoreCase(favTeam)) {
        fav = home; favScore = homeScore; opp = away; oppScore = awayScore;
        return true;
      }
      if (away.equalsIgnoreCase(favTeam)) {
        fav = away; favScore = awayScore; opp = home; oppScore = homeScore;
        return true;
      }

      // Fallback when favorite team is an ID instead of a full name.
      fav = away; favScore = awayScore; opp = home; oppScore = homeScore;
      return true;
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
  #ifdef USERMOD_BASEBALL_API
          if (_instance->baseballApi && _instance->baseballApi->isGameOverrideActive()) return;
  #endif
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

      // Check for TM1637 display usermod availability.
      if (!UsermodManager::lookup(USERMOD_ID_TM1637_DISPLAY)) {
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
      static unsigned long lastGateLogMs = 0;
      unsigned long nowMs = millis();

      if (!enabled) {
        if (nowMs - lastGateLogMs > 5000) {
          DEBUG_PRINTLN(F("TM1637 Clock: MLB gate blocked (TM1637Clock disabled)"));
          lastGateLogMs = nowMs;
        }
        return;
      }

      // Check WiFi
      if (WiFi.status() != WL_CONNECTED) {
        if (nowMs - lastGateLogMs > 5000) {
          DEBUG_PRINTF("TM1637 Clock: MLB gate blocked (WiFi status=%d)\n", WiFi.status());
          lastGateLogMs = nowMs;
        }
        return;
      }

      // Check time using WLED's internal synced clock (localTime/toki).
      // Using libc time(nullptr) can report 1970 even when WLED time is valid.
      updateLocalTime();
      bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL); // 2024-01-01 .. 2100-01-01
      if (!timeValid) {
        if (nowMs - lastGateLogMs > 5000) {
          DEBUG_PRINTF("TM1637 Clock: MLB gate blocked (WLED time not valid, localTime=%lu toki=%lu)\n",
            (unsigned long)localTime, (unsigned long)toki.second());
          lastGateLogMs = nowMs;
        }
      }

      // Check weather — TM1637_Display already shows time; just skip LED/score logic until ready
      // Re-apply weather-driven LED patterns periodically (every 60s)
      if (timeValid && weatherFetched && weatherOverrideType > 0 &&
          (nowMs - lastPatternApply > TM1637_PATTERN_APPLY_INTERVAL_MS)) {
      #ifdef USERMOD_BASEBALL_API
        if (!baseballApi || !baseballApi->isGameOverrideActive()) {
      #endif
        applyWeatherLightPattern();
        lastPatternApply = nowMs;
      #ifdef USERMOD_BASEBALL_API
        }
      #endif
      }

      if(baseballApi){
        if( baseballApi->isGameLive()){
          if (baseballApi->getLastScore().length() > 0) {
            if (!baseballShown && (nowMs - lastWeatherCycleEnd > 6000)) {
              // Parse and display score
              String fav, opp;
              int favScore = 0, oppScore = 0;
              if (parseBaseballScore(baseballApi->getLastScore(), baseballApi->getFavoriteTeam(), fav, favScore, opp, oppScore)) {
                showBaseballScore(fav, favScore, opp, oppScore);
                DEBUG_PRINTF("TM1637 Clock: MLB displayed live score %s %d vs %s %d (raw='%s')\n",
                  fav.c_str(), favScore, opp.c_str(), oppScore, baseballApi->getLastScore().c_str());
                baseballShown = true;
                lastWeatherCycleEnd = nowMs;
              } else {
                // Parse failed: show a visible failure marker on the 4-digit display.
                // "----" maps cleanly to TM1637 segments and is unambiguous.
                DEBUG_PRINTF("TM1637 Clock: MLB parseBaseballScore failed, raw='%s' fav='%s'\n",
                  baseballApi->getLastScore().c_str(), baseballApi->getFavoriteTeam().c_str());
                tm1637DisplayShowMessage("----", 3000);
                DEBUG_PRINTLN(F("TM1637 Clock: MLB displayed parse-failure marker ----"));
                baseballShown = true;
                lastWeatherCycleEnd = nowMs;
              }
            } else if (baseballShown && (nowMs - lastWeatherCycleEnd > 9000)) {
              // 3 seconds passed, reset
              baseballShown = false;
              lastWeatherCycleEnd = nowMs;
              DEBUG_PRINTLN(F("TM1637 Clock: MLB reset baseball score display after 3s"));
            }
          } else {
            baseballShown = false;
            lastWeatherCycleEnd = nowMs;
            DEBUG_PRINTLN(F("TM1637 Clock: MLB live game but no score available yet, skipping display") );
          }
        }else{
          
         // DEBUG_PRINTLN(F("TM1637 Clock: MLB no live game, skipping baseball score display") );
        }
        // Show baseball score on TM1637 when a game is live (independent of weather)
        
      }else{
        if (nowMs - lastGateLogMs > 5000) {
          DEBUG_PRINTLN(F("TM1637 Clock: MLB gate blocked (Baseball API usermod lookup returned null)"));
          lastGateLogMs = nowMs;
        }
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
      // Keep settings UI script minimal to improve reliability on constrained devices.
      oappend(SET_F("var dd=addDropdown('TM1637Clock','override-type');"));
      oappend(SET_F("addOption(dd,'No override',0);"));
      oappend(SET_F("addOption(dd,'Light pattern',1);"));
      oappend(SET_F("addOption(dd,'Color palette',2);"));
      oappend(SET_F("dd=addDropdown('TM1637Clock','light-mode');"));
      oappend(SET_F("addOption(dd,'Condition only',0);"));
      oappend(SET_F("addOption(dd,'Temperature only',1);"));
      oappend(SET_F("addOption(dd,'Time of day only',2);"));
      oappend(SET_F("addOption(dd,'Condition + Temperature',3);"));
      oappend(SET_F("addOption(dd,'Condition + Time of day',4);"));
      oappend(SET_F("addOption(dd,'Temperature + Time of day',5);"));
      oappend(SET_F("addOption(dd,'All (Condition + Temp + Time)',6);"));
      oappend(SET_F("addInfo('TM1637Clock:preset-clear',1,'WLED preset # for Clear/Sunny (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-cloudy',1,'WLED preset # for Cloudy/Overcast (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-fog',1,'WLED preset # for Fog/Mist (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-thunder',1,'WLED preset # for Thunder/Storm (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-snow',1,'WLED preset # for Snow/Ice (0=off)');"));
      oappend(SET_F("addInfo('TM1637Clock:preset-rain',1,'WLED preset # for Rain/Drizzle (0=off)');"));
      oappend(SET_F("dd=addDropdown('TM1637Clock','palette-mode');"));
      oappend(SET_F("addOption(dd,'Condition only',0);"));
      oappend(SET_F("addOption(dd,'Temperature only',1);"));
      oappend(SET_F("addOption(dd,'Time of day only',2);"));
      oappend(SET_F("addOption(dd,'Condition + Temperature',3);"));
      oappend(SET_F("addOption(dd,'Condition + Time of day',4);"));
      oappend(SET_F("addOption(dd,'Temperature + Time of day',5);"));
      oappend(SET_F("addOption(dd,'All (Condition + Temp + Time)',6);"));
     
    }

    void applyWeatherLightPattern() {
      int h = hour(localTime);

      #ifdef USERMOD_BASEBALL_API
        // Baseball game override takes priority — do not apply weather patterns while active
        if (baseballApi && baseballApi->isGameOverrideActive()) return;
      #endif

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
