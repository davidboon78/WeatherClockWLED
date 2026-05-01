# WeatherApi Usermod

Fetches current weather from [WeatherAPI.com](https://www.weatherapi.com/) and dispatches
individual JSON field values to any usermod that subscribes. Only subscribed fields are
retained after parsing — an ArduinoJSON filter is built dynamically from the registered
subscription paths, so unused data never touches RAM.

## Features

- **Subscription-based API** — register any dot-notation JSON path (e.g. `current.temp_c`,
  `current.condition.code`, `location.name`) from any usermod's `setup()`.
- **Memory efficient** — ArduinoJSON filter ensures only subscribed fields are parsed and kept.
- **Configurable interval** — default 60 s, minimum 30 s (free tier allows 1 M req/month).
- **Location** — uses WLED Time settings lat/lon, or auto-detects via IP when both are 0.
- **API key never in git** — options below keep the key out of source control.

## API Key Setup (choose one)

### Option 1 — WLED Settings UI (recommended, no recompile)
Enter the key in the **WeatherApi** section of the WLED usermod settings page.
It is stored in `cfg.json` on the device filesystem, which is not part of this repository.

### Option 2 — `platformio_override.ini` (compile-time, already gitignored)
Add to the `[env]` section of your `platformio_override.ini`:
```ini
[env]
build_flags = ${common.build_flags}
              -D WEATHERAPI_KEY='"your-key-here"'
```

## Enabling the Usermod

Add `weather_api` to `custom_usermods` in your `platformio_override.ini`:
```ini
[env:esp32dev]
custom_usermods = weather_api
                  TM1637_Clock
                  ...
```

## Using From Another Usermod

```cpp
#ifdef USERMOD_WEATHER_API
#include "../weather_api/usermod_weather_api.h"
#endif

class MyUsermod : public Usermod {
  // Static storage for the callback values (callbacks cannot capture 'this')
  static float s_tempC;
  static int   s_condCode;

  static void onTempC(JsonVariant v)    { s_tempC    = v.as<float>(); }
  static void onCondCode(JsonVariant v) { s_condCode = v.as<int>();   }

public:
  void setup() override {
#ifdef USERMOD_WEATHER_API
    auto* wapi = (WeatherApiUsermod*)UsermodManager::lookup(USERMOD_ID_WEATHER_API);
    if (wapi) {
      wapi->subscribe("current.temp_c",         onTempC);
      wapi->subscribe("current.condition.code", onCondCode);
    }
#endif
  }

  void loop() override {
    // s_tempC and s_condCode are updated automatically after each fetch
  }
};

float MyUsermod::s_tempC    = 0.0f;
int   MyUsermod::s_condCode = 1000;
```

The `WeatherValueCallback` signature is `void callback(JsonVariant value)`.
The `JsonVariant` is only valid for the duration of the callback — copy the value
out immediately (`.as<float>()`, `.as<int>()`, `.as<const char*>()`, etc.).

## Subscribable WeatherAPI Fields (examples)

| Path | Type | Description |
|------|------|-------------|
| `current.temp_c` | float | Temperature in Celsius |
| `current.temp_f` | float | Temperature in Fahrenheit |
| `current.feelslike_c` | float | Feels-like temperature (°C) |
| `current.humidity` | int | Relative humidity % |
| `current.wind_kph` | float | Wind speed (km/h) |
| `current.condition.code` | int | WeatherAPI condition code (1000–1282) |
| `current.condition.text` | string | Human-readable condition |
| `current.is_day` | int | 1 = daytime, 0 = night |
| `current.precip_mm` | float | Precipitation (mm) |
| `current.cloud` | int | Cloud cover % |
| `location.name` | string | City name |
| `location.country` | string | Country |

Full field reference: https://www.weatherapi.com/docs/

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Enabled | true | Enable/disable the usermod |
| API key | (empty) | WeatherAPI.com free key |
| Latitude | 0 | Decimal degrees; 0 = auto via IP |
| Longitude | 0 | Decimal degrees; 0 = auto via IP |
| Interval | 60 | Fetch interval in seconds (min 30) |

## Build-time Tuning

Override in `platformio_override.ini` `build_flags`:

| Define | Default | Purpose |
|--------|---------|---------|
| `WEATHER_API_MAX_SUBS` | 12 | Maximum simultaneous subscriptions |
| `WEATHER_API_FILTER_DOC_SIZE` | 512 | ArduinoJSON filter document capacity (bytes) |
| `WEATHER_API_RESPONSE_DOC_SIZE` | 256 | Parsed response document capacity (bytes) |
| `WEATHER_API_FETCH_INTERVAL_MS` | 60000 | Default fetch interval (ms) |

## Usermod ID

`USERMOD_ID_WEATHER_API = 60` (defined in `wled00/const.h`).
