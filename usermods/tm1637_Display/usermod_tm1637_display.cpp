#include "wled.h"
#ifdef USERMOD_TM1637_DISPLAY
#include <TM1637Display.h>
#include "usermod_tm1637_display.h"
#ifdef USERMOD_WEATHER_API
  #include "../weather_api/usermod_weather_api.h"
  #include "../weather_api/weather_light_patterns.h"
#endif

class TM1637DisplayUsermod;
static TM1637DisplayUsermod* g_tm1637DisplayInstance = nullptr;

/*
 * TM1637 Display Usermod
 * Drives a TM1637 4-digit 7-segment display showing a rotating cycle of slots:
 *   - Time  (HH:MM with blinking colon) — shown for timeDurationMs between info cycles
 *   - Temperature (°C)                  — shown for slotDurationMs
 *   - Weather condition abbreviation    — shown for slotDurationMs (e.g. "rAn2", "FOG1")
 *   - Baseball score (when live)        — shown for slotDurationMs, injected via slot provider
 *   - Status messages when WiFi / internet / NTP are unavailable
 *
 * Display cycle
 * -------------
 * The cycle is driven by a small fixed-size DisplaySlot queue.  When the queue
 * empties, _rebuildQueue() is called which:
 *   1. Always adds Temperature and Condition slots (when weather data is available).
 *   2. Calls every registered SlotProvider callback so external usermods (e.g.
 *      TM1637_Clock) can append additional slots (e.g. a live baseball score).
 *
 * Each slot carries its own duration so different content can be shown for
 * different lengths of time.  The time display fills the gap between cycles
 * using `timeDurationMs` (configurable).
 *
 * Adding a new data source
 * ------------------------
 * Call tm1637DisplayAddSlotProvider(myCallback) during setup.  The callback
 * receives a reference to the queue builder and may call
 * builder.push(SlotType, durationMs) to append slots.
 *
 * This module handles only the physical TM1637 display hardware.  WLED LED
 * light-pattern and palette control are intentionally omitted — see
 * TM1637_Clock for a combined display + LED-override usermod.
 *
 * Weather data is sourced from the WeatherApi usermod (USERMOD_WEATHER_API)
 * via its publish/subscribe mechanism when that mod is present in the build.
 *
 * Connections (NodeMCU v3):
 *   TM1637 CLK -> D5 (GPIO14)
 *   TM1637 DIO -> D6 (GPIO12)
 *   TM1637 VCC -> 3.3V
 *   TM1637 GND -> GND
 */

#ifndef USERMOD_TM1637_CLK_PIN
#define USERMOD_TM1637_CLK_PIN 14  // D5 on NodeMCU
#endif
#ifndef USERMOD_TM1637_DIO_PIN
#define USERMOD_TM1637_DIO_PIN 12  // D6 on NodeMCU
#endif

// Maximum number of registered external slot providers
#define TM1637D_PROVIDER_MAX  4

// ---------------------------------------------------------------------------
// TM1637DisplayUsermod
// ---------------------------------------------------------------------------

class TM1637DisplayUsermod : public Usermod {
  private:
    TM1637Display *display = nullptr;
    bool enabled   = true;
    bool initDone  = false;
    unsigned long lastUpdate      = 0;
    unsigned long lastBlinkToggle = 0;
    bool blinkColon = false;

    // Pin and brightness configuration
    int8_t  clkPin     = USERMOD_TM1637_CLK_PIN;
    int8_t  dioPin     = USERMOD_TM1637_DIO_PIN;
    uint8_t brightness = 2;  // 0–7, 7 is brightest

    // Configurable slot durations (milliseconds).
    // slotDurationMs applies to every info slot (temp, condition, custom, …).
    // timeDurationMs is the clock-face gap shown between full info cycles.
    uint16_t slotDurationMs = 3000;  // default: 3 s per info slot
    uint16_t timeDurationMs = 5000;  // default: 5 s of clock between cycles

