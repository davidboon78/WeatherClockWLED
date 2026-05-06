#include "wled.h"
#ifdef USERMOD_WEATHER_API

#ifdef ESP8266
  #include <ESP8266HTTPClient.h>
#else
  #include <HTTPClient.h>
#endif
#include <WiFiClient.h>


#include "usermod_weather_api.h"
#include "weather_light_patterns.h"

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------
const char WeatherApiUsermod::_name[]       PROGMEM = "WeatherApi";
const char WeatherApiUsermod::_keyEnabled[] PROGMEM = "enabled";
const char WeatherApiUsermod::_keyApiKey[]  PROGMEM = "api-key";
const char WeatherApiUsermod::_keyInterval[]PROGMEM = "interval";

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

// Write a dot-notation path into an ArduinoJSON v6 filter object.
// Intermediate nodes are created as nested objects; the final segment is set to true.
// Existing intermediate nodes are reused — safe to call with overlapping prefixes
// (e.g. "current.temp_c" and "current.condition.code" both share "current").
void WeatherApiUsermod::_setFilterPath(JsonObject root, const char* path) {
  char buf[64];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  JsonObject cur = root;
  char* seg  = strtok(buf, ".");
  while (seg) {
    char* next = strtok(nullptr, ".");
    if (next == nullptr) {
      // Leaf — mark as true so ArduinoJSON keeps this field
      cur[seg] = true;
    } else {
      // Intermediate — reuse existing nested object or create a new one
      if (cur[seg].is<JsonObject>()) {
        cur = cur[seg].as<JsonObject>();
      } else {
        cur = cur.createNestedObject(seg);
      }
    }
    seg = next;
  }
}

// Traverse root via dot-notation path and return the matching JsonVariant.
// Returns a null JsonVariant if any segment is missing.
JsonVariant WeatherApiUsermod::_getByPath(JsonVariant root, const char* path) {
  char buf[64];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  JsonVariant cur = root;
  char* seg = strtok(buf, ".");
  while (seg && !cur.isNull()) {
    cur = cur[seg];
    seg = strtok(nullptr, ".");
  }
  return cur;
}

