
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

#ifdef USERMOD_BASEBALL_API
#include "../baseball_api/usermod_baseball_api.h"
#endif

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
    #ifdef USERMOD_BASEBALL_API
    UsermodBaseballAPI* baseballApi = nullptr;
    #endif

    // Build a 4-char score text (D-D) from parsed scores.
    // Returns the text in outBuf[5].
    static void _buildScoreText(int favScore, int oppScore, char outBuf[5]) {
      int fav = (favScore < 0) ? 0 : favScore;
      int opp = (oppScore < 0) ? 0 : oppScore;

      // TM1637 has exactly 4 character positions.
      // Prefer full values when they fit:
      //  - d-d, dd-d, d-dd fit exactly.
      //  - dd-dd (or wider) does not fit, so show last digits.
      if (fav < 10 && opp < 10) {
        outBuf[0] = '0' + fav;
        outBuf[1] = '-';
        outBuf[2] = '0' + opp;
        outBuf[3] = '\0';
        return;
      }

      if (fav < 100 && opp < 10) {
        outBuf[0] = '0' + (fav / 10);
        outBuf[1] = '0' + (fav % 10);
        outBuf[2] = '-';
        outBuf[3] = '0' + opp;
        outBuf[4] = '\0';
        return;
      }

      if (fav < 10 && opp < 100) {
        outBuf[0] = '0' + fav;
        outBuf[1] = '-';
        outBuf[2] = '0' + (opp / 10);
        outBuf[3] = '0' + (opp % 10);
        outBuf[4] = '\0';
        return;
      }

      outBuf[0] = '0' + (fav % 10);
      outBuf[1] = '-';
      outBuf[2] = '0' + (opp % 10);
      outBuf[3] = '\0';
    }

    // Slot provider callback — called by TM1637Display once per cycle rebuild.
    // Pushes a SLOT_BASEBALL entry when a live game score is available.
    // Uses SLOT_BASEBALL type so the display can render it distinctly if needed.
    static void _baseballSlotProvider(SlotQueue& queue) {
#ifdef USERMOD_BASEBALL_API
      if (!_instance || !_instance->baseballApi) return;
      if (!_instance->baseballApi->isGameLive()) return;

      const String& lastScore = _instance->baseballApi->getLastScore();
      if (lastScore.length() == 0) return;
      String favTeamName = _instance->baseballApi->getFavoriteTeamResolvedName();

      DEBUG_PRINTF("TM1637 Clock: MLB slot provider - raw lastScore='%s'\n", lastScore.c_str());
      DEBUG_PRINTF("TM1637 Clock: MLB slot provider - resolved favorite='%s'\n", favTeamName.c_str());

      String fav, opp;
      int favScore = 0, oppScore = 0;
      char buf[5];
      // Parse directly in favorite-vs-opponent order using resolved team name.
      if (_instance->parseBaseballScore(lastScore, favTeamName, fav, favScore, opp, oppScore)) {
          DEBUG_PRINTF("TM1637 Clock: MLB slot provider - parsed fav='%s' score=%d, opp='%s' score=%d\n",
                 fav.c_str(), favScore, opp.c_str(), oppScore);
        bool isFavHome = _instance->baseballApi->isFavoriteHomeTeam();
        DEBUG_PRINTF("TM1637 Clock: MLB slot provider - isFavoriteHomeTeam()=%d\n", (int)isFavHome);
        _buildScoreText(favScore, oppScore, buf);
        DEBUG_PRINTF("TM1637 Clock: MLB slot provider pushing score '%s' (homeGame=%d)\n",
                     buf, isFavHome);
      } else {
        // Parse failure — show unambiguous error marker
        strncpy(buf, "----", sizeof(buf));
        DEBUG_PRINTF("TM1637 Clock: MLB slot provider push parse-failure '----' for lastScore='%s'\n", lastScore.c_str());
      }
      uint16_t dur = tm1637DisplayGetSlotDurationMs();
      queue.push(SLOT_BASEBALL, dur, buf);
#endif
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
      // Strip trailing " (inning info)" or " (Final)" from homePart before
      // parsing the score. Without this, lastIndexOf(' ') lands on the space
      // before "out" / "outs" / "Final", making homeScore always 0.
      int parenIdx = homePart.indexOf(" (");
      String homePartClean = (parenIdx >= 0) ? homePart.substring(0, parenIdx) : homePart;
      homeSplit = homePartClean.lastIndexOf(' ');
      if (homeSplit <= 0) return false;
      String home = homePartClean.substring(0, homeSplit);
      int homeScore = homePartClean.substring(homeSplit + 1).toInt();

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
          if (_instance->baseballApi && _instance->baseballApi->isGameLive()) return;
  #endif
        _instance->applyWeatherLightPattern();
        _instance->lastPatternApply = millis();
      }
    }