    // Weather data received from WeatherApi usermod
    float temperatureC       = 0.0f;
    int   conditionCode      = 1000;  // default: clear/sunny (valid WeatherAPI code)
    bool  weatherFetched     = false;
    bool  weatherFetchFailed = false;

    // Display slot queue — rebuilt whenever it empties
    SlotQueue _queue;
    unsigned long _slotStart = 0;  // millis() when the current slot began

    // Whether the display is currently showing the inter-cycle clock face.
    // Becomes false as soon as the first slot of a new cycle is dequeued.
    bool _showingTime = true;
    unsigned long _timeStart = 0;  // millis() when time display began

    // External slot provider callbacks registered by other usermods
    SlotProviderFn _providers[TM1637D_PROVIDER_MAX];
    uint8_t        _providerCount = 0;

    // Display state machine
    enum DisplayState {
      NO_WIFI,      // WiFi not connected
      NO_INTERNET,  // WiFi connected but no internet
      NO_NTP,       // Internet reachable but NTP not configured or not yet synced
      SHOW_TIME     // Normal time/info display
    };

    DisplayState currentState = NO_WIFI;
    DisplayState lastState    = SHOW_TIME;  // Force initial update

    // Pre-built 7-segment patterns for status messages
    const uint8_t MSG_NO_WIFI[4]     = {0x00, 0x00, 0x00, 0x00};  // "    " blank
    const uint8_t MSG_NO_INTERNET[4] = {0x54, 0x50, 0x76, 0x78};  // "noIP"
    const uint8_t MSG_NO_NTP[4]      = {0x54, 0x5E, 0x78, 0x73};  // "ntP."
    const uint8_t MSG_WIFI[4]        = {0x3E, 0x06, 0x71, 0x06};  // "WiFI"

    // Config key strings (stored in PROGMEM to save RAM)
    static const char _name[];
    static const char _enabled[];
    static const char _clkPin[];
    static const char _dioPin[];
    static const char _brightness[];
    static const char _slotDuration[];
    static const char _timeDuration[];

    // Singleton pointer used by static WeatherApi subscription callbacks.
    // Safe because TM1637DisplayUsermod is registered as a single instance.
    static TM1637DisplayUsermod* _instance;

#ifdef USERMOD_WEATHER_API
    // Called by WeatherApi when a new temperature reading arrives.
    static void _onTempC(JsonVariant v) {
      if (!_instance) return;
      _instance->temperatureC = v.as<float>();
      { char tBuf[8]; dtostrf(_instance->temperatureC, 4, 1, tBuf);
        DEBUG_PRINTF("TM1637 Display: weather temp_c=%s\n", tBuf); }
    }

    // Called by WeatherApi when a new condition code arrives.
    // Marks weather as successfully fetched so the display cycle can begin.
    static void _onCondCode(JsonVariant v) {
      if (!_instance) return;
      _instance->conditionCode      = v.as<int>();
      _instance->weatherFetched     = true;
      _instance->weatherFetchFailed = false;
      DEBUG_PRINTF("TM1637 Display: weather condition_code=%d\n", _instance->conditionCode);
    }
#endif

    // Build a fresh display cycle into _queue.
    // Always adds weather slots when data is available, then asks every
    // registered provider to append its own slots.
    void _rebuildQueue() {
      _queue.clear();

#ifdef USERMOD_WEATHER_API
      if (weatherFetched && !weatherFetchFailed) {
        _queue.push(SLOT_TEMP,      slotDurationMs);
        _queue.push(SLOT_CONDITION, slotDurationMs);
        DEBUG_PRINTLN(F("TM1637 Display: queue rebuilt with temp+condition slots"));
      }
#endif

      // Let external usermods inject their own slots (e.g. baseball score)
      for (uint8_t i = 0; i < _providerCount; i++) {
        if (_providers[i]) _providers[i](_queue);
      }

      // If nothing was pushed (no weather yet, no providers), leave queue
      // empty so we just keep showing the clock until data arrives.
    }