// Blocking HTTP fetch from WeatherAPI.com.
// Builds an ArduinoJSON filter from subscribed paths so only those fields are
// retained after parsing, keeping heap usage proportional to what was subscribed.
void WeatherApiUsermod::_doFetch() {
  if (_subCount == 0) {
    DEBUG_PRINTLN(F("WeatherApi: no subscribers, skipping fetch"));
    return;
  }
  if (strlen(_apiKey) == 0) {
    _lastError   = "No API key";
    _lastFetchOk = false;
    DEBUG_PRINTLN(F("WeatherApi: API key not configured"));
    return;
  }

  // Guard against low-heap crashes. On ESP8266, malloc() returns NULL when the
  // heap is exhausted; String operations (via HTTPClient::sendHeader etc.) write
  // through that NULL and trigger a StoreProhibited exception (exception 29).
  // The 'Heap low, purging segments' log line before the crash confirms the heap
  // was already critically low when the fetch was attempted.
  {
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < WEATHER_API_MIN_HEAP_B) {
      _lastError   = "Low heap";
      _lastFetchOk = false;
      DEBUG_PRINTF("WeatherApi: fetch deferred, low heap (%u B free)\n", freeHeap);
      return;
    }
  }

  // Build request URL using WLED's lat/lon from Time settings.
  // Falls back to auto:ip when both are zero (IP-based geolocation).
  char url[160];
  if (_latitude != 0.0f || _longitude != 0.0f) {
    char latBuf[12], lonBuf[12];
    dtostrf(_latitude,  1, 4, latBuf);
    dtostrf(_longitude, 1, 4, lonBuf);
    snprintf_P(url, sizeof(url),
               PSTR("http://api.weatherapi.com/v1/current.json?key=%s&q=%s,%s&aqi=no"),
               _apiKey, latBuf, lonBuf);
  } else {
    snprintf_P(url, sizeof(url),
               PSTR("http://api.weatherapi.com/v1/current.json?key=%s&q=auto:ip&aqi=no"),
               _apiKey);
  }
  DEBUG_PRINTF("WeatherApi: fetching %s\n", url);
  strncpy(_lastUrl, url, sizeof(_lastUrl) - 1);
  _lastUrl[sizeof(_lastUrl) - 1] = '\0';

  // Build filter from all registered subscription paths.
  // Only the subscribed fields will be retained in the parsed document.
  DynamicJsonDocument filterDoc(WEATHER_API_FILTER_DOC_SIZE);
  JsonObject filterRoot = filterDoc.to<JsonObject>();
  for (uint8_t i = 0; i < _subCount; i++) {
    _setFilterPath(filterRoot, _subs[i].path);
  }

  WiFiClient wifiClient;
  HTTPClient http;
  if (!http.begin(wifiClient, url)) {
    _lastError   = "HTTP begin failed";
    _lastFetchOk = false;
    DEBUG_PRINTLN(F("WeatherApi: HTTP begin failed"));
    return;
  }
  // 5 s is generous for WeatherAPI and well within the HW watchdog window (~8 s on ESP8266)
  http.setTimeout(1000);
  int httpCode = http.GET();
  DEBUG_PRINTF("WeatherApi: HTTP %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    char buf[24];
    snprintf(buf, sizeof(buf), "HTTP %d", httpCode);
    _lastError   = buf;
    _lastFetchOk = false;
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  // Parse with filter — only subscribed fields are kept, minimising heap use
  DynamicJsonDocument doc(WEATHER_API_RESPONSE_DOC_SIZE);
  DeserializationError err = deserializeJson(doc, body,
                                             DeserializationOption::Filter(filterDoc));
  if (err) {
    _lastError   = err.c_str();
    _lastFetchOk = false;
    DEBUG_PRINTF("WeatherApi: JSON parse error: %s\n", err.c_str());
    return;
  }

  // Dispatch each subscription: navigate to the subscribed path and invoke the callback.
  // The JsonVariant is valid only for the lifetime of doc; subscribers must copy values out.
  JsonVariant docVar = doc.as<JsonVariant>();
  for (uint8_t i = 0; i < _subCount; i++) {
    JsonVariant val = _getByPath(docVar, _subs[i].path);
    if (!val.isNull() && _subs[i].callback) {
      _subs[i].callback(val);
    } else if (val.isNull()) {
      DEBUG_PRINTF("WeatherApi: path not found in response: %s\n", _subs[i].path);
    }
  }

  _lastFetchOk   = true;
  _lastFetchTime = localTime;
  _lastError     = "";
  DEBUG_PRINTF("WeatherApi: fetch OK, dispatched to %d subscriber(s)\n", _subCount);
}

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------

bool WeatherApiUsermod::subscribe(const char* path, WeatherValueCallback callback) {
  if (_subCount >= WEATHER_API_MAX_SUBS) {
    DEBUG_PRINTLN(F("WeatherApi: subscription table full"));
    return false;
  }
  _subs[_subCount].path     = path;
  _subs[_subCount].callback = callback;
  _subCount++;
  DEBUG_PRINTF("WeatherApi: registered subscription #%d for '%s'\n", _subCount, path);
  return true;
}

void WeatherApiUsermod::triggerFetch() {
  _fetchPending = true;
}

void WeatherApiUsermod::setup() {
  _fetchPending = true;  // fetch as soon as WiFi connects
}

