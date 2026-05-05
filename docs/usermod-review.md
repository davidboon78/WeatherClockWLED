# Usermod System Code Review & Refactoring Guide

**Files reviewed:**
- `usermods/weather_api/usermod_weather_api.h` / `.cpp`
- `usermods/weather_api/weather_light_patterns.h`
- `usermods/tm1637_Display/usermod_tm1637_display.h` / `.cpp`
- `usermods/TM1637_Clock/usermod_tm1637_clock.h` / `.cpp`
- `usermods/baseball_api/usermod_baseball_api.h` / `.cpp`

**Reference docs:**
- [WLED Usermod API](https://kno.wled.ge/advanced/usermod-api/)
- [ArduinoJSON v6 Filter](https://arduinojson.org/v6/api/json/deserializejson/)
- [WLED PinManager](https://github.com/wled/WLED/blob/main/wled00/pin_manager.h)
- [One Definition Rule (ODR) — cppreference](https://en.cppreference.com/w/cpp/language/definition)
- [SOLID Principles overview](https://en.wikipedia.org/wiki/SOLID)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│  WeatherApiUsermod  (fetch & pub/sub hub)                   │
│   subscribe("current.temp_c",         cb)                   │
│   subscribe("current.condition.code", cb)                   │
└──────────┬──────────────────────┬──────────────────────────┘
           │ 2 subscriptions      │ 2 subscriptions
           ▼                      ▼
┌──────────────────┐   ┌──────────────────────────────────────┐
│ TM1637Display    │   │ TM1637Clock                          │
│ (hardware driver)│   │ (LED pattern / palette selector)     │
│  - time on HW    │   │  - applyWeatherLightPattern()        │
│  - temp on HW    │   │  - reads BaseballAPI for score       │
│  - condition HW  │   │  - calls tm1637DisplayShowMessage()  │
└──────────────────┘   └─────────────────┬────────────────────┘
                                         │ lookup + cast
                                         ▼
                              ┌──────────────────────┐
                              │ UsermodBaseballAPI   │
                              │  - MLB schedule poll │
                              │  - team palette      │
                              │  - light override    │
                              └──────────────────────┘
```

**Key design decisions (intentional):**
- `WeatherApiUsermod` is a pure data provider; it knows nothing about display or LEDs.
- `TM1637Display` owns all TM1637 hardware I/O.
- `TM1637Clock` owns WLED LED state changes and baseball score display routing.
- Usermods locate each other via `UsermodManager::lookup()`.

---

## 2. Critical Bugs

### BUG-1 — ODR Violation / Memory Layout Mismatch in `UsermodBaseballAPI` 🔴

**Files:** `usermod_baseball_api.h` lines 6–73, `usermod_baseball_api.cpp` lines 11–50

**Severity:** High — causes wrong field reads / potential memory corruption at runtime.

**Problem:** `UsermodBaseballAPI` is fully defined twice with different member lists:

| Offset (approx.) | `.h` layout | `.cpp` layout |
|---|---|---|
| after `intervalMs` | `String favoriteTeam` | `bool wasConnected` ← **EXTRA** |
| after `gameLive` | `String lastScore` | `int liveGamePk` ← **EXTRA** |
| after `lastRequestUrl` | `String lastFetchError` | `String lastLiveDataUrl` ← **EXTRA** |

When `TM1637ClockUsermod` includes the `.h` and then casts the `UsermodManager::lookup()` result:

```cpp
// usermod_tm1637_clock.cpp line 295
baseballApi = (UsermodBaseballAPI*)UsermodManager::lookup(USERMOD_ID_BASEBALL_API);
```

…the inline accessors compiled by `TM1637ClockUsermod`'s translation unit use `.h`-layout offsets on a `.cpp`-layout object. Calling `baseballApi->isGameLive()`, `getLastScore()`, `getFavoriteTeam()`, or `isGameOverrideActive()` reads from wrong memory addresses.

**Fix:**

**Step 1.** Move the complete class definition out of `usermod_baseball_api.cpp` and into `usermod_baseball_api.h`. The `.h` already has a partial definition — merge the private members and methods from the `.cpp` class into the `.h` class.

**Step 2.** At the top of `usermod_baseball_api.cpp`, `#include` the header to use its class:

```cpp
// usermod_baseball_api.cpp — add after line 2
#include "usermod_baseball_api.h"
```

**Step 3.** Remove the entire class re-definition block from `usermod_baseball_api.cpp` (the `class UsermodBaseballAPI { … };` block) and replace it with out-of-line method implementations.

The resulting `.h` should contain all member declarations (public + private), all inline getters, and the `TeamMap` struct. The `.cpp` contains only method bodies.

**Reference:** [C++ ODR — cppreference](https://en.cppreference.com/w/cpp/language/definition#One_definition_rule)

---

### BUG-2 — `toki.getTime().sec == 0` Incorrect NTP Check 🔴

**File:** `usermod_tm1637_display.cpp`  
**Lines:** ~374 (in `updateDisplayState()`), ~400 (in `showTime()`)

**Problem:** `.sec` is the *seconds-within-the-minute* field (0–59). It equals `0` for one full second at the top of every minute even when NTP is perfectly synced. The display will show `"ntP."` for one second every minute.

```cpp
// BUGGY — usermod_tm1637_display.cpp ~line 374
} else if (toki.getTime().sec == 0) {
    newState = NO_NTP;  // Time not yet synced  ← WRONG
}

// BUGGY — showTime() ~line 400
if (toki.getTime().sec != 0) {
    // show time
} else {
    const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};  // "----"  ← shows every minute
    display->setSegments(dashes);
}
```

**Fix — `updateDisplayState()`:**

```cpp
// Replace the toki.getTime().sec == 0 branch with:
} else {
    updateLocalTime();
    bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);
    newState = timeValid ? SHOW_TIME : NO_NTP;
}
```

**Fix — `showTime()`:**

```cpp
// Replace the toki.getTime().sec != 0 guard with:
updateLocalTime();
bool timeValid = (localTime >= 1704067200UL && localTime < 4102444800UL);
if (timeValid) {
    int currentHour   = hour(localTime);
    int currentMinute = minute(localTime);
    // ... existing display code ...
} else {
    const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
    display->setSegments(dashes);
}
```

---

### BUG-3 — Double Assignment of `lastFetch` in `readFromConfig` 🟡

**File:** `usermod_baseball_api.cpp`  
**Lines:** ~785 and ~800 (inside the `if (favoriteTeam != previousFavoriteTeam)` block)

```cpp
// First assignment:
lastFetch = millis() - intervalMs;
DEBUG_PRINTF("BaseballAPI: team changed from '%s' to '%s'\n", ...);

// Dead code — same assignment again:
lastFetch = millis() - intervalMs;   // ← remove this second one
```

The second assignment is unreachable-effect dead code. Remove it.

---

### BUG-4 — `savedMode/Palette/Speed/Intensity` Uninitialized in `.cpp` 🟡

**File:** `usermod_baseball_api.cpp` line ~38

```cpp
// BUGGY — no initializers in .cpp class definition:
uint8_t savedMode, savedPalette, savedSpeed, savedIntensity;
```

The `.h` version has safe defaults (`savedSpeed = 128`, `savedIntensity = 128`) but since the full class lives in the `.cpp` (see BUG-1), those initializers are irrelevant. If `_restoreGameOverride()` is ever called with `gameOverrideActive == true` but before `_applyGameOverride()` saved the values (possible through direct JSON manipulation), these unitialized values get applied to the strip.

**Fix:** Add zero-initializers:
```cpp
uint8_t savedMode = 0, savedPalette = 0, savedSpeed = 128, savedIntensity = 128;
```

---

### BUG-5 — `weatherFetchFailed` Is Never Set to `true` 🟡

**File:** `usermod_tm1637_display.cpp`

The flag `weatherFetchFailed` is declared at ~line 68, initialized to `false`, and set to `false` again in `_onCondCode`. It is **never** set to `true` anywhere in the file. All code checking it (`showTemperature()` ~line 439, `addToJsonInfo()` ~line 558) effectively checks only `!weatherFetched`.

**Fix:** Either:
- Remove the field and simplify all checks to `if (!weatherFetched)`, **or**
- Add a callback for the WeatherAPI error case (requires extending the WeatherAPI's subscription interface to notify on failure).

---

## 3. DRY Violations

### DRY-1 — `conditionSeverity()` Triplicated 🔴

**Files:**
- `usermod_tm1637_display.cpp` lines ~122–144 (23 lines)
- `usermod_tm1637_clock.cpp` lines ~179–203 (25 lines)
- (`weather_light_patterns.h` duplicates the same category logic inline)

The function body is byte-for-byte identical. Any correction (e.g., changing a severity rating for a condition code) must be made in three places.

**Fix:** Add to `weather_light_patterns.h` (after the palette constant block, before section 8):

```cpp
// ---------------------------------------------------------------------------
// 7b. Condition severity: 1=mild, 2=moderate, 3=severe
// ---------------------------------------------------------------------------
inline uint8_t conditionSeverity(int code) {
  if (code == 1000) return 1;
  if (code == 1003) return 1; if (code == 1006) return 2; if (code == 1009) return 3;
  if (code == 1030) return 1; if (code == 1135) return 2; if (code == 1147) return 3;
  if (code == 1087) return 1;
  if (code == 1273 || code == 1279) return 2;
  if (code == 1276 || code == 1282) return 3;
  if (code == 1210 || code == 1213 || code == 1255) return 1;
  if (code == 1114 || code == 1216 || code == 1219 || code == 1237 ||
      code == 1258 || code == 1261) return 2;
  if (code == 1117 || code == 1222 || code == 1225 || code == 1264) return 3;
  if (code == 1063 || code == 1069 || code == 1072 || code == 1150 ||
      code == 1153 || code == 1180 || code == 1183 || code == 1198 ||
      code == 1204 || code == 1240 || code == 1249) return 1;
  if (code == 1186 || code == 1189 || code == 1201 || code == 1207 ||
      code == 1243 || code == 1252) return 2;
  if (code == 1171 || code == 1192 || code == 1195 || code == 1246) return 3;
  return 1;
}
```

Then delete the private `static uint8_t conditionSeverity(int code)` from both `TM1637DisplayUsermod` and `TM1637ClockUsermod`.

---

### DRY-2 — `conditionDescription()` Triplicated 🔴

**Files:**
- `usermod_tm1637_display.cpp` lines ~148–202
- `usermod_tm1637_clock.cpp` lines ~215–272

Both are identical 49-case switch statements. The same vulnerability as DRY-1 — a typo fix or new code addition must be done twice.

**Fix:** Add to `weather_light_patterns.h` (alongside `conditionSeverity`):

```cpp
// ---------------------------------------------------------------------------
// 7c. Human-readable condition description (all 49 WeatherAPI codes)
// ---------------------------------------------------------------------------
inline const char* conditionDescription(int code) {
  switch (code) {
    case 1000: return "Clear";
    case 1003: return "Partly cloudy";
    // ... (full 49-entry switch) ...
    default:   return "Unknown";
  }
}
```

Delete from both usermod files. Since `weather_light_patterns.h` is already included by both, no additional include is needed.

---

### DRY-3 — `conditionToCategory()` Not Shared

**File:** `usermod_tm1637_clock.cpp` lines ~206–213

This mapping function only exists in `TM1637ClockUsermod`. The same categorization logic is used implicitly in `showConditionText()` in `TM1637DisplayUsermod` (via a separate parallel switch-case structure). Add this alongside `conditionSeverity` in `weather_light_patterns.h`:

```cpp
// Returns a category index: 0=Clear, 1=Cloudy, 2=Fog, 3=Thunder, 4=Snow, 5=Rain
inline uint8_t conditionToCategory(int code) {
  if (code == 1000) return 0;
  if (code <= 1009) return 1;
  if (code == 1030 || code == 1135 || code == 1147) return 2;
  if (code == 1087 || code >= 1273) return 3;
  if ((code >= 1114 && code <= 1117) || (code >= 1210 && code <= 1264)) return 4;
  return 5;
}
```

Then delete the local copy from `TM1637ClockUsermod`.

> **Note:** `conditionToCategory()` incorrectly classifies code `1066` (Patchy snow possible) as Rain (5) because it falls through to the default. Consider adding `code == 1066` to the Snow/Ice branch.

---

### DRY-4 — Score String Parsed in the Wrong Usermod

**File:** `usermod_tm1637_clock.cpp` lines ~88–150 (`parseBaseballScore`)

`UsermodBaseballAPI::_parseMLB()` builds `lastScore` as a formatted string. `TM1637ClockUsermod::parseBaseballScore()` then reverse-engineers that string to extract teams and scores. This is fragile bidirectional string manipulation — if `_parseMLB` changes its format, `parseBaseballScore` silently breaks.

**Fix (Step-by-step):**

**Step 1.** Add structured accessors to `UsermodBaseballAPI`:

```cpp
// Add to usermod_baseball_api.h public section:
int   getHomeScore()  const { return _homeScore; }
int   getAwayScore()  const { return _awayScore; }
const String& getHomeTeam() const { return _homeTeam; }
const String& getAwayTeam() const { return _awayTeam; }
```

**Step 2.** Add private storage fields in `UsermodBaseballAPI`:

```cpp
// Add to private section:
String _homeTeam = "";
String _awayTeam = "";
int    _homeScore = -1;
int    _awayScore = -1;
```

**Step 3.** In `_parseMLB()`, populate them at the same point `lastScore` is set (baseball_api.cpp ~line 568):

```cpp
// Replace the lastScore = ... line with:
_awayTeam  = away;  _awayScore = aScore;
_homeTeam  = home;  _homeScore = hScore;
lastScore  = away + " " + String(aScore) + " @ " + home + " " + String(hScore);
```

**Step 4.** Replace `parseBaseballScore()` + `showBaseballScore()` in `TM1637ClockUsermod`:

```cpp
// Replace the entire parseBaseballScore + showBaseballScore dance with:
if (baseballApi->isGameLive()) {
    const String& favTeam = baseballApi->getFavoriteTeam();
    bool favIsHome = baseballApi->getHomeTeam().equalsIgnoreCase(favTeam);
    int favScore = favIsHome ? baseballApi->getHomeScore() : baseballApi->getAwayScore();
    int oppScore = favIsHome ? baseballApi->getAwayScore() : baseballApi->getHomeScore();
    char buf[5];
    snprintf(buf, sizeof(buf), "%u-%u",
             (unsigned)(favScore < 0 ? 0 : favScore) % 10,
             (unsigned)(oppScore < 0 ? 0 : oppScore) % 10);
    tm1637DisplayShowMessage(buf, 3000);
}
```

---

## 4. SOLID Violations

### S-1 (Single Responsibility) — `TM1637ClockUsermod` Has Four Responsibilities

**File:** `usermod_tm1637_clock.cpp`

This class handles:
1. LED pattern / palette selection based on weather
2. Display message routing to TM1637 hardware (via `tm1637DisplayShowMessage`)
3. Baseball score display scheduling state machine
4. Baseball score string parsing (`parseBaseballScore`)

Responsibilities 3 and 4 belong in the baseball API (data source) or in a thin coordinator. After implementing DRY-4 above, the `parseBaseballScore` method is eliminated. The score display scheduling (the `baseballShown` / `lastWeatherCycleEnd` state machine) could optionally move to `UsermodBaseballAPI`, but the cross-module display call makes it easier to keep in `TM1637ClockUsermod` as a coordinator — just ensure it delegates to properly abstracted interfaces.

---

### S-2 (Open/Closed) — Hard-Coded 6-Category Condition Array

**File:** `usermod_tm1637_clock.cpp` lines ~44–47

```cpp
uint8_t conditionPresetOverrides[6] = {0, 0, 0, 0, 0, 0};
```

Adding a 7th weather category (e.g., separating "Sleet" from "Rain") requires:
1. Changing the array size
2. Updating `conditionToCategory()` 
3. Updating `addToConfig` / `readFromConfig`
4. Adding a new PROGMEM key
5. Adding a new `appendConfigData` entry

This is acceptable for a constrained embedded system where the category list is stable. However, document the coupling clearly in comments.

---

### D-1 (Dependency Inversion) — Hard Dependency on Concrete Baseball Class

**File:** `usermod_tm1637_clock.cpp` line 28

```cpp
#include "../baseball_api/usermod_baseball_api.h"
```

This include is **not guarded by `#ifdef USERMOD_BASEBALL_API`**. Any build without the baseball API usermod will fail to compile because the header exposes `UsermodBaseballAPI` which won't exist.

**Fix:** Wrap the include and the `baseballApi` member usage:

```cpp
// usermod_tm1637_clock.cpp line 28 — add guard:
#ifdef USERMOD_BASEBALL_API
  #include "../baseball_api/usermod_baseball_api.h"
#endif
```

And guard the member declaration:
```cpp
// In TM1637ClockUsermod private section:
#ifdef USERMOD_BASEBALL_API
    UsermodBaseballAPI* baseballApi = nullptr;
#endif
```

And guard all uses of `baseballApi` in `setup()`, `loop()`, and `applyWeatherLightPattern()`. Most of these already have `#ifdef USERMOD_BASEBALL_API` guards; the missing ones are the member declaration and the include.

---

### L-1 (Liskov) — `isEnabled()` Missing `const` in `TM1637ClockUsermod`

**File:** `usermod_tm1637_clock.cpp` last lines of class

```cpp
// TM1637Clock — missing const:
bool isEnabled() { return enabled; }

// TM1637Display — correct:
bool isEnabled() const { return enabled; }
```

**Fix:**
```cpp
bool isEnabled() const { return enabled; }
```

---

## 5. WLED Standards Violations

### W-1 — `_keyLat` / `_keyLon` Declared but Never Defined or Used

**File:** `usermod_weather_api.h` lines (in class declaration)

```cpp
static const char _keyLat[];
static const char _keyLon[];
```

These are declared in the class but **not defined** in `usermod_weather_api.cpp` and **not used** in `addToConfig` / `readFromConfig`. The actual lat/lon sync uses the global `::latitude` / `::longitude` WLED variables directly (in `loop()`), with no user-configurable override stored in config.

**Options:**
- If lat/lon config UI override is intended: define and use these keys in `addToConfig`/`readFromConfig`.
- If not intended: **remove the declarations** from the header to avoid confusion.

---

### W-2 — `PinOwner::UM_Unspecified` for TM1637 Pins

**File:** `usermod_tm1637_display.cpp` lines ~221–233

```cpp
PinManager::allocatePin(clkPin, true, PinOwner::UM_Unspecified)
PinManager::deallocatePin(clkPin, PinOwner::UM_Unspecified)
```

The WLED convention is to use a named `PinOwner` value so the info page and pin manager can identify what owns each pin.

**Fix — `wled00/const.h`:** Add adjacent to the other TM1637 IDs (after line 223):

```cpp
// In PinOwner enum (find it in pin_manager.h):
UM_TM1637Display,   // TM1637 4-digit display (CLK + DIO)
```

Then update both `allocatePin` and `deallocatePin` calls to use `PinOwner::UM_TM1637Display`.

---

### W-3 — Display-Only Fields Written into `addToConfig`

**File:** `usermod_baseball_api.cpp` lines ~758–763

```cpp
void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabledKey)]  = enabled;
    top[FPSTR(_teamKey)]     = favoriteTeam;
    top[FPSTR(_overrideKey)] = overrideLightsOnGame;
    top[FPSTR(_paletteKey)]  = _buildPaletteDisplay();  // ← computed display string
    top[FPSTR(_statusKey)]   = _buildStatusValue();     // ← computed HTML string
}
```

`_buildPaletteDisplay()` and `_buildStatusValue()` are UI-only computed strings. They are not read back by `readFromConfig`, yet they are serialised to flash (`cfg.json`) and returned on every config page load. This wastes flash writes and config payload size.

**Fix:** Remove `_paletteKey` and `_statusKey` from `addToConfig`. Move their display to `addToJsonInfo` only:

```cpp
void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabledKey)]  = enabled;
    top[FPSTR(_teamKey)]     = favoriteTeam;
    top[FPSTR(_overrideKey)] = overrideLightsOnGame;
    // _paletteKey and _statusKey removed — display-only, use addToJsonInfo
}
```

The `appendConfigData()` JS that renders the read-only palette and status fields must be updated to fetch from `/json/info` instead, or the fields must be removed from the settings UI entirely if `addToConfig` no longer provides them. The simplest approach is to keep the `appendConfigData` JS fields but have them populated from the info endpoint rather than the config endpoint.

---

### W-4 — `colorUpdated()` Not Called After Weather Pattern Apply

**File:** `usermod_tm1637_clock.cpp` — `applyWeatherLightPattern()` last lines

```cpp
// Current — bypasses normal WLED state notification:
stateChanged = true;
strip.trigger();
```

The idiomatic WLED pattern after modifying `effectCurrent`, `effectPalette`, etc. is:

```cpp
colorUpdated(CALL_MODE_DIRECT_CHANGE);
```

`colorUpdated()` internally sets `stateChanged`, applies values to selected segments, and triggers a strip update. Using bare `stateChanged = true; strip.trigger()` can leave the UI out of sync (e.g. the web interface slider positions don't update).

Note: `BaseballAPI::_applyGameOverride()` correctly uses `colorUpdated(CALL_MODE_DIRECT_CHANGE)`. The `TM1637ClockUsermod` should match it.

**Fix:**
```cpp
// Replace at end of applyWeatherLightPattern():
// stateChanged = true;
// strip.trigger();
colorUpdated(CALL_MODE_DIRECT_CHANGE);
```

---

### W-5 — `_ensureTeamPalette()` Called on Every `loop()` Tick

**File:** `usermod_baseball_api.cpp` — `loop()` lines ~668–670

```cpp
void loop() override {
    if (!enabled || strip.isUpdating()) return;

    _ensureTeamPalette();   // ← runs every frame (30+ times/sec)
```

`_ensureTeamPalette()` iterates all 30 entries of `mlbMap` on every call, plus checks `customPalettes.size()`. Even if the palette is already built, the team lookup runs every frame.

**Fix:** Add a dirty flag:

```cpp
// Add private member:
bool _teamPaletteDirty = true;

// _ensureTeamPalette() — add early exit:
void _ensureTeamPalette() {
    if (!_teamPaletteDirty) return;
    // ... existing code ...
    _teamPaletteDirty = false;
}

// Set dirty when team changes (in readFromConfig team-change block):
_teamPaletteDirty = true;

// Set dirty when palette is removed:
void _removeTeamPalette() {
    // ... existing code ...
    _teamPaletteDirty = true;
}
```

---

### W-6 — Baseball API HTTP Timeout Too High for ESP8266 Loop

**File:** `usermod_baseball_api.cpp` line ~425

```cpp
http.setTimeout(7000);
```

The ESP8266 software WDT window is ~8 seconds. With a 7-second HTTP timeout plus DNS resolution plus the JSON parse of a 6144-byte document, the main loop is blocked for up to 8+ seconds. This risks:
- WDT resets on ESP8266
- Visible LED freeze (no strip updates)
- TM1637 display freeze

`WeatherApiUsermod` uses a more conservative 5-second timeout and calls `yield()` before the fetch.

**Fix:** Reduce to 5000ms and add a `yield()` call before the HTTP request, matching the WeatherAPI pattern:

```cpp
// Add before http.begin():
yield();
http.setTimeout(5000);
```

---

### W-7 — `String` Heap Allocations in `addToJsonInfo` Hot Path

**File:** `usermod_baseball_api.cpp` — `addToJsonInfo()` lines ~726–730

```cpp
void addToJsonInfo(JsonObject& root) override {
    _ensureTeamPalette();
    // ...
    String apiUrl = _buildScheduleUrl(mlbId);   // ← builds URL every /json/info call
    String statusLine = _buildStatusLine();      // ← another String allocation
```

`/json/info` is called frequently (every time the WLED info page is open, every UI refresh). `_buildScheduleUrl()` does multiple `String` concatenations. On a constrained heap, this causes fragmentation.

**Fix:** Cache the schedule URL in a member variable and only rebuild it when the team changes:

```cpp
// Add private member:
String _cachedScheduleUrl = "";

// In readFromConfig team-change block:
_cachedScheduleUrl = "";  // invalidate

// In _doFetch() — already calls _buildScheduleUrl, cache the result:
_cachedScheduleUrl = url;

// In addToJsonInfo(), replace _buildScheduleUrl(mlbId) with:
const String& apiUrl = _cachedScheduleUrl;
```

---

## 6. Code Quality Issues

### Q-1 — Commented-Out Debug Line

**File:** `usermod_tm1637_clock.cpp` in `loop()` (near the `else` for `!gameLive`)

```cpp
}else{
  
 // DEBUG_PRINTLN(F("TM1637 Clock: MLB no live game, skipping baseball score display") );
}
```

Remove the commented-out line and fix the brace/spacing style to match WLED conventions:

```cpp
} else {
    // no live game — nothing to display
}
```

---

### Q-2 — Missing Spaces After `if` Keywords

**File:** `usermod_tm1637_clock.cpp` lines ~355, ~358

```cpp
if(baseballApi){
    if( baseballApi->isGameLive()){
```

WLED style requires a space after `if`:
```cpp
if (baseballApi) {
    if (baseballApi->isGameLive()) {
```

---

### Q-3 — Verbose Gate Logging When `baseballApi == nullptr`

**File:** `usermod_tm1637_clock.cpp` — `loop()` near end

```cpp
} else {
    if (nowMs - lastGateLogMs > 5000) {
        DEBUG_PRINTLN(F("TM1637 Clock: MLB gate blocked (Baseball API usermod lookup returned null)"));
        lastGateLogMs = nowMs;
    }
}
```

When `USERMOD_BASEBALL_API` is in the build but the module simply isn't initialised yet, this logs a "blocked" message every 5 seconds indefinitely. It's also misleading — `nullptr` is the expected result in builds without the baseball mod. After implementing the `#ifdef USERMOD_BASEBALL_API` guard (see D-1), this else branch should only exist inside the ifdef guard, and the log should fire at most once:

```cpp
#ifdef USERMOD_BASEBALL_API
if (!baseballApi) {
    DEBUG_PRINTLN(F("TM1637 Clock: Baseball API not available"));
    // no need to spam; this is a one-time startup condition
}
#endif
```

---

### Q-4 — `_buildStatusValue()` Parses Its Own Constructed String

**File:** `usermod_baseball_api.cpp` lines ~335–380

`_buildStatusValue()` extracts data from `nextGameInfo` — a string that was constructed by `_parseMLB()` from structured fields. This pattern (serialize → deserialize in the same class) is fragile. If the format of `nextGameInfo` changes even slightly, `_buildStatusValue()` silently returns `"Unavailable"`.

**Fix:** After implementing DRY-4 (structured accessors), store the home/away team and game date as separate private fields and use them directly in `_buildStatusValue()`:

```cpp
// Add private fields:
String _upcomingHomeTeam = "";
String _upcomingAwayTeam = "";
String _upcomingGameDateLocal = "";

// Build the display string directly from these fields:
String _buildStatusValue() const {
    if (gameLive && lastScore.length() > 0) return lastScore;
    if (_upcomingHomeTeam.length() > 0) {
        // determine which side is favorite...
        return favoriteTeamName + " vs " + opponent + "<br/>" + _upcomingGameDateLocal;
    }
    return "Unavailable";
}
```

---

### Q-5 — Magic Numbers in `_isGameHappeningNow()`

**File:** `usermod_baseball_api.cpp` lines ~253–256

```cpp
return gameUtcTime >= (nowUtc - 1800) && gameUtcTime <= (nowUtc + 21600);
```

These should be named constants:

```cpp
static constexpr int32_t GAME_WINDOW_PRE_SEC  = 1800;    // 30 min before first pitch
static constexpr int32_t GAME_WINDOW_POST_SEC = 21600;   // 6 hours after (covers extra innings)

return gameUtcTime >= (nowUtc - GAME_WINDOW_PRE_SEC) &&
       gameUtcTime <= (nowUtc + GAME_WINDOW_POST_SEC);
```

---

### Q-6 — Two `public:` Sections in `TM1637ClockUsermod`

**File:** `usermod_tm1637_clock.cpp`

The class has a `public:` section for `setup()` / `loop()` (around line 275) and another `public:` section just before `getId()` (around line 403). C++ allows multiple access specifiers but it's confusing. Merge them:

```cpp
public:
    void setup()   override { ... }
    void loop()    override { ... }
    uint16_t getId() override { return USERMOD_ID_TM1637_CLOCK; }
    void addToJsonInfo(JsonObject& root)   override { ... }
    void addToJsonState(JsonObject& root)  override { ... }
    void readFromJsonState(JsonObject& root) override { ... }
    void addToConfig(JsonObject& root)     override { ... }
    bool readFromConfig(JsonObject& root)  override { ... }
    void appendConfigData()                override { ... }
    void applyWeatherLightPattern(bool ignoreBaseballOverride = false);
    void enable(bool en) { enabled = en; }
    bool isEnabled() const { return enabled; }
```

---

## 7. Step-by-Step Refactoring Roadmap

Work through these in order — later steps depend on earlier ones.

---

### Step 1 — Fix the ODR violation (BUG-1)

**Priority: Critical — do this first**

1. Open `usermods/baseball_api/usermod_baseball_api.h`.
2. Replace the current partial class definition with the **complete** class definition from `usermod_baseball_api.cpp` — all private members, struct, static members, private methods, and public methods.
3. Add the missing fields that the `.h` was missing:
   - `bool wasConnected = false;`
   - `int liveGamePk = 0;`
   - `String lastLiveDataUrl = "";`
   - All private helper methods (`_isLiveState`, `_isFinalState`, `_isGameHappeningNow`, `_formatUtcDebug`, `_buildDateYmd`, `_buildScheduleUrl`, `_buildLiveDataUrl`, `_buildStatusLine`, `_buildPaletteDisplay`, `_buildStatusValue`)
4. In `usermod_baseball_api.cpp`:
   - Add `#include "usermod_baseball_api.h"` at the top (inside the `#ifdef USERMOD_BASEBALL_API` block).
   - Delete the entire `class UsermodBaseballAPI { … };` re-definition.
   - Keep only out-of-line method implementations and static member definitions.

---

### Step 2 — Fix the NTP seconds check (BUG-2)

1. Open `usermods/tm1637_Display/usermod_tm1637_display.cpp`.
2. In `updateDisplayState()` (~line 374), replace:
   ```cpp
   } else if (toki.getTime().sec == 0) {
       newState = NO_NTP;
   } else {
       newState = SHOW_TIME;
   }
   ```
   with:
   ```cpp
   } else {
       updateLocalTime();
       newState = (localTime >= 1704067200UL && localTime < 4102444800UL)
                  ? SHOW_TIME : NO_NTP;
   }
   ```
3. In `showTime()` (~line 400), replace:
   ```cpp
   if (toki.getTime().sec != 0) {
   ```
   with:
   ```cpp
   updateLocalTime();
   if (localTime >= 1704067200UL && localTime < 4102444800UL) {
   ```

---

### Step 3 — Add shared helpers to `weather_light_patterns.h` (DRY-1/2/3)

1. Open `usermods/weather_api/weather_light_patterns.h`.
2. After the palette constant `#define` block and before section 8 (`getPaletteByCondition`), add the three inline functions: `conditionSeverity`, `conditionDescription`, and `conditionToCategory` as shown in sections DRY-1, DRY-2, and DRY-3 above.
3. Open `usermods/tm1637_Display/usermod_tm1637_display.cpp` — delete the private `static uint8_t conditionSeverity(int code)` and `static const char* conditionDescription(int code)` methods from the class.
4. Open `usermods/TM1637_Clock/usermod_tm1637_clock.cpp` — delete the private `static uint8_t conditionSeverity(int code)`, `static const char* conditionDescription(int code)`, and `static uint8_t conditionToCategory(int code)` methods from the class.
5. Verify both files still compile — both already include `weather_light_patterns.h`.

---

### Step 4 — Guard the baseball include (D-1 / W-1 guard)

1. Open `usermods/TM1637_Clock/usermod_tm1637_clock.cpp`.
2. Change line 28 from:
   ```cpp
   #include "../baseball_api/usermod_baseball_api.h"
   ```
   to:
   ```cpp
   #ifdef USERMOD_BASEBALL_API
     #include "../baseball_api/usermod_baseball_api.h"
   #endif
   ```
3. Inside the class definition, guard the `baseballApi` member:
   ```cpp
   #ifdef USERMOD_BASEBALL_API
       UsermodBaseballAPI* baseballApi = nullptr;
   #endif
   ```
4. Audit all uses of `baseballApi` in `setup()`, `loop()`, and `applyWeatherLightPattern()`. Those already inside `#ifdef USERMOD_BASEBALL_API` blocks are fine. The cast in `setup()` also needs a guard:
   ```cpp
   #ifdef USERMOD_BASEBALL_API
       baseballApi = (UsermodBaseballAPI*)UsermodManager::lookup(USERMOD_ID_BASEBALL_API);
   #endif
   ```

---

### Step 5 — Add structured score accessors (DRY-4)

1. In `usermod_baseball_api.h`, add to the private section:
   ```cpp
   String _homeTeam  = "";
   String _awayTeam  = "";
   int    _homeScore = -1;
   int    _awayScore = -1;
   ```
2. Add to the public section:
   ```cpp
   const String& getHomeTeam()  const { return _homeTeam; }
   const String& getAwayTeam()  const { return _awayTeam; }
   int           getHomeScore() const { return _homeScore; }
   int           getAwayScore() const { return _awayScore; }
   ```
3. In `_parseMLB()` (baseball_api.cpp ~line 568), add field population alongside the `lastScore` assignment:
   ```cpp
   _awayTeam  = away;
   _awayScore = (aScore >= 0) ? aScore : 0;
   _homeTeam  = home;
   _homeScore = (hScore >= 0) ? hScore : 0;
   lastScore  = away + " " + String(_awayScore) + " @ " + home + " " + String(_homeScore);
   ```
4. Reset these fields when a live game ends (in the `gameLive = false; liveGamePk = 0;` block):
   ```cpp
   _homeTeam = ""; _awayTeam = ""; _homeScore = -1; _awayScore = -1;
   ```
5. In `TM1637ClockUsermod`, replace the `parseBaseballScore()` + `showBaseballScore()` methods with the simplified logic from DRY-4 section above.
6. Delete `parseBaseballScore()` and `showBaseballScore()` from `TM1637ClockUsermod`.

---

### Step 6 — Fix `weatherFetchFailed` dead field (BUG-5)

1. Open `usermods/tm1637_Display/usermod_tm1637_display.cpp`.
2. Delete the `weatherFetchFailed` field declaration.
3. In `showTemperature()`, change:
   ```cpp
   if (!weatherFetched || weatherFetchFailed) {
   ```
   to:
   ```cpp
   if (!weatherFetched) {
   ```
4. In `addToJsonInfo()`, change:
   ```cpp
   if (weatherFetched && !weatherFetchFailed) {
   ```
   to:
   ```cpp
   if (weatherFetched) {
   ```

---

### Step 7 — Fix duplicate `lastFetch` assignment (BUG-3)

1. Open `usermod_baseball_api.cpp`.
2. Find the `if (favoriteTeam != previousFavoriteTeam)` block in `readFromConfig()`.
3. The block assigns `lastFetch = millis() - intervalMs;` twice. Delete the **second** assignment (the one after the `DEBUG_PRINTF`).

---

### Step 8 — Initialize saved state variables (BUG-4)

1. In `usermod_baseball_api.cpp` (or `.h` after Step 1), change:
   ```cpp
   uint8_t savedMode, savedPalette, savedSpeed, savedIntensity;
   ```
   to:
   ```cpp
   uint8_t savedMode = 0, savedPalette = 0, savedSpeed = 128, savedIntensity = 128;
   ```

---

### Step 9 — Use `colorUpdated()` in `applyWeatherLightPattern()` (W-4)

1. Open `usermods/TM1637_Clock/usermod_tm1637_clock.cpp`.
2. At the end of `applyWeatherLightPattern()`, replace:
   ```cpp
   stateChanged = true;
   strip.trigger();
   ```
   with:
   ```cpp
   colorUpdated(CALL_MODE_DIRECT_CHANGE);
   ```

---

### Step 10 — Add `_teamPaletteDirty` flag (W-5)

1. In `usermod_baseball_api.h` private section, add:
   ```cpp
   bool _teamPaletteDirty = true;
   ```
2. In `_ensureTeamPalette()`, add an early-out at the top:
   ```cpp
   void _ensureTeamPalette() {
       if (!_teamPaletteDirty) return;
       // ... existing logic ...
       _teamPaletteDirty = false;
   }
   ```
3. Set `_teamPaletteDirty = true` in:
   - `_removeTeamPalette()` (after clearing `teamPaletteIndex`)
   - `readFromConfig()` when `favoriteTeam != previousFavoriteTeam`

---

### Step 11 — Remove display-only fields from `addToConfig` (W-3)

1. In `usermod_baseball_api.cpp` `addToConfig()`, remove the two lines:
   ```cpp
   top[FPSTR(_paletteKey)] = _buildPaletteDisplay();
   top[FPSTR(_statusKey)]  = _buildStatusValue();
   ```
2. Remove `_paletteKey` and `_statusKey` from `addToConfig` only. They can remain in `appendConfigData()` if you want the UI display fields — but those fields must be populated from `/json/info` rather than config. 
   - The simplest approach: remove the settings page fields entirely and show this data only in the info panel (which already shows `scoreArr`).
   - If you want to keep the status display in the settings page, the `appendConfigData` JS needs to fetch from `/json/info` like the Weather API's status panel does.

---

### Step 12 — Fix `_keyLat` / `_keyLon` declarations (W-1 / dead code)

1. Open `usermods/weather_api/usermod_weather_api.h`.
2. Remove the two dead declarations:
   ```cpp
   static const char _keyLat[];    // delete
   static const char _keyLon[];    // delete
   ```
3. If you want lat/lon to be user-configurable (overriding WLED's global location), add the full implementation:
   - Define in `.cpp`: `const char WeatherApiUsermod::_keyLat[] PROGMEM = "lat";`
   - Add to `addToConfig`: `top[FPSTR(_keyLat)] = _latitude;`
   - Add to `readFromConfig`: `getJsonValue(top[FPSTR(_keyLat)], _latitude);`
   - Add a hint in `appendConfigData`

---

### Step 13 — Code style cleanup

1. Fix missing spaces after `if` in `TM1637ClockUsermod::loop()`:
   ```cpp
   // Change:
   if(baseballApi){
       if( baseballApi->isGameLive()){
   // To:
   if (baseballApi) {
       if (baseballApi->isGameLive()) {
   ```
2. Remove commented-out debug line in `loop()`.
3. Fix `isEnabled()` missing `const` in `TM1637ClockUsermod`.
4. Merge the two `public:` sections in `TM1637ClockUsermod` into one.
5. Add named constants for magic numbers in `_isGameHappeningNow()`.

---

### Step 14 — Reduce HTTP timeout in `UsermodBaseballAPI` (W-6)

1. In `usermod_baseball_api.cpp` `_doFetch()`, change:
   ```cpp
   http.setTimeout(7000);
   ```
   to:
   ```cpp
   yield();
   http.setTimeout(5000);
   ```

---

## 8. Summary Checklist

| # | Issue | Type | Severity | File(s) |
|---|-------|------|----------|---------|
| BUG-1 | ODR violation: dual class definition | Bug | 🔴 High | `baseball_api.h/.cpp` |
| BUG-2 | `toki.sec == 0` false NTP check | Bug | 🔴 High | `tm1637_display.cpp` |
| BUG-3 | Duplicate `lastFetch` assignment | Bug | 🟡 Low | `baseball_api.cpp` |
| BUG-4 | Uninitialized save-state fields | Bug | 🟡 Medium | `baseball_api.cpp` |
| BUG-5 | `weatherFetchFailed` never true | Bug | 🟡 Low | `tm1637_display.cpp` |
| DRY-1 | `conditionSeverity()` × 2 | DRY | 🔴 High | display.cpp, clock.cpp |
| DRY-2 | `conditionDescription()` × 2 | DRY | 🔴 High | display.cpp, clock.cpp |
| DRY-3 | `conditionToCategory()` not shared | DRY | 🟡 Medium | clock.cpp |
| DRY-4 | Score string parsed in wrong class | DRY | 🟡 Medium | clock.cpp / baseball.cpp |
| S-1 | `TM1637ClockUsermod` >1 responsibility | SOLID | 🟡 Medium | clock.cpp |
| D-1 | Unconditional baseball include | SOLID | 🔴 High | clock.cpp |
| L-1 | `isEnabled()` not const | SOLID | 🟢 Low | clock.cpp |
| W-1 | `_keyLat/_keyLon` declared, never defined | WLED | 🟡 Medium | `weather_api.h` |
| W-2 | `PinOwner::UM_Unspecified` for TM1637 | WLED | 🟡 Medium | `tm1637_display.cpp` |
| W-3 | Computed fields in `addToConfig` | WLED | 🟡 Medium | `baseball_api.cpp` |
| W-4 | `stateChanged` instead of `colorUpdated()` | WLED | 🟡 Medium | `clock.cpp` |
| W-5 | `_ensureTeamPalette()` every loop tick | WLED | 🟡 Medium | `baseball_api.cpp` |
| W-6 | 7s HTTP timeout (ESP8266 WDT risk) | WLED | 🟡 Medium | `baseball_api.cpp` |
| W-7 | `String` heap alloc in hot `addToJsonInfo` | WLED | 🟢 Low | `baseball_api.cpp` |
| Q-1–5 | Style / dead code / magic numbers | Quality | 🟢 Low | all |

---

## 9. Reference Links

| Topic | Link |
|---|---|
| WLED Usermod API | https://kno.wled.ge/advanced/usermod-api/ |
| WLED PinManager source | https://github.com/wled/WLED/blob/main/wled00/pin_manager.h |
| WLED `colorUpdated()` | https://github.com/wled/WLED/blob/main/wled00/wled.h |
| ArduinoJSON v6 filter | https://arduinojson.org/v6/how-to/filter-keys-of-an-object/ |
| ArduinoJSON memory guide | https://arduinojson.org/v6/assistant/ |
| C++ One Definition Rule | https://en.cppreference.com/w/cpp/language/definition |
| SOLID principles | https://en.wikipedia.org/wiki/SOLID |
| DRY principle | https://en.wikipedia.org/wiki/Don%27t_repeat_yourself |
| WeatherAPI condition codes | https://www.weatherapi.com/docs/weather_conditions.json |
| MLB Stats API (unofficial) | https://github.com/toddrob99/MLB-StatsAPI |
| ESP8266 WDT / yield | https://arduino-esp8266.readthedocs.io/en/latest/reference.html#watchdog-timer |
| WLED `F()` / PROGMEM guide | https://kno.wled.ge/advanced/compiling-wled/#flash-string-helper |