    // Advance to the next slot in the queue, or start the inter-cycle time
    // display when the queue empties.
    void _advanceSlot(unsigned long now) {
      if (!_queue.empty()) {
        _queue.pop();
      }
      if (_queue.empty()) {
        // End of cycle — rebuild for the next pass and show clock face first
        _rebuildQueue();
        _showingTime = true;
        _timeStart   = now;
        DEBUG_PRINTLN(F("TM1637 Display: cycle complete, showing time"));
      } else {
        _slotStart   = now;
        _showingTime = false;
        DEBUG_PRINTF("TM1637 Display: advancing to next slot type=%d\n",
                     (int)_queue.front().type);
      }
    }

    // Render whichever slot is at the front of the queue.
    void _renderCurrentSlot(unsigned long now) {
      if (_queue.empty()) return;
      const DisplaySlot& slot = _queue.front();

      switch (slot.type) {
        case SLOT_TEMP:      _renderTemperature(); break;
        case SLOT_CONDITION: _renderCondition();   break;
        case SLOT_BASEBALL:  /* fall-through: custom text */
        case SLOT_CUSTOM:    _renderCustomText(slot.text); break;
        default:             break;
      }
    }

  public:
    // Register a callback that will be called every time the display queue
    // is rebuilt (i.e., every cycle).  The callback may push additional slots.
    // Safe to call from setup() of another usermod after this one has run setup().
    bool addSlotProvider(SlotProviderFn fn) {
      if (!fn || _providerCount >= TM1637D_PROVIDER_MAX) return false;
      _providers[_providerCount++] = fn;
      return true;
    }

    // Return the configured slot duration so callers can use a consistent value.
    uint16_t getSlotDurationMs() const { return slotDurationMs; }

    // Push a one-off SLOT_CUSTOM message into the *current* cycle immediately.
    // If the queue is full it is silently dropped.  The slot is inserted at the
    // front (after the current slot completes) so it plays before the normal
    // weather slots finish.
    bool showMessage(const char* msg, uint16_t durationMs = 0) {
      if (!msg || !initDone || !display) return false;
      uint16_t dur = durationMs > 0 ? durationMs : slotDurationMs;
      // If we are currently showing time, start the queue immediately with
      // this message rather than waiting for the time window to expire.
      if (_showingTime) {
        _queue.clear();
        bool ok = _queue.push(SLOT_CUSTOM, dur, msg);
        if (ok) {
          _showingTime = false;
          _slotStart   = millis();
          DEBUG_PRINTF("TM1637 Display: showMessage injected '%s' dur=%u\n", msg, dur);
        }
        return ok;
      }
      // Otherwise push after the current slot by temporarily borrowing the tail
      return _queue.push(SLOT_CUSTOM, dur, msg);
    }

    void setup() override {
      // Clean up any previous initialisation (e.g. after OTA) to avoid leaking
      // the old TM1637Display heap object and its reserved pins.
      if (initDone) {
        delete display;
        display  = nullptr;
        initDone = false;
        PinManager::deallocatePin(clkPin, PinOwner::UM_Unspecified);
        PinManager::deallocatePin(dioPin, PinOwner::UM_Unspecified);
      }

      if (PinManager::allocatePin(clkPin, true, PinOwner::UM_Unspecified) &&
          PinManager::allocatePin(dioPin, true, PinOwner::UM_Unspecified)) {
        display = new TM1637Display(clkPin, dioPin);
        if (display) {
          display->setBrightness(brightness);
          display->clear();
          initDone = true;
          DEBUG_PRINTLN(F("TM1637 Display: Initialized successfully"));
        } else {
          DEBUG_PRINTLN(F("TM1637 Display: Failed to create display object"));
        }
      } else {
        DEBUG_PRINTLN(F("TM1637 Display: Failed to allocate pins"));
      }

      g_tm1637DisplayInstance = this;

      // Start in the time-display phase; queue will be built on first cycle end
      _showingTime = true;
      _timeStart   = millis();

#ifdef USERMOD_WEATHER_API
      _instance = this;
      auto* wapi = (WeatherApiUsermod*)UsermodManager::lookup(USERMOD_ID_WEATHER_API);
      if (wapi) {
        wapi->subscribe("current.temp_c",         _onTempC);
        wapi->subscribe("current.condition.code", _onCondCode);
        DEBUG_PRINTLN(F("TM1637 Display: subscribed to WeatherApi usermod"));
      } else {
        DEBUG_PRINTLN(F("TM1637 Display: WeatherApi usermod not found — weather display disabled"));
      }
#endif
    }