#endif

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

      // Register baseball slot provider with the display queue.
      // The provider is called once per cycle so it can inject a live score
      // slot without needing any timing logic here.
      if (!tm1637DisplayAddSlotProvider(_baseballSlotProvider)) {
        DEBUG_PRINTLN(F("TM1637 Clock: TM1637 display usermod not found or provider table full"));
      } else {
        DEBUG_PRINTLN(F("TM1637 Clock: registered baseball slot provider"));
      }

      #ifdef USERMOD_BASEBALL_API
      baseballApi = (UsermodBaseballAPI*)UsermodManager::lookup(USERMOD_ID_BASEBALL_API);
      if (!baseballApi) {
        DEBUG_PRINTLN(F("TM1637 Clock: Baseball API usermod not found"));
      }
      #endif
    }

    void loop() override {
      unsigned long nowMs = millis();

      if (!enabled) return;
      if (WiFi.status() != WL_CONNECTED) return;

      // Check time using WLED's internal synced clock (localTime/toki).
      // Using libc time(nullptr) can report 1970 even when WLED time is valid.
      updateLocalTime();
      bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);

      // Re-apply weather-driven LED patterns periodically.
      // Baseball score display is now handled entirely by the slot provider
      // registered in setup(), so no timing state is needed here.
      if (timeValid && weatherFetched && weatherOverrideType > 0 &&
          (nowMs - lastPatternApply > TM1637_PATTERN_APPLY_INTERVAL_MS)) {
        // Always reset the timer so we don't fire every loop tick while
        // baseball is active. The gate below will skip the actual apply.
        lastPatternApply = nowMs;
      #ifdef USERMOD_BASEBALL_API
        if (!baseballApi || !baseballApi->isGameLive()) {
      #endif
        applyWeatherLightPattern();
      #ifdef USERMOD_BASEBALL_API
        }
      #endif
      }
    }



  public:
    uint16_t getId() override {
      return USERMOD_ID_TM1637_CLOCK;
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));

      if (!enabled)                          { arr.add(F("Disabled")); return; }
      if (WiFi.status() != WL_CONNECTED)     { arr.add(F("No WiFi"));  return; }
      if (!weatherFetched)                   { arr.add(F("Weather: fetching...")); return; }

      // Keep /json/info compact. The generic info page renders usermod info as a
      // single summary row, so detailed diagnostics belong in /json/state.
      char tBuf[8]; dtostrf(weatherTempCached, 4, 1, tBuf);
      char buf[96];
      const char* modeSummary = "Off";
      if (weatherOverrideType == 1) {
        uint8_t cat    = conditionToCategory(weatherCondCode);
        uint8_t preset = conditionPresetOverrides[cat];
        static const char* const _modeNames[] = {
          "Condition", "Temperature", "Time of day",
          "Cond+Temp", "Cond+Time",  "Temp+Time", "All"
        };
        if (preset > 0) {
          snprintf(buf, sizeof(buf), "%s C %s | Preset #%u", tBuf, conditionDescription(weatherCondCode), preset);
        } else {
          modeSummary = weatherPatternMode < 7 ? _modeNames[weatherPatternMode] : "All";
          snprintf(buf, sizeof(buf), "%s C %s | Pattern %s", tBuf, conditionDescription(weatherCondCode), modeSummary);
        }
      } else if (weatherOverrideType == 2) {
        static const char* const _palModeNames[] = {
          "Condition", "Temperature", "Time of day",
          "Cond+Temp", "Cond+Time",  "Temp+Time", "All"
        };
        modeSummary = weatherPaletteMode < 7 ? _palModeNames[weatherPaletteMode] : "All";
        snprintf(buf, sizeof(buf), "%s C %s | Palette %s", tBuf, conditionDescription(weatherCondCode), modeSummary);
      } else {
        snprintf(buf, sizeof(buf), "%s C %s | No override", tBuf, conditionDescription(weatherCondCode));
      }
      arr.add(buf);
    }

    // Expose current weather state in GET /json/state response.
    // Note: `enabled` is intentionally excluded — it is a config-only value.
    // Including it here would allow saved WLED presets to capture and later
    // restore it, silently disabling the usermod when an old preset is applied.
    void addToJsonState(JsonObject& root) override {
      JsonObject top = root.createNestedObject("TM1637Clock");
      top["fetched"]      = weatherFetched;
      top["condCode"]     = weatherCondCode;
      top["condDesc"]     = conditionDescription(weatherCondCode);
      top["tempC"]        = weatherTempCached;
      top["overrideType"] = weatherOverrideType;
      top["patternMode"]  = weatherPatternMode;
      top["paletteMode"]  = weatherPaletteMode;
      uint8_t cat         = conditionToCategory(weatherCondCode);
      top["condCategory"] = cat;
      top["condPreset"]   = conditionPresetOverrides[cat];
    }

    // Accept test commands via POST /json/state.
    // Example: {"TM1637Clock":{"testCode":1276,"testTemp":5.0,"overrideType":1,"patternMode":6,"reapply":true,"forceApply":true}}
    //   testCode      — inject a WeatherAPI condition code (without waiting for a real fetch)
    //   testTemp      — inject a temperature in °C
    //   overrideType  — 0=none, 1=light pattern, 2=color palette
    //   patternMode   — 0..6 for weather-driven light patterns
    //   paletteMode   — 0..6 for weather-driven palettes
    //   preset-*      — per-category preset overrides (same names as config keys)
    //   reapply       — immediately re-apply the current (or just-injected) weather pattern
    //   forceApply    — bypass baseball override once for explicit API testing
    // Note: `enabled` is NOT accepted here — use the settings page (config) to enable/disable.
    void readFromJsonState(JsonObject& root) override {
      JsonObject top = root["TM1637Clock"];
      if (top.isNull()) return;

      // Note: `enabled` is not handled here — it is config-only and must not
      // be overwritten by preset state (see addToJsonState for rationale).
      if (!top["overrideType"].isNull()) {
        uint8_t value = top["overrideType"].as<uint8_t>();
        weatherOverrideType = (value <= 2) ? value : weatherOverrideType;
      }
      if (!top["patternMode"].isNull()) {
        uint8_t value = top["patternMode"].as<uint8_t>();
        weatherPatternMode = (value <= 6) ? value : weatherPatternMode;
      }
      if (!top["paletteMode"].isNull()) {
        uint8_t value = top["paletteMode"].as<uint8_t>();
        weatherPaletteMode = (value <= 6) ? value : weatherPaletteMode;
      }
      if (!top["preset-clear"].isNull())   conditionPresetOverrides[0] = top["preset-clear"].as<uint8_t>();
      if (!top["preset-cloudy"].isNull())  conditionPresetOverrides[1] = top["preset-cloudy"].as<uint8_t>();
      if (!top["preset-fog"].isNull())     conditionPresetOverrides[2] = top["preset-fog"].as<uint8_t>();
      if (!top["preset-thunder"].isNull()) conditionPresetOverrides[3] = top["preset-thunder"].as<uint8_t>();
      if (!top["preset-snow"].isNull())    conditionPresetOverrides[4] = top["preset-snow"].as<uint8_t>();
      if (!top["preset-rain"].isNull())    conditionPresetOverrides[5] = top["preset-rain"].as<uint8_t>();

      if (!top["testCode"].isNull()) {
        weatherCondCode = top["testCode"].as<int>();
        weatherFetched  = true;
        DEBUG_PRINTF("TM1637 Clock: testCode injected=%d\n", weatherCondCode);
      }
      if (!top["testTemp"].isNull()) {
        weatherTempCached = top["testTemp"].as<float>();
        weatherFetched    = true;
        DEBUG_PRINTF("TM1637 Clock: testTemp injected=%.1f\n", (double)weatherTempCached);
      }
      bool reapply = top["reapply"] | false;
      bool forceApply = top["forceApply"] | false;
      if (reapply && weatherFetched && weatherOverrideType > 0) {
        applyWeatherLightPattern(forceApply);
        lastPatternApply = millis();
        DEBUG_PRINTF("TM1637 Clock: forced reapply via JSON state (forceApply=%d)\n", forceApply);
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
      // s.js?p=8 is a single shared response for all usermods — keep output minimal.
      // Wrap in an IIFE to avoid leaking 'dd'/'mo' into the global JS scope.
      oappend(SET_F("(function(){"));
      oappend(SET_F("addInfo('TM1637Clock:enabled',1,'Weather-driven LED control');"));
      // Override-type: only 3 options — direct addOption calls are shorter than a forEach loop.
      oappend(SET_F("var dd=addDropdown('TM1637Clock','override-type');"));
      oappend(SET_F("addOption(dd,'No override',0);addOption(dd,'Light pattern',1);addOption(dd,'Color palette',2);"));
      // light-mode and palette-mode share the same 7 options; build once, apply to both.
      oappend(SET_F("var mo='Condition|0,Temperature|1,Time of day|2,Cond+Temp|3,Cond+Time|4,Temp+Time|5,All|6'.split(',');"));
      oappend(SET_F("['light-mode','palette-mode'].forEach(f=>{dd=addDropdown('TM1637Clock',f);mo.forEach(s=>{var p=s.indexOf('|');addOption(dd,s.slice(0,p),+s.slice(p+1));});});"));
      // Six preset hints — category label used directly as hint text.
      oappend(SET_F("'clear|Clear/Sunny,cloudy|Cloudy/Overcast,fog|Fog/Mist,thunder|Thunder/Storm,snow|Snow/Ice,rain|Rain/Drizzle'.split(',').forEach(s=>{var p=s.indexOf('|');addInfo('TM1637Clock:preset-'+s.slice(0,p),1,s.slice(p+1)+' (0=off)');});"));
      oappend(SET_F("})();"));
    }

    void applyWeatherLightPattern(bool ignoreBaseballOverride = false) {
      int h = hour(localTime);

      #ifdef USERMOD_BASEBALL_API
        // Baseball game override takes priority — do not apply weather patterns while a game is live
        if (!ignoreBaseballOverride && baseballApi && baseballApi->isGameLive()) return;
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
