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
 * Drives a TM1637 4-digit 7-segment display showing:
 *   - Current time (from NTP), blinking colon
 *   - Outside temperature (°C) for 3 s every 5 s when weather data is available
 *   - Weather condition abbreviation + severity (e.g. "rAn2", "FOG1") after the temperature
 *   - Status messages when WiFi / internet / NTP are unavailable
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

// How often and how long to show temperature + condition on the display
#define TM1637D_TEMP_SHOW_EVERY_MS    5000U   // show temp every 5 seconds
#define TM1637D_TEMP_SHOW_DURATION_MS 3000U   // keep temp visible for 3 seconds

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

    // Weather data received from WeatherApi usermod
    float temperatureC       = 0.0f;
    int   conditionCode      = 1000;  // default: clear/sunny (valid WeatherAPI code)
    bool  weatherFetched     = false;
    bool  weatherFetchFailed = false;

    // Temperature / condition display cycling
    bool          showingTemp        = false;
    bool          showingCondition   = false;
    unsigned long tempShowStart      = 0;  // when we started showing temp
    unsigned long conditionShowStart = 0;  // when we started showing condition
    unsigned long lastTempCycleEnd   = 0;  // when the full temp+condition cycle ended

    // Display state machine
    enum DisplayState {
      NO_WIFI,      // WiFi not connected
      NO_INTERNET,  // WiFi connected but no internet
      NO_NTP,       // Internet reachable but NTP not configured or not yet synced
      SHOW_TIME     // Normal time display
    };

    DisplayState currentState = NO_WIFI;
    DisplayState lastState    = SHOW_TIME;  // Force initial update
    char overrideMessage[5]   = {0, 0, 0, 0, 0};
    unsigned long overrideUntil = 0;

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

  public:
    bool showMessage(const char* msg, uint16_t durationMs = 3000) {
      if (!msg || !initDone || !display) return false;
      strncpy(overrideMessage, msg, sizeof(overrideMessage) - 1);
      overrideMessage[sizeof(overrideMessage) - 1] = '\0';
      overrideUntil = millis() + durationMs;
      return true;
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

      if (overrideMessage[0] != '\0' && (long)(overrideUntil - now) > 0) {
        uint8_t segs[4] = {0, 0, 0, 0};
        for (uint8_t i = 0; i < 4; i++) {
          char c = overrideMessage[i];
          if (c == '\0' || c == ' ') segs[i] = 0x00;
          else if (c >= '0' && c <= '9') segs[i] = display->encodeDigit(c - '0');
          else if (c == '-') segs[i] = 0x40;
          else segs[i] = 0x00;
        }
        display->setSegments(segs);
        return;
      }

      if (overrideMessage[0] != '\0' && (long)(overrideUntil - now) <= 0) {
        overrideMessage[0] = '\0';
      }

      // Poll display state every 100 ms for responsiveness
      if (now - lastUpdate > 100) {
        lastUpdate = now;
        updateDisplayState();
      }

      // Always clear the display at startup or when state changes
      static DisplayState prevState = SHOW_TIME;
      if (currentState != prevState) {
        display->clear();
        prevState = currentState;
      }

      // Handle display content based on current state
      switch (currentState) {
        case NO_WIFI:
          if (currentState != lastState) {
            display->clear();
            showScrollingMessage("WiFi", 4);
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
#ifdef USERMOD_WEATHER_API
          if (showingTemp) {
            if (now - tempShowStart >= TM1637D_TEMP_SHOW_DURATION_MS) {
              // Temp window done — move to condition abbreviation
              showingTemp        = false;
              showingCondition   = true;
              conditionShowStart = now;
              DEBUG_PRINTF("TM1637 Display: displaying condition %d = \"%s\"\n",
                conditionCode, conditionDescription(conditionCode));
              showConditionText();
            } else {
              showTemperature();
            }
          } else if (showingCondition) {
            if (now - conditionShowStart >= TM1637D_TEMP_SHOW_DURATION_MS) {
              // Condition window done — return to clock.
              // Reset lastTempCycleEnd to NOW so the 5-second gap starts after
              // the full cycle completes, preventing immediate back-to-back replay.
              showingCondition = false;
              lastTempCycleEnd = now;
            } else {
              showConditionText();
            }
          } else {
            if (weatherFetched && !weatherFetchFailed &&
                (now - lastTempCycleEnd >= TM1637D_TEMP_SHOW_EVERY_MS)) {
              // 5-second gap elapsed — start with temperature
              showingTemp   = true;
              tempShowStart = now;
              { char tBuf[8]; dtostrf(temperatureC, 4, 1, tBuf);
                DEBUG_PRINTF("TM1637 Display: displaying temperature %s C\n", tBuf); }
              showTemperature();
            } else {
              showTime();
            }
          }
#else
          showTime();
#endif
          break;
      }
    }

    void connected() override {
      DEBUG_PRINTLN(F("TM1637 Display: WiFi connected"));
      // Display state is updated on the next loop iteration
    }

  private:
    void updateDisplayState() {
      DisplayState newState;

      if (WiFi.status() != WL_CONNECTED) {
        newState = NO_WIFI;
      } else if (!isConnectedToInternet()) {
        newState = NO_INTERNET;
      } else if (!ntpEnabled || strlen(ntpServerName) == 0) {
        newState = NO_NTP;
      } else {
        updateLocalTime();
        bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);
        newState = timeValid ? SHOW_TIME : NO_NTP;
      }

      // Reset temp/condition cycle flags when leaving SHOW_TIME so the
      // cycle restarts cleanly on reconnect.
      if (currentState == SHOW_TIME && newState != SHOW_TIME) {
        showingTemp      = false;
        showingCondition = false;
      }

      currentState = newState;
    }

    bool isConnectedToInternet() {
      // Treat a non-zero gateway as proof of internet reachability.
      // A more thorough check would ping an external host, but that
      // adds latency and complexity not warranted here.
      return (WiFi.gatewayIP() != IPAddress(0, 0, 0, 0));
    }

    // Render the current time (HH:MM, 12-hour) with a blinking colon.
    // Shows "----" if NTP has not yet delivered a valid time.
    void showTime() {
      if (currentState != lastState) lastState = currentState;

      unsigned long now = millis();
      if (now - lastBlinkToggle <= 500) return;  // Only refresh every 500 ms

      lastBlinkToggle = now;
      blinkColon      = !blinkColon;

      updateLocalTime();
      bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);
      if (timeValid) {
        int currentHour   = hour(localTime);
        int currentMinute = minute(localTime);
        static int lastLoggedMinute = -1;

        // Convert to 12-hour format
        if (currentHour == 0)       currentHour = 12;
        else if (currentHour > 12)  currentHour -= 12;

        int timeValue = currentHour * 100 + currentMinute;
        display->showNumberDecEx(timeValue, blinkColon ? 0b01000000 : 0b00000000, true);
        if (currentMinute != lastLoggedMinute) {
          DEBUG_PRINTF("TM1637 Display: showing time %02d:%02d\n", currentHour, currentMinute);
          lastLoggedMinute = currentMinute;
        }
      } else {
        const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};  // "----"
        display->setSegments(dashes);
      }
    }

    // Display a short status message.  Currently only "WiFI" is handled;
    // the parameter is retained for future expansion.
    void showScrollingMessage(const char* message, int /*len*/) {
      if (strcmp(message, "WiFi") == 0) {
        display->setSegments(MSG_WIFI);
      }
    }

    // Render the current temperature on the display.
    // Format: "  XC" (single digit), " XXC" (double), "XXXC" (triple),
    //         "-XC" / "-XXC" (negative).
    void showTemperature() {
      if (!weatherFetched || weatherFetchFailed) {
        const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};  // "----"
        display->setSegments(dashes);
        return;
      }

      const uint8_t kSegCelsius = 0x39;  // C
      const uint8_t kSegMinus   = 0x40;  // minus sign
      const uint8_t kSegBlank   = 0x00;  // blank digit

      int tempInt = (int)roundf(temperatureC);
      uint8_t segs[4];

      if (tempInt >= 0 && tempInt < 10) {
        segs[0] = kSegBlank;
        segs[1] = kSegBlank;
        segs[2] = display->encodeDigit(tempInt);
        segs[3] = kSegCelsius;
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
        // Negative temperature: "-XC" or "-XXC"
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
    void showConditionText() {
      static const uint8_t kSeg_S = 0x6D;  // S
      static const uint8_t kSeg_C = 0x39;  // C
      static const uint8_t kSeg_L = 0x38;  // L
      static const uint8_t kSeg_d = 0x5E;  // d (lowercase)
      static const uint8_t kSeg_F = 0x71;  // F
      static const uint8_t kSeg_o = 0x5C;  // o (lowercase)
      static const uint8_t kSeg_G = 0x3D;  // G
      static const uint8_t kSeg_t = 0x78;  // t
      static const uint8_t kSeg_H = 0x76;  // H
      static const uint8_t kSeg_n = 0x54;  // n (lowercase)
      static const uint8_t kSeg_r = 0x50;  // r (lowercase)
      static const uint8_t kSeg_A = 0x77;  // A
      static const uint8_t kBlank = 0x00;  // blank digit

      uint8_t segs[4];

      if (conditionCode == 1000) {
        // " CLr" — avoids "SUN" being read as Sunday; valid day and night
        segs[0] = kBlank;  segs[1] = kSeg_C;  segs[2] = kSeg_L;  segs[3] = kSeg_d;
        display->setSegments(segs);
        return;
      }

      segs[3] = display->encodeDigit(conditionSeverity(conditionCode));

      if (conditionCode <= 1009) {
        segs[0] = kSeg_C;  segs[1] = kSeg_L;  segs[2] = kSeg_d;  // CLD
      } else if (conditionCode == 1030 ||
                 conditionCode == 1135 ||
                 conditionCode == 1147) {
        segs[0] = kSeg_F;  segs[1] = kSeg_o;  segs[2] = kSeg_G;  // FOG
      } else if (conditionCode == 1087 || conditionCode >= 1273) {
        segs[0] = kSeg_t;  segs[1] = kSeg_H;  segs[2] = kSeg_n;  // tHN
      } else if ((conditionCode >= 1114 && conditionCode <= 1117) ||
                  (conditionCode >= 1210 && conditionCode <= 1264)) {
        segs[0] = kSeg_S;  segs[1] = kSeg_n;  segs[2] = kSeg_o;  // Sno
      } else {
        segs[0] = kSeg_r;  segs[1] = kSeg_A;  segs[2] = kSeg_n;  // rAn
      }

      display->setSegments(segs);
    }
#endif // USERMOD_WEATHER_API

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
      top[FPSTR(_enabled)]    = enabled;
      top[FPSTR(_clkPin)]     = clkPin;
      top[FPSTR(_dioPin)]     = dioPin;
      top[FPSTR(_brightness)] = brightness;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;

      bool configComplete = true;
      configComplete &= getJsonValue(top[FPSTR(_enabled)],    enabled);
      configComplete &= getJsonValue(top[FPSTR(_clkPin)],     clkPin);
      configComplete &= getJsonValue(top[FPSTR(_dioPin)],     dioPin);
      configComplete &= getJsonValue(top[FPSTR(_brightness)], brightness, (uint8_t)2);

      DEBUG_PRINTLN(F("TM1637 Display: readFromConfig"));
      DEBUG_PRINTF("  enabled=%d  clkPin=%d  dioPin=%d  brightness=%d\n",
        enabled, clkPin, dioPin, brightness);

      // Apply brightness immediately without requiring a reboot
      if (display) display->setBrightness(brightness);

      return configComplete;
    }

    void appendConfigData() override {
      oappend(SET_F("addInfo('TM1637Display:CLK-pin',1,'D5 / GPIO14 on NodeMCU');"));
      oappend(SET_F("addInfo('TM1637Display:DIO-pin',1,'D6 / GPIO12 on NodeMCU');"));
      oappend(SET_F("addInfo('TM1637Display:brightness',1,'0 (dim) \xe2\x80\x93 7 (bright)');"));
    }

    void enable(bool en)  { enabled = en; }
    bool isEnabled() const { return enabled; }
};

// Static member definitions
TM1637DisplayUsermod* TM1637DisplayUsermod::_instance = nullptr;
const char TM1637DisplayUsermod::_name[]       PROGMEM = "TM1637Display";
const char TM1637DisplayUsermod::_enabled[]    PROGMEM = "enabled";
const char TM1637DisplayUsermod::_clkPin[]     PROGMEM = "CLK-pin";
const char TM1637DisplayUsermod::_dioPin[]     PROGMEM = "DIO-pin";
const char TM1637DisplayUsermod::_brightness[] PROGMEM = "brightness";

// Create and register the usermod instance
static TM1637DisplayUsermod tm1637Display;
REGISTER_USERMOD(tm1637Display);

bool tm1637DisplayShowMessage(const char* msg, uint16_t durationMs) {
  if (!g_tm1637DisplayInstance) return false;
  return g_tm1637DisplayInstance->showMessage(msg, durationMs);
}

#endif // USERMOD_TM1637_DISPLAY