    void loop() override {
      if (!enabled || !initDone || !display) return;

      unsigned long now = millis();

      // Poll display state every 100 ms for responsiveness
      if (now - lastUpdate > 100) {
        lastUpdate = now;
        _updateDisplayState();
      }

      // Always clear the display at startup or when state changes
      static DisplayState prevState = SHOW_TIME;
      if (currentState != prevState) {
        display->clear();
        prevState = currentState;
      }

      switch (currentState) {
        case NO_WIFI:
          if (currentState != lastState) {
            display->clear();
            _showScrollingMessage("WiFi");
            lastState = currentState;
          }
          break;

        case NO_INTERNET:
          if (currentState != lastState) {
            display->clear();
            display->setSegments(MSG_NO_INTERNET);
            lastState = currentState;
          }
          break;

        case NO_NTP:
          if (currentState != lastState) {
            display->clear();
            display->setSegments(MSG_NO_NTP);
            lastState = currentState;
          }
          break;

        case SHOW_TIME:
          lastState = currentState;
          if (_showingTime) {
            // Show clock face until the time window expires
            if ((now - _timeStart) >= (unsigned long)timeDurationMs) {
              // Time window done — try to populate the queue if it is empty.
              // This handles first boot where _rebuildQueue() was never called yet.
              if (_queue.empty()) {
                _rebuildQueue();
              }
              if (!_queue.empty()) {
                // Queue has content — start first slot of the new cycle
                _showingTime = false;
                _slotStart   = now;
                DEBUG_PRINTF("TM1637 Display: time window done, starting slot type=%d\n",
                             (int)_queue.front().type);
              } else {
                // Still no data (e.g. weather not yet fetched) — reset timer
                // and keep showing the clock until a source provides content.
                _timeStart = now;
                _renderTime(now);
              }
            } else {
              _renderTime(now);
            }
          } else {
            // Playing through the info queue
            if (_queue.empty()) {
              // No slots available (e.g. no weather yet, no providers pushed anything)
              // Rebuild and fall back to clock until data arrives
              _rebuildQueue();
              _showingTime = true;
              _timeStart   = now;
              _renderTime(now);
            } else {
              // Check if the current slot has expired
              if ((now - _slotStart) >= (unsigned long)_queue.front().durationMs) {
                _advanceSlot(now);
              }
              // Render whatever is at the front (may have just changed)
              if (_showingTime) {
                _renderTime(now);
              } else {
                _renderCurrentSlot(now);
              }
            }
          }
          break;
      }
    }

    void connected() override {
      DEBUG_PRINTLN(F("TM1637 Display: WiFi connected"));
      // Display state is updated on the next loop iteration
    }

  private:
    void _updateDisplayState() {
      DisplayState newState;

      if (WiFi.status() != WL_CONNECTED) {
        newState = NO_WIFI;
      } else if (!_isConnectedToInternet()) {
        newState = NO_INTERNET;
      } else if (!ntpEnabled || strlen(ntpServerName) == 0) {
        newState = NO_NTP;
      } else {
        updateLocalTime();
        bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);
        newState = timeValid ? SHOW_TIME : NO_NTP;
      }

      // Reset queue state cleanly when leaving SHOW_TIME
      if (currentState == SHOW_TIME && newState != SHOW_TIME) {
        _queue.clear();
        _showingTime = true;
      }

