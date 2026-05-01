#pragma once
#ifdef USERMOD_WEATHER_API

#include "wled.h"

/*
 * WeatherApi Usermod — Public Interface
 * ======================================
 * Fetches current weather from WeatherAPI.com on a configurable interval.
 * Other usermods subscribe to individual JSON field values by dot-notation path.
 * Only subscribed fields are retained after parsing — unsubscribed data is
 * discarded by the ArduinoJSON filter before it reaches RAM.
 *
 * Usage from another usermod's setup():
 *
 *   #ifdef USERMOD_WEATHER_API
 *   #include "../weather_api/usermod_weather_api.h"
 *
 *   static float s_tempC    = 0.f;
 *   static int   s_condCode = 1000;
 *   static void onTempC(JsonVariant v)    { s_tempC    = v.as<float>(); }
 *   static void onCondCode(JsonVariant v) { s_condCode = v.as<int>();   }
 *
 *   // In your setup():
 *   auto* wapi = (WeatherApiUsermod*)UsermodManager::lookup(USERMOD_ID_WEATHER_API);
 *   if (wapi) {
 *     wapi->subscribe("current.temp_c",          onTempC);
 *     wapi->subscribe("current.condition.code",  onCondCode);
 *   }
 *   #endif
 *
 * API key configuration:
 *   1. WLED Settings UI  — enter at runtime, stored in cfg.json on device (recommended)
 *   2. Build-time define — add to platformio_override.ini (already gitignored):
 *        build_flags = -D WEATHERAPI_KEY='"your-key-here"'
 *
 * Location:
 *   The weather API will automatically use the latitude/longitude set in WLED's
 *   Time settings page (http://device-ip/settings/time). If those values are 0,
 *   or to override with a specific location, you can also configure separate
 *   latitude/longitude values in the Weather API settings. The API will fall back
 *   to IP-based auto-detection if both WLED and Weather API location values are 0.
 */

// Maximum simultaneous subscriptions. Override in platformio_override.ini if needed:
//   build_flags = -D WEATHER_API_MAX_SUBS=16
#ifndef WEATHER_API_MAX_SUBS
  #define WEATHER_API_MAX_SUBS 12
#endif

// ArduinoJSON filter document capacity — increase if subscription paths are numerous or deep.
#ifndef WEATHER_API_FILTER_DOC_SIZE
  #define WEATHER_API_FILTER_DOC_SIZE 512
#endif

// ArduinoJSON response document capacity — holds only the filtered fields.
#ifndef WEATHER_API_RESPONSE_DOC_SIZE
  #define WEATHER_API_RESPONSE_DOC_SIZE 256
#endif

// Default fetch interval in milliseconds.
#ifndef WEATHER_API_FETCH_INTERVAL_MS
  #define WEATHER_API_FETCH_INTERVAL_MS (60UL * 1000UL)
#endif

// Callback type. The JsonVariant is valid only during the callback; copy values out immediately.
typedef void (*WeatherValueCallback)(JsonVariant value);

struct WeatherSubscription {
  const char*          path;      // dot-notation, e.g. "current.temp_c"
  WeatherValueCallback callback;  // called with the parsed value after each successful fetch
};

class WeatherApiUsermod : public Usermod {
 private:
  bool     _enabled         = true;
  char     _apiKey[48]      = "";
  float    _latitude        = 0.0f;
  float    _longitude       = 0.0f;
  uint32_t _intervalSec     = (uint32_t)(WEATHER_API_FETCH_INTERVAL_MS / 1000UL);

  unsigned long _lastFetch      = 0;
  bool          _fetchPending   = false;   // triggers on first WiFi connect
  bool          _lastFetchOk    = false;
  time_t        _lastFetchTime  = 0;
  String        _lastError      = "";
  char          _lastUrl[160]   = "";   // last URL sent to WeatherAPI (for display in info/settings)

  WeatherSubscription _subs[WEATHER_API_MAX_SUBS];
  uint8_t             _subCount = 0;

  static const char _name[];
  static const char _keyEnabled[];
  static const char _keyApiKey[];
  static const char _keyLat[];
  static const char _keyLon[];
  static const char _keyInterval[];

  // Write a dot-notation path into an ArduinoJSON v6 filter object as nested true-leaves.
  // Shared intermediate nodes (e.g. "current") are reused rather than recreated.
  // path must be a dot-separated ASCII string, max depth 8 segments.
  static void _setFilterPath(JsonObject root, const char* path);

  // Traverse root following dot-notation path. Returns a null JsonVariant on any miss.
  static JsonVariant _getByPath(JsonVariant root, const char* path);

  // Perform the blocking HTTP fetch and dispatch to subscribers.
  void _doFetch();

 public:
  // Register a subscription. path is dot-notation (e.g. "current.temp_c").
  // callback is invoked with the matching JsonVariant after every successful fetch.
  // Returns true if registered; false when the subscription table (_subCount == WEATHER_API_MAX_SUBS) is full.
  // Call from another usermod's setup(), before the first fetch fires.
  bool subscribe(const char* path, WeatherValueCallback callback);

  // Force a fetch on the next loop iteration regardless of the interval.
  // Useful after a config change or on explicit user request.
  void triggerFetch();

  // True if the most recent fetch succeeded.
  bool isAvailable() const { return _lastFetchOk; }

  // WLED usermod interface
  void     setup()                                  override;
  void     loop()                                   override;
  void     connected()                              override;
  void     addToJsonInfo(JsonObject& root)          override;
  void     addToConfig(JsonObject& root)            override;
  bool     readFromConfig(JsonObject& root)         override;
  void     appendConfigData()                       override;
  uint16_t getId()                                  override { return USERMOD_ID_WEATHER_API; }
};

#endif // USERMOD_WEATHER_API