void WeatherApiUsermod::loop() {
  if (!_enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Sync with global WLED latitude/longitude if available.
  // This allows the weather API to automatically use the location set in WLED's time settings page.
  // If user configured lat/lon in weather API settings, only override if global values are non-zero
  // and different from cached values (to avoid overwriting user config with 0,0).
  if (::latitude != 0.0f && ::latitude != _latitude) {
    _latitude = ::latitude;
  }
  if (::longitude != 0.0f && ::longitude != _longitude) {
    _longitude = ::longitude;
  }

  unsigned long now = millis();
  unsigned long intervalMs = (unsigned long)_intervalSec * 1000UL;
  bool due = _fetchPending || (now - _lastFetch >= intervalMs);
  if (!due) return;

  _fetchPending = false;
  _lastFetch    = now;
  yield();   // feed SW watchdog before the blocking HTTP call
  _doFetch();
}

void WeatherApiUsermod::connected() {
  _fetchPending = true;
}

void WeatherApiUsermod::addToJsonInfo(JsonObject& root) {
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");
  JsonArray arr = user.createNestedArray(FPSTR(_name));

  if (!_enabled) {
    arr.add(F("Disabled"));
    return;
  }
  if (_lastFetchOk) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Last fetch: %02d:%02d",
             hour(_lastFetchTime), minute(_lastFetchTime));
    arr.add(buf);
    char sBuf[28];
    snprintf(sBuf, sizeof(sBuf), "Subscribers: %d / %d", _subCount, WEATHER_API_MAX_SUBS);
    arr.add(sBuf);
  } else if (_lastError.length() > 0) {
    String msg = F("Error: ");
    msg += _lastError;
    arr.add(msg);
  } else if (WiFi.status() == WL_CONNECTED) {
    arr.add(F("Waiting for first fetch..."));
  } else {
    arr.add(F("No WiFi"));
  }
  // Expose current URL with a "url:" prefix so the settings JS can render it as a link.
  // Shown whenever a URL has been built, regardless of fetch success.
  if (_lastUrl[0] != '\0') {
    String urlEntry = F("url:");
    urlEntry += _lastUrl;
    arr.add(urlEntry);
  }
}

void WeatherApiUsermod::addToConfig(JsonObject& root) {
  JsonObject top = root.createNestedObject(FPSTR(_name));
  top[FPSTR(_keyEnabled)]  = _enabled;
  top[FPSTR(_keyApiKey)]   = _apiKey;
  top[FPSTR(_keyInterval)] = _intervalSec;
}

bool WeatherApiUsermod::readFromConfig(JsonObject& root) {
  JsonObject top = root[FPSTR(_name)];
  if (top.isNull()) return false;

  bool ok = true;
  ok &= getJsonValue(top[FPSTR(_keyEnabled)],  _enabled);

  const char* storedKey = top[FPSTR(_keyApiKey)] | "";
  strncpy(_apiKey, storedKey, sizeof(_apiKey) - 1);
  _apiKey[sizeof(_apiKey) - 1] = '\0';
  ok &= getJsonValue(top[FPSTR(_keyInterval)], _intervalSec);

  // Enforce a minimum interval to avoid hammering the free-tier API quota
  if (_intervalSec < 30) _intervalSec = 30;

  DEBUG_PRINTF("WeatherApi: config loaded — enabled=%d interval=%us key=%s\n",
               _enabled, _intervalSec, strlen(_apiKey) > 0 ? "(set)" : "(empty)");
  return ok;
}

void WeatherApiUsermod::appendConfigData() {
  // s.js?p=8 is a single shared response for all usermods — keep output minimal.
  // Short hint strings; avoid redundant platform-specific guidance here.
  oappend(SET_F("addInfo('WeatherApi:api-key',1,'Free key at weatherapi.com');"));
  oappend(SET_F("addInfo('WeatherApi:interval',1,'Seconds between fetches (min 30)');"));
  // Status panel: on-demand <details> toggle; url: entries rendered as links; arrow functions throughout.
}
// ---------------------------------------------------------------------------
// Usermod registration
// ---------------------------------------------------------------------------
static WeatherApiUsermod weatherApiUsermod;
REGISTER_USERMOD(weatherApiUsermod);

#endif // USERMOD_WEATHER_API