      currentState = newState;
    }

    bool _isConnectedToInternet() {
      // Treat a non-zero gateway as proof of internet reachability.
      return (WiFi.gatewayIP() != IPAddress(0, 0, 0, 0));
    }

    // Render the current time (HH:MM, 12-hour) with a blinking colon.
    // Shows "----" if NTP has not yet delivered a valid time.
    void _renderTime(unsigned long now) {
      if (now - lastBlinkToggle <= 500) return;  // Only refresh every 500 ms
      lastBlinkToggle = now;
      blinkColon      = !blinkColon;

      updateLocalTime();
      bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);
      if (timeValid) {
        int currentHour   = hour(localTime);
        int currentMinute = minute(localTime);
        static int lastLoggedMinute = -1;

        if (currentHour == 0)       currentHour = 12;
        else if (currentHour > 12)  currentHour -= 12;

        int timeValue = currentHour * 100 + currentMinute;
        display->showNumberDecEx(timeValue, blinkColon ? 0b01000000 : 0b00000000, true);
        if (currentMinute != lastLoggedMinute) {
          DEBUG_PRINTF("TM1637 Display: showing time %02d:%02d\n", currentHour, currentMinute);
          lastLoggedMinute = currentMinute;
        }
      } else {
        const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
        display->setSegments(dashes);
      }
    }

    void _showScrollingMessage(const char* message) {
      if (strcmp(message, "WiFi") == 0) {
        display->setSegments(MSG_WIFI);
      }
    }

    // Render the current temperature on the display.
    // Format: "  XC" (single digit), " XXC" (double), "XXXC" (triple),
    //         "-XC" / "-XXC" (negative).
    void _renderTemperature() {
      if (!weatherFetched || weatherFetchFailed) {
        const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
        display->setSegments(dashes);
        return;
      }

      const uint8_t kSegCelsius = 0x39;
      const uint8_t kSegMinus   = 0x40;
      const uint8_t kSegBlank   = 0x00;

      int tempInt = (int)roundf(temperatureC);
      uint8_t segs[4];

      if (tempInt >= 0 && tempInt < 10) {
        segs[0] = kSegBlank; segs[1] = kSegBlank;
        segs[2] = display->encodeDigit(tempInt); segs[3] = kSegCelsius;
      } else if (tempInt >= 10 && tempInt < 100) {
        segs[0] = kSegBlank;
        segs[1] = display->encodeDigit(tempInt / 10);
        segs[2] = display->encodeDigit(tempInt % 10);
        segs[3] = kSegCelsius;
      } else if (tempInt >= 100) {
        segs[0] = display->encodeDigit(tempInt / 100);
        segs[1] = display->encodeDigit((tempInt / 10) % 10);
        segs[2] = display->encodeDigit(tempInt % 10);
        segs[3] = kSegCelsius;
      } else {
        int absTemp = -tempInt;
        segs[0] = kSegMinus;
        if (absTemp < 10) {
          segs[1] = kSegBlank;
          segs[2] = display->encodeDigit(absTemp);
        } else {
          segs[1] = display->encodeDigit(absTemp / 10);
          segs[2] = display->encodeDigit(absTemp % 10);
        }
        segs[3] = kSegCelsius;
      }
      display->setSegments(segs);
    }

#ifdef USERMOD_WEATHER_API
    // Display a 3-letter condition abbreviation + severity digit (1–3).
    // Format: XXX# where XXX is the category and # is 1=mild, 2=moderate, 3=severe.
    //
    //  Condition   Display    Examples
    //  Clear        CLr       (no digit — always clear)
    //  Cloudy      CLD1–3    partly=1, cloudy=2, overcast=3
    //  Fog         FOG1–3    mist=1, fog=2, freezing=3
    //  Thunder     tHN1–3    possible=1, light+thunder=2, heavy+thunder=3
    //  Snow/Ice    Sno1–3    light=1, moderate=2, heavy/blizzard=3
    //  Rain        rAn1–3    light=1, moderate=2, heavy=3
    void _renderCondition() {
      static const uint8_t kSeg_S = 0x6D;
      static const uint8_t kSeg_C = 0x39;
      static const uint8_t kSeg_L = 0x38;
      static const uint8_t kSeg_d = 0x5E;
      static const uint8_t kSeg_F = 0x71;
      static const uint8_t kSeg_o = 0x5C;
      static const uint8_t kSeg_G = 0x3D;
      static const uint8_t kSeg_t = 0x78;
      static const uint8_t kSeg_H = 0x76;
      static const uint8_t kSeg_n = 0x54;
      static const uint8_t kSeg_r = 0x50;
      static const uint8_t kSeg_A = 0x77;
      static const uint8_t kBlank = 0x00;

      uint8_t segs[4];

      if (conditionCode == 1000) {
        segs[0] = kBlank; segs[1] = kSeg_C; segs[2] = kSeg_L; segs[3] = kSeg_d;
        display->setSegments(segs);
        return;
      }

      segs[3] = display->encodeDigit(conditionSeverity(conditionCode));

      if (conditionCode <= 1009) {
        segs[0] = kSeg_C; segs[1] = kSeg_L; segs[2] = kSeg_d;
      } else if (conditionCode == 1030 || conditionCode == 1135 || conditionCode == 1147) {
        segs[0] = kSeg_F; segs[1] = kSeg_o; segs[2] = kSeg_G;
      } else if (conditionCode == 1087 || conditionCode >= 1273) {
        segs[0] = kSeg_t; segs[1] = kSeg_H; segs[2] = kSeg_n;
      } else if ((conditionCode >= 1114 && conditionCode <= 1117) ||
                 (conditionCode >= 1210 && conditionCode <= 1264)) {
        segs[0] = kSeg_S; segs[1] = kSeg_n; segs[2] = kSeg_o;
      } else {
        segs[0] = kSeg_r; segs[1] = kSeg_A; segs[2] = kSeg_n;
      }

      display->setSegments(segs);
    }
#else
    void _renderCondition() {}  // stub when weather API is not compiled in
#endif // USERMOD_WEATHER_API

    // Render a 4-character custom text slot.
    // Supports digits (0–9) and '-'; all other characters become blanks.
    void _renderCustomText(const char* text) {
      uint8_t segs[4] = {0, 0, 0, 0};
      for (uint8_t i = 0; i < 4 && text[i] != '\0'; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9') segs[i] = display->encodeDigit(c - '0');
        else if (c == '-')        segs[i] = 0x40;
      }
      display->setSegments(segs);
    }

  public:
    uint16_t getId() override {
      return USERMOD_ID_TM1637_DISPLAY;
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");

      JsonArray tm1637 = user.createNestedArray(FPSTR(_name));

      if (enabled && initDone) {
        const char* stateStr = "Unknown";
        switch (currentState) {
          case NO_WIFI:     stateStr = "No WiFi";      break;
          case NO_INTERNET: stateStr = "No Internet";  break;
          case NO_NTP:      stateStr = "No NTP";       break;
          case SHOW_TIME:   stateStr = "Showing Time"; break;
        }
        tm1637.add(stateStr);
        char qBuf[32];
        snprintf(qBuf, sizeof(qBuf), "Queue: %d slots  slot=%us  time=%us",
                 _queue.count, slotDurationMs / 1000, timeDurationMs / 1000);
        tm1637.add(qBuf);
#ifdef USERMOD_WEATHER_API
        if (weatherFetched && !weatherFetchFailed) {
          char tBuf[8]; dtostrf(temperatureC, 4, 1, tBuf);
          char wBuf[48];
          snprintf(wBuf, sizeof(wBuf), "%s C  %s", tBuf, conditionDescription(conditionCode));
          tm1637.add(wBuf);
        } else if (WiFi.status() == WL_CONNECTED) {
          tm1637.add("Weather: fetching...");
        }
#endif
      } else {
        tm1637.add("Disabled");
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]      = enabled;
      top[FPSTR(_clkPin)]       = clkPin;
      top[FPSTR(_dioPin)]       = dioPin;
      top[FPSTR(_brightness)]   = brightness;
      top[FPSTR(_slotDuration)] = slotDurationMs;
      top[FPSTR(_timeDuration)] = timeDurationMs;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;

      bool configComplete = true;
      configComplete &= getJsonValue(top[FPSTR(_enabled)],      enabled);
      configComplete &= getJsonValue(top[FPSTR(_clkPin)],       clkPin);
      configComplete &= getJsonValue(top[FPSTR(_dioPin)],       dioPin);
      configComplete &= getJsonValue(top[FPSTR(_brightness)],   brightness,   (uint8_t)2);
      configComplete &= getJsonValue(top[FPSTR(_slotDuration)], slotDurationMs, (uint16_t)3000);
      configComplete &= getJsonValue(top[FPSTR(_timeDuration)], timeDurationMs, (uint16_t)5000);

      // Enforce sensible minimums so the display is always readable
      if (slotDurationMs < 500)  slotDurationMs = 500;
      if (timeDurationMs < 1000) timeDurationMs = 1000;

      DEBUG_PRINTLN(F("TM1637 Display: readFromConfig"));
      DEBUG_PRINTF("  enabled=%d  clkPin=%d  dioPin=%d  brightness=%d  slot=%u  time=%u\n",
        enabled, clkPin, dioPin, brightness, slotDurationMs, timeDurationMs);

      if (display) display->setBrightness(brightness);

      return configComplete;
    }

    void appendConfigData() override {
      oappend(SET_F("addInfo('TM1637Display:CLK-pin',1,'D5 / GPIO14 on NodeMCU');"));
      oappend(SET_F("addInfo('TM1637Display:DIO-pin',1,'D6 / GPIO12 on NodeMCU');"));
      oappend(SET_F("addInfo('TM1637Display:brightness',1,'0 (dim) \xe2\x80\x93 7 (bright)');"));
      oappend(SET_F("addInfo('TM1637Display:slot-duration-ms',1,'How long each info slot is shown (ms, min 500)');"));
      oappend(SET_F("addInfo('TM1637Display:time-duration-ms',1,'How long the clock is shown between info cycles (ms, min 1000)');"));
    }

    void enable(bool en)  { enabled = en; }
    bool isEnabled() const { return enabled; }
};

// Static member definitions
TM1637DisplayUsermod* TM1637DisplayUsermod::_instance = nullptr;
const char TM1637DisplayUsermod::_name[]         PROGMEM = "TM1637Display";
const char TM1637DisplayUsermod::_enabled[]      PROGMEM = "enabled";
const char TM1637DisplayUsermod::_clkPin[]       PROGMEM = "CLK-pin";
const char TM1637DisplayUsermod::_dioPin[]       PROGMEM = "DIO-pin";
const char TM1637DisplayUsermod::_brightness[]   PROGMEM = "brightness";
const char TM1637DisplayUsermod::_slotDuration[] PROGMEM = "slot-duration-ms";
const char TM1637DisplayUsermod::_timeDuration[] PROGMEM = "time-duration-ms";

// Create and register the usermod instance
static TM1637DisplayUsermod tm1637Display;
REGISTER_USERMOD(tm1637Display);

bool tm1637DisplayShowMessage(const char* msg, uint16_t durationMs) {
  if (!g_tm1637DisplayInstance) return false;
  return g_tm1637DisplayInstance->showMessage(msg, durationMs);
}

bool tm1637DisplayAddSlotProvider(SlotProviderFn fn) {
  if (!g_tm1637DisplayInstance) return false;
  return g_tm1637DisplayInstance->addSlotProvider(fn);
}

uint16_t tm1637DisplayGetSlotDurationMs() {
  if (!g_tm1637DisplayInstance) return 3000;
  return g_tm1637DisplayInstance->getSlotDurationMs();
}

#endif // USERMOD_TM1637_DISPLAY
