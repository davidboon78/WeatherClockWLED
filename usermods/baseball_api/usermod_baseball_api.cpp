#include "wled.h"

#ifdef USERMOD_BASEBALL_API

#ifdef ESP8266
  #include <ESP8266HTTPClient.h>
#else
  #include <HTTPClient.h>
#endif

class UsermodBaseballAPI : public Usermod {
  private:
    static const uint32_t TEAM_COLOR_BLANK = 0xFFFFFFFF;

    // Private class members
    bool enabled = false;
    bool initDone = false;
    unsigned long lastFetch = 0;
    unsigned long intervalMs = 600000; // Default 10 minutes
    bool wasConnected = false;
    
    // Configuration variables
    String favoriteTeam = "";
    bool overrideLightsOnGame = false;
    
    // Internal state
    bool gameLive = false;
    int liveGamePk = 0;
    String lastScore = "No data";
    String nextGameInfo = "";
    String lastRequestUrl = "";
    String lastLiveDataUrl = "";
    String lastFetchError = "";
    int lastHttpCode = 0;
    
    // Save/Restore light state
    uint8_t savedMode = 0, savedPalette = 0, savedSpeed = 128, savedIntensity = 128;
    bool gameOverrideActive = false;
    int8_t teamPaletteIndex = -1;
    String teamPaletteName = "";

    // String keys for config (PROGMEM saves RAM)
    static const char _name[];
    static const char _enabledKey[];
    static const char _teamKey[];
    static const char _overrideKey[];
    static const char _paletteKey[];
    static const char _statusKey[];

    // MLB team metadata used for config UI, display, and team-color lookup.
    struct TeamMap {
      const char* teamName;
      int mlbId;
      uint8_t colorCount;
      uint32_t colors[5];
    };
    static const TeamMap mlbMap[];

    const TeamMap* _resolveTeamMap(int mlbId) const {
      for (uint8_t i = 0; i < 30; i++) {
        if (mlbMap[i].mlbId == mlbId) return &mlbMap[i];
      }
      DEBUG_PRINTF("BaseballAPI: resolveTeamMap failed for mlbId=%d\n", mlbId);
      return nullptr;
    }

    int _resolveMlbId() const {
      return favoriteTeam.toInt();
    }

    const char* _resolveTeamName(int mlbId) const {
      const TeamMap* team = _resolveTeamMap(mlbId);
      return team ? team->teamName : nullptr;
    }

    String _buildTeamPaletteName(const TeamMap& team) const {
      return String(team.teamName) + " Team Colors";
    }

    uint8_t _buildEffectiveTeamColors(const TeamMap& team, uint32_t outColors[5]) const {
      uint8_t colorCount = team.colorCount;
      if (colorCount == 0) {
        DEBUG_PRINTF("BaseballAPI: no palette colors defined for team=%s\n", team.teamName);
        return 0;
      }
      if (colorCount > 5) colorCount = 5;

      // Replace explicit blanks (TEAM_COLOR_BLANK) by repeating non-blank
      // colors while preserving legitimate black (0x000000).
      uint8_t nonBlankIndices[5];
      uint8_t nonBlankCount = 0;
      for (uint8_t i = 0; i < colorCount; i++) {
        if (team.colors[i] != TEAM_COLOR_BLANK && nonBlankCount < 5) {
          nonBlankIndices[nonBlankCount++] = i;
        }
      }

      if (nonBlankCount == 0) {
        DEBUG_PRINTF("BaseballAPI: all configured colors are blank for team=%s\n", team.teamName);
        return 0;
      }

      uint8_t repeatPos = 0;
      for (uint8_t i = 0; i < colorCount; i++) {
        uint32_t color = team.colors[i];
        if (color == TEAM_COLOR_BLANK) {
          color = team.colors[nonBlankIndices[repeatPos % nonBlankCount]];
          repeatPos++;
        }
        outColors[i] = color;
      }

      return colorCount;
    }

    void _loadPaletteColors(CRGBPalette16& palette, const TeamMap& team) const {
      uint32_t effectiveColors[5];
      uint8_t colorCount = _buildEffectiveTeamColors(team, effectiveColors);
      if (colorCount == 0) {
        palette = CRGBPalette16(CRGB(BLACK));
        return;
      }

      if (colorCount == 1) {
        uint32_t color = effectiveColors[0];
        palette = CRGBPalette16(CRGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
        return;
      }

      uint8_t tcp[20];
      for (uint8_t i = 0; i < colorCount; i++) {
        uint32_t color = effectiveColors[i];
        tcp[i * 4] = (uint8_t)((i * 255U) / (colorCount - 1));
        tcp[i * 4 + 1] = (color >> 16) & 0xFF;
        tcp[i * 4 + 2] = (color >> 8) & 0xFF;
        tcp[i * 4 + 3] = color & 0xFF;
      }

      palette.loadDynamicGradientPalette(tcp);
    }

    void _removeTeamPalette() {
      if (teamPaletteIndex < 0) {
        teamPaletteName = "";
        return;
      }

      if (teamPaletteIndex < (int)customPalettes.size()) {
        customPalettes.erase(customPalettes.begin() + teamPaletteIndex);
      }

      teamPaletteIndex = -1;
      teamPaletteName = "";
    }

    void _ensureTeamPalette() {
      const TeamMap* team = _resolveTeamMap(_resolveMlbId());
      if (!team) {
        DEBUG_PRINTF("BaseballAPI: team palette unavailable, team id='%s'\n", favoriteTeam.c_str());
        _removeTeamPalette();
        return;
      }

      if (teamPaletteIndex >= (int)customPalettes.size()) {
        teamPaletteIndex = -1;
      }

      if (teamPaletteIndex < 0) {
        if (customPalettes.size() >= WLED_MAX_CUSTOM_PALETTES) {
          DEBUG_PRINTF("BaseballAPI: cannot allocate team palette, customPalettes=%u max=%u\n", (unsigned)customPalettes.size(), (unsigned)WLED_MAX_CUSTOM_PALETTES);
          return;
        }
        customPalettes.push_back(CRGBPalette16(CRGB(BLACK)));
        teamPaletteIndex = (int)customPalettes.size() - 1;
      }

      _loadPaletteColors(customPalettes[teamPaletteIndex], *team);
      teamPaletteName = _buildTeamPaletteName(*team);
    }

    bool _hasValidClock() const {
      updateLocalTime();
      return localTime >= 1704067200 && localTime < 4102444800; // 2024-01-01 .. 2100-01-01
    }

    time_t _currentUtcApprox() const {
      return (time_t)toki.second();
    }

    int32_t _currentLocalOffsetSecs() const {
      updateLocalTime();
      return (int32_t)((time_t)localTime - _currentUtcApprox());
    }

    bool _parseApiUtc(const String& apiDate, time_t& utcTime) const {
      int yearValue, monthValue, dayValue, hourValue, minuteValue, secondValue;
      if (sscanf(apiDate.c_str(), "%d-%d-%dT%d:%d:%dZ", &yearValue, &monthValue, &dayValue, &hourValue, &minuteValue, &secondValue) != 6) {
        DEBUG_PRINTF("BaseballAPI: parseApiUtc failed for '%s'\n", apiDate.c_str());
        return false;
      }
      utcTime = getUnixTime(hourValue, minuteValue, secondValue, dayValue, monthValue, yearValue);
      if (!(utcTime > 0)) DEBUG_PRINTF("BaseballAPI: getUnixTime failed for '%s'\n", apiDate.c_str());
      return utcTime > 0;
    }

    String _formatApiUtcToLocal(const String& apiDate) const {
      time_t utcTime;
      if (!_parseApiUtc(apiDate, utcTime)) return apiDate;

      // If local clock is not synced yet, keep API time in UTC so schedule data
      // still appears instead of being blocked on NTP.
      if (!_hasValidClock()) return apiDate + " UTC";

      time_t localGameTime = utcTime + (time_t)_currentLocalOffsetSecs();
      tmElements_t tmLocal;
      breakTime(localGameTime, tmLocal);

      char buf[32];
      if (useAMPM) {
        uint8_t hour12 = tmLocal.Hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %u:%02u %s",
          (int)tmLocal.Year + 1970, tmLocal.Month, tmLocal.Day, hour12, tmLocal.Minute, (tmLocal.Hour >= 12) ? "PM" : "AM");
      } else {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02u:%02u",
          (int)tmLocal.Year + 1970, tmLocal.Month, tmLocal.Day, tmLocal.Hour, tmLocal.Minute);
      }

      return String(buf);
    }

    unsigned long _upcomingPollInterval(time_t gameUtcTime) const {
      time_t currentUtc = _currentUtcApprox();
      if (gameUtcTime <= currentUtc + 900) return 30000;
      if (gameUtcTime <= currentUtc + 3600) return 60000;
      if (gameUtcTime <= currentUtc + 10800) return 120000;
      return 300000;
    }

    bool _isLiveState(const String& abstractState, const String& detailedState, const String& codedState) const {
      return (abstractState == "Live") ||
             (codedState == "I") ||
             (codedState == "M") ||
             (detailedState.indexOf("In Progress") >= 0);
    }

    bool _isFinalState(const String& abstractState, const String& detailedState, const String& codedState) const {
      return (abstractState == "Final") || (codedState == "F") || (detailedState == "Final");
    }

    bool _isGameHappeningNow(time_t nowUtc, time_t gameUtcTime) const {
      // Consider a game "happening now" if its scheduled start time is within:
      //   - up to 6 hours in the past  (covers long/extra-inning games in progress)
      //   - up to 30 minutes in the future (pre-game window so override applies before first pitch)
      return gameUtcTime >= (nowUtc - 21600) && gameUtcTime <= (nowUtc + 1800);
    }

    String _formatUtcDebug(time_t t) const {
      if (t <= 0) return "invalid";
      struct tm tmUtc;
      gmtime_r(&t, &tmUtc);
      char buf[40];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d UTC",
        tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
        tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
      return String(buf);
    }

    String _buildDateYmd(int dayOffset) const {
      if (!_hasValidClock()) return "";
      time_t now = _currentUtcApprox() + (time_t)dayOffset * 86400;
      struct tm tmUtc;
      gmtime_r(&now, &tmUtc);
      char buf[16];
      uint16_t y = (uint16_t)(tmUtc.tm_year + 1900);
      uint8_t mo = (uint8_t)(tmUtc.tm_mon + 1);
      uint8_t d = (uint8_t)tmUtc.tm_mday;
      snprintf(buf, sizeof(buf), "%04u-%02u-%02u", y, mo, d);
      return String(buf);
    }

    String _buildScheduleUrl(int mlbId) const {
      if (mlbId <= 0) return "";
      String url = "http://statsapi.mlb.com/api/v1/schedule?sportId=1&teamId=" + String(mlbId);
      // Start 1 day back (UTC) so games that began UTC-yesterday but are still
      // in progress (e.g. late-night games past midnight UTC) are included.
      String startDate = _buildDateYmd(-1);
      String endDate = _buildDateYmd(2);
      if (startDate.length() > 0 && endDate.length() > 0) {
        url += "&startDate=" + startDate + "&endDate=" + endDate;
      }
      url += "&fields=dates,date,games,gamePk,gameDate,status,abstractGameState,detailedState,codedGameState,teams,home,away,score,team,name";
      return url;
    }

    String _buildLiveDataUrl(int gamePk) const {
      if (gamePk <= 0) return "";
      String url = "http://statsapi.mlb.com/api/v1.1/game/" + String(gamePk) + "/feed/live";
      DEBUG_PRINTF("BaseballAPI: live data URL=%s\n", url.c_str());
      return url;
    }

    String _buildStatusLine() const {
      int mlbId = _resolveMlbId();
      const char* resolvedTeamName = _resolveTeamName(mlbId);
      String teamName = resolvedTeamName ? String(resolvedTeamName) : (favoriteTeam.length() > 0 ? ("Team " + favoriteTeam) : "Team not selected");

      if (gameLive && lastScore.length() > 0) {
        return "Status(2): Live game - " + lastScore;
      }
      if (nextGameInfo.length() > 0) {
        return "Status(1): Upcoming " + teamName + " game - " + nextGameInfo;
      }
      return "Status(1): Upcoming " + teamName + " game - unavailable";
    }

    String _buildPaletteDisplay() const {
      const TeamMap* team = _resolveTeamMap(_resolveMlbId());
      if (!team) return "Unavailable";

      uint32_t effectiveColors[5];
      uint8_t colorCount = _buildEffectiveTeamColors(*team, effectiveColors);
      if (colorCount == 0) return _buildTeamPaletteName(*team);

      String palette = _buildTeamPaletteName(*team) + " [";
      for (uint8_t i = 0; i < colorCount; i++) {
        if (i > 0) palette += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "#%06X", (unsigned int)(effectiveColors[i] & 0xFFFFFF));
        palette += hex;
      }
      palette += "]";
      return palette;
    }

    String _buildStatusValue() const {
      if (gameLive && lastScore.length() > 0) return lastScore;
      if (nextGameInfo.length() > 0 && nextGameInfo != "Waiting for first fetch" && nextGameInfo != "Refreshing team schedule") {
        int openParen = nextGameInfo.lastIndexOf('(');
        int atPos = nextGameInfo.indexOf(" @ ");
        if (atPos > 0 && openParen > atPos + 3) {
          int dateStart = -1;
          for (int i = atPos + 3; i + 9 < openParen; i++) {
            bool ymd = (nextGameInfo[i] >= '0' && nextGameInfo[i] <= '9') &&
                       (nextGameInfo[i + 1] >= '0' && nextGameInfo[i + 1] <= '9') &&
                       (nextGameInfo[i + 2] >= '0' && nextGameInfo[i + 2] <= '9') &&
                       (nextGameInfo[i + 3] >= '0' && nextGameInfo[i + 3] <= '9') &&
                       (nextGameInfo[i + 4] == '-') &&
                       (nextGameInfo[i + 5] >= '0' && nextGameInfo[i + 5] <= '9') &&
                       (nextGameInfo[i + 6] >= '0' && nextGameInfo[i + 6] <= '9') &&
                       (nextGameInfo[i + 7] == '-') &&
                       (nextGameInfo[i + 8] >= '0' && nextGameInfo[i + 8] <= '9') &&
                       (nextGameInfo[i + 9] >= '0' && nextGameInfo[i + 9] <= '9');
            if (ymd) {
              dateStart = i;
              break;
            }
          }

          if (dateStart > atPos + 3) {
            String awayTeam = nextGameInfo.substring(0, atPos);
            String homeTeam = nextGameInfo.substring(atPos + 3, dateStart - 1);
            String gameDateLocal = nextGameInfo.substring(dateStart, openParen - 1);

            awayTeam.trim();
            homeTeam.trim();
            gameDateLocal.trim();

            int mlbId = _resolveMlbId();
            const char* resolvedTeamName = _resolveTeamName(mlbId);
            String favoriteTeamName = resolvedTeamName ? String(resolvedTeamName) : "Favorite team";

            String opponent = "Opponent";
            if (favoriteTeamName.equalsIgnoreCase(awayTeam)) opponent = homeTeam;
            else if (favoriteTeamName.equalsIgnoreCase(homeTeam)) opponent = awayTeam;
            else opponent = (homeTeam.length() > 0) ? homeTeam : awayTeam;

            return favoriteTeamName + " vs " + opponent + "<br/>" + gameDateLocal;
          }
        }
      }
      return "Unavailable";
    }

    void _doFetch() {
      if (favoriteTeam.length() == 0) {
        DEBUG_PRINTLN(F("BaseballAPI: fetch skipped, no favorite team selected"));
        lastFetch = millis();
        return;
      }
      if (!WLED_CONNECTED) {
        DEBUG_PRINTLN(F("BaseballAPI: fetch skipped, WLED not connected"));
        lastFetchError = "WiFi not connected";
        lastFetch = millis();
        return;
      }
      if (!_hasValidClock()) {
        DEBUG_PRINTF("BaseballAPI: fetch skipped, clock not valid (toki=%lu)\n", (unsigned long)toki.second());
        lastFetchError = "Clock not synced";
        intervalMs = 10000; // retry every 10s until clock syncs
        lastFetch = millis();
        return;
      }

      int mlbId = _resolveMlbId();
      if (mlbId <= 0) {
        nextGameInfo = "Team unavailable";
        lastFetchError = "Invalid MLB team id";
        lastHttpCode = 0;
        DEBUG_PRINTF("BaseballAPI: invalid favorite team id '%s'\n", favoriteTeam.c_str());
        lastFetch = millis();
        return;
      }

      String url = _buildScheduleUrl(mlbId);
      DEBUG_PRINTF("BaseballAPI: schedule URL=%s\n", url.c_str());
      lastRequestUrl = url;
      lastFetchError = "";
      lastHttpCode = 0;

      WiFiClient client;
      HTTPClient http;
      http.setTimeout(7000);

      if (!http.begin(client, url)) {
        lastFetchError = "HTTP begin failed";
        nextGameInfo = "Request setup failed";
        DEBUG_PRINTF("BaseballAPI: http.begin failed url=%s\n", url.c_str());
        lastFetch = millis();
        return;
      }

      int httpCode = http.GET();
      lastHttpCode = httpCode;
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        if (payload.length() == 0) DEBUG_PRINTF("BaseballAPI: empty HTTP payload, url=%s\n", url.c_str());
        _parseMLB(payload);
      } else {
        nextGameInfo = "HTTP " + String(httpCode);
        lastFetchError = nextGameInfo;
        DEBUG_PRINTF("BaseballAPI: HTTP GET failed code=%d url=%s\n", httpCode, url.c_str());
        if (gameLive && overrideLightsOnGame) _restoreGameOverride();
        gameLive = false;
      }

      http.end();
      lastFetch = millis();
    }

    void _parseMLB(const String& json) {
      DynamicJsonDocument doc(6144);
      DeserializationError err = deserializeJson(doc, json);
      if (err) {
        nextGameInfo = "JSON parse failed";
        lastFetchError = err.c_str();
        DEBUG_PRINTF("BaseballAPI: JSON parse failed: %s\n", err.c_str());
        return;
      }

      JsonArray dates = doc["dates"].as<JsonArray>();
      if (dates.isNull() || dates.size() == 0) {
        nextGameInfo = "No upcoming game in window";
        lastFetchError = "No dates in response";
        DEBUG_PRINTLN(F("BaseballAPI: no dates array in MLB response"));
        if (gameLive && overrideLightsOnGame) _restoreGameOverride();
        gameLive = false;
        intervalMs = 900000;
        return;
      }

      bool wasLive = gameLive;
      bool foundLive = false;
      bool foundUpcoming = false;
      bool sawActiveLiveState = false;
      bool sawActiveFinalState = false;
      bool sawAnyStartedGame = false;
      String upAway = "";
      String upHome = "";
      String upDate = "";
      String upState = "";
      String upLiveUrl = "";
      time_t upcomingUtcTime = 0;
      time_t nowUtc = _currentUtcApprox();
      bool clockValid = _hasValidClock();

      DEBUG_PRINTF("BaseballAPI: UTC now=%ld (%s) clockValid=%d\n",
        (long)nowUtc, _formatUtcDebug(nowUtc).c_str(), clockValid);
      if (!clockValid) {
        DEBUG_PRINTLN(F("BaseballAPI: WLED clock invalid; cannot verify live window by time yet"));
      }

      for (JsonObject d : dates) {
        JsonArray games = d["games"].as<JsonArray>();
        if (games.isNull()) continue;
        for (JsonObject game : games) {
          String state = game["status"]["abstractGameState"].as<String>();
          String detailed = game["status"]["detailedState"].as<String>();
          String coded = game["status"]["codedGameState"].as<String>();
          int gamePk = game["gamePk"] | 0;
          String home = game["teams"]["home"]["team"]["name"].as<String>();
          String away = game["teams"]["away"]["team"]["name"].as<String>();
          String gameDate = game["gameDate"].as<String>();
          int hScore = game["teams"]["home"]["score"] | -1;
          int aScore = game["teams"]["away"]["score"] | -1;

          time_t gameUtcTime = 0;
          bool hasGameUtc = _parseApiUtc(gameDate, gameUtcTime);
          bool gameInLiveWindow = clockValid && hasGameUtc && _isGameHappeningNow(nowUtc, gameUtcTime);

          if (hasGameUtc) {
            time_t liveWindowStartUtc = nowUtc - 1800;
            time_t liveWindowEndUtc = nowUtc + 21600;
            DEBUG_PRINTF("BaseballAPI: gamePk=%d UTC window %ld (%s) <= gameStart=%ld (%s) <= %ld (%s), inWindow=%d\n",
              gamePk,
              (long)liveWindowStartUtc,
              _formatUtcDebug(liveWindowStartUtc).c_str(),
              (long)gameUtcTime,
              _formatUtcDebug(gameUtcTime).c_str(),
              (long)liveWindowEndUtc,
              _formatUtcDebug(liveWindowEndUtc).c_str(),
              gameInLiveWindow);
          } else {
            DEBUG_PRINTF("BaseballAPI: gamePk=%d UTC window gameStart=unparsed raw='%s' inWindow=%d\n",
              gamePk, gameDate.c_str(), gameInLiveWindow);
          }

          if (gameInLiveWindow) sawAnyStartedGame = true;

          bool isLiveState = _isLiveState(state, detailed, coded) && gameInLiveWindow;
          bool isFinalState = _isFinalState(state, detailed, coded);

          if (liveGamePk > 0 && gamePk == liveGamePk) {
            if (isLiveState) sawActiveLiveState = true;
            if (isFinalState) sawActiveFinalState = true;
          }

          DEBUG_PRINTF("BaseballAPI: gamePk=%d state='%s' detailed='%s' coded='%s' live=%d %s @ %s\n",
            gamePk, state.c_str(), detailed.c_str(), coded.c_str(), isLiveState, away.c_str(), home.c_str());

          if (isLiveState) {
            if (hScore >= 0 && aScore >= 0) {
              lastScore = away + " " + String(aScore) + " @ " + home + " " + String(hScore);
            } else {
              lastScore = away + " @ " + home + " (Live)";
            }
            liveGamePk = gamePk;
            lastLiveDataUrl = _buildLiveDataUrl(gamePk);
            foundLive = true;
            break;
          }

          if (!foundUpcoming && (state == "Preview" || detailed == "Scheduled" || state == "Pre-Game")) {
            upAway = away;
            upHome = home;
            upDate = _formatApiUtcToLocal(gameDate);
            upState = (detailed.length() > 0) ? detailed : state;
            upLiveUrl = _buildLiveDataUrl(gamePk);
            _parseApiUtc(gameDate, upcomingUtcTime);
            foundUpcoming = true;
          }

          if (lastScore == "No data" && (hScore >= 0 && aScore >= 0)) {
            lastScore = away + " " + String(aScore) + " @ " + home + " " + String(hScore) + " (" + state + ")";
          }
        }
        if (foundLive) break;
      }

      if (foundLive) {
        if (!wasLive && overrideLightsOnGame) _applyGameOverride();
        gameLive = true;
        intervalMs = 30000;
        lastFetchError = "";
        if (lastLiveDataUrl.length() > 0) {
          DEBUG_PRINTF("BaseballAPI: live data URL %s\n", lastLiveDataUrl.c_str());
        }
        return;
      }

      // If a known live game disappears from the filtered window and we only
      // see future/scheduled games, keep live state until we can positively
      // observe the tracked game reach a final state.
      bool uncertainLiveState = wasLive &&
                                (liveGamePk > 0) &&
                                !sawActiveLiveState &&
                                !sawActiveFinalState &&
                                !sawAnyStartedGame;
      if (uncertainLiveState) {
        intervalMs = 30000;
        lastFetchError = "";
        DEBUG_PRINTF("BaseballAPI: holding live state for gamePk=%d (only future games in schedule window)\n", liveGamePk);
        return;
      }

      if (wasLive && overrideLightsOnGame) _restoreGameOverride();
      gameLive = false;
      liveGamePk = 0;

      if (foundUpcoming) {
        nextGameInfo = upAway + " @ " + upHome + " " + upDate + " (" + upState + ")";
        lastLiveDataUrl = upLiveUrl;
        intervalMs = (upcomingUtcTime > 0) ? _upcomingPollInterval(upcomingUtcTime) : 300000;
        lastFetchError = "";
      } else {
        nextGameInfo = "No upcoming game in window";
        lastLiveDataUrl = "";
        intervalMs = 900000;
        lastFetchError = "No Preview/Scheduled game found";
        DEBUG_PRINTLN(F("BaseballAPI: no live game and no Preview/Scheduled game found in window"));
      }
    }

    void _applyGameOverride() {
      // Save global effect state only on first entry into live-game override.
      // Later re-enforcement passes must not overwrite the restore point.
      if (!gameOverrideActive) {
        // Save global effect state — these are what applyValuesToSelectedSegs() applies
        // to selected segments on every colorUpdated() call. Saving them (rather than
        // raw segment values) ensures restore puts WLED back to the same globals.
        savedMode      = effectCurrent;
        savedPalette   = effectPalette;
        savedSpeed     = effectSpeed;
        savedIntensity = effectIntensity;
      }

      _ensureTeamPalette();

      if (!(teamPaletteIndex >= 0 && teamPaletteIndex < (int)customPalettes.size())) {
        DEBUG_PRINTLN(F("BaseballAPI: live override failed, team palette unavailable"));
        gameOverrideActive = false;
        return;
      }

      effectCurrent   = FX_MODE_CHASE_COLOR;
      effectSpeed     = 255;
      effectIntensity = 255;
      effectPalette   = 255 - teamPaletteIndex;
      colorUpdated(CALL_MODE_DIRECT_CHANGE);

      DEBUG_PRINTF("BaseballAPI: live override applied mode=%u palette=%u speed=%u intensity=%u\n",
        (unsigned)effectCurrent, (unsigned)effectPalette, (unsigned)effectSpeed, (unsigned)effectIntensity);
      gameOverrideActive = true;
    }

    void _restoreGameOverride() {
      if (!gameOverrideActive) return;
      effectCurrent   = savedMode;
      effectPalette   = savedPalette;
      effectSpeed     = savedSpeed;
      effectIntensity = savedIntensity;
      colorUpdated(CALL_MODE_DIRECT_CHANGE);
      DEBUG_PRINTF("BaseballAPI: live override restored mode=%u palette=%u speed=%u intensity=%u\n",
        (unsigned)effectCurrent, (unsigned)effectPalette, (unsigned)effectSpeed, (unsigned)effectIntensity);
      gameOverrideActive = false;
    }

  public:
    bool isGameLive() const { return gameLive; }
    const String& getLastScore() const { return lastScore; }
    const String& getFavoriteTeam() const { return favoriteTeam; }
    bool isGameOverrideActive() const { return gameOverrideActive; }

    void setup() override {
      initDone = true;
      nextGameInfo = "Waiting for first fetch";
      lastFetch = millis() - intervalMs;
      _ensureTeamPalette();
    }

    void loop() override {
      // Guard clause: Don't run while LEDs are being updated
      if (!enabled || strip.isUpdating()) return;

      _ensureTeamPalette();

      // While game override is enabled and game is live, continuously enforce
      // chase + team palette so this beats any competing override path.
      if (gameLive && overrideLightsOnGame) {
        bool paletteReady = (teamPaletteIndex >= 0 && teamPaletteIndex < (int)customPalettes.size());
        uint8_t expectedPalette = paletteReady ? (uint8_t)(255 - teamPaletteIndex) : 0;
        bool overrideMismatch = !gameOverrideActive ||
                                effectCurrent != FX_MODE_CHASE_COLOR ||
                                effectSpeed != 255 ||
                                effectIntensity != 255 ||
                                (paletteReady && effectPalette != expectedPalette);

        if (overrideMismatch) {
          DEBUG_PRINTLN(F("BaseballAPI: enforcing live-game override"));
          _applyGameOverride();
          if (!(gameOverrideActive && effectCurrent == FX_MODE_CHASE_COLOR &&
                effectSpeed == 255 && effectIntensity == 255 &&
                (paletteReady ? (effectPalette == expectedPalette) : false))) {
            DEBUG_PRINTLN(F("BaseballAPI: enforce check failed after apply"));
          }
        }
      }

      // Trigger an immediate fetch whenever WiFi first becomes available
      // so we don't wait out the current intervalMs.
      bool isConnected = WLED_CONNECTED;
      if (isConnected && !wasConnected) {
        DEBUG_PRINTLN(F("BaseballAPI: WiFi connected, triggering immediate fetch"));
        lastFetch = millis() - intervalMs;
      }
      wasConnected = isConnected;

      if (millis() - lastFetch > intervalMs) {
        _doFetch();
      }
    }

    void addToJsonInfo(JsonObject& root) override {
      _ensureTeamPalette();

      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");

      if (teamPaletteIndex >= 0 && teamPaletteIndex < (int)customPalettes.size() && teamPaletteName.length() > 0) {
        JsonObject userCustomPalettes = root["umcp"];
        if (userCustomPalettes.isNull()) userCustomPalettes = root.createNestedObject("umcp");
        userCustomPalettes[String(255 - teamPaletteIndex)] = teamPaletteName;
      }
      
      JsonArray scoreArr = user.createNestedArray("MLB Game");
      int mlbId = _resolveMlbId();
      String apiUrl = _buildScheduleUrl(mlbId);
      String statusLine = _buildStatusLine();

      scoreArr.add(statusLine);
      // scoreArr.add("Poll ms=" + String(intervalMs));
      if (lastLiveDataUrl.length() > 0) {
        scoreArr.add("LiveDataURL=" + lastLiveDataUrl);
      }
      if (teamPaletteIndex >= 0 && teamPaletteIndex < (int)customPalettes.size() && teamPaletteName.length() > 0) {
        scoreArr.add("Palette=" + teamPaletteName + " (id " + String(255 - teamPaletteIndex) + ")");

        const TeamMap* team = _resolveTeamMap(mlbId);
        if (team) {
          uint32_t effectiveColors[5];
          uint8_t colorCount = _buildEffectiveTeamColors(*team, effectiveColors);
          if (colorCount > 0) {
            String paletteColors = "PaletteColors=";
            for (uint8_t i = 0; i < colorCount; i++) {
              if (i > 0) paletteColors += ",";
              char hex[8];
              snprintf(hex, sizeof(hex), "#%06X", (unsigned int)(effectiveColors[i] & 0xFFFFFF));
              paletteColors += hex;
            }
            scoreArr.add(paletteColors);
          }
        }
      }
      if (nextGameInfo.length() == 0 || nextGameInfo.indexOf("unavailable") >= 0 || nextGameInfo.indexOf("No upcoming") >= 0) {
        String diag = "Diag: teamId=" + favoriteTeam;
        diag += " resolvedId=" + String(mlbId);
        diag += " url=";
        if (lastRequestUrl.length() > 0) diag += lastRequestUrl;
        else diag += (apiUrl.length() > 0) ? apiUrl : "(unresolved)";
        scoreArr.add(diag);
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabledKey)] = enabled;
      top[FPSTR(_teamKey)] = favoriteTeam;
      top[FPSTR(_overrideKey)] = overrideLightsOnGame;
      top[FPSTR(_paletteKey)] = _buildPaletteDisplay();
      top[FPSTR(_statusKey)] = _buildStatusValue();
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      String previousFavoriteTeam = favoriteTeam;
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabledKey)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_teamKey)], favoriteTeam);
      configComplete &= getJsonValue(top[FPSTR(_overrideKey)], overrideLightsOnGame);

      if (favoriteTeam != previousFavoriteTeam) {
        if (gameOverrideActive) _restoreGameOverride();
        gameLive = false;
        lastScore = "No data";
        nextGameInfo = favoriteTeam.length() > 0 ? "Refreshing team schedule" : "Team not selected";
        lastRequestUrl = "";
        lastFetchError = "";
        lastHttpCode = 0;
        lastFetch = millis() - intervalMs;
        DEBUG_PRINTF("BaseballAPI: team changed from '%s' to '%s'\n", previousFavoriteTeam.c_str(), favoriteTeam.c_str());

        // Force next loop tick to fetch regardless of current connection state.
        // If not yet connected, the fetch guard in _doFetch() will retry each intervalMs
        // until connection succeeds.
      }

      _ensureTeamPalette();

      return configComplete;
    }

    void appendConfigData() override {
      oappend(SET_F("addInfo('BaseballAPI:enabled',1,'');"));
      oappend(SET_F("addInfo('BaseballAPI:team',1,'');"));
      oappend(SET_F("addInfo('BaseballAPI:override',1,'');"));
      oappend(SET_F("addInfo('BaseballAPI:palette',1,'');"));
      oappend(SET_F("addInfo('BaseballAPI:status',1,'');"));
      oappend(SET_F("var bpa=d.getElementsByName('BaseballAPI:palette');var bp=(bpa&&bpa.length>1)?bpa[1]:null;if(bp){bp.readOnly=true;var pc=bp.nextElementSibling;if(!pc||pc.className!=='bp-palette-colors'){pc=document.createElement('span');pc.className='bp-palette-colors';pc.style.marginLeft='8px';bp.insertAdjacentElement('afterend',pc);}var m=(bp.value||'').match(/#[0-9A-Fa-f]{6}/g)||[];pc.innerHTML='';for(var i=0;i<m.length;i++){var s=document.createElement('span');s.style.cssText='display:inline-block;width:12px;height:12px;margin:0 3px;border:1px solid #666;vertical-align:middle;background:'+m[i]+';';pc.appendChild(s);}}"));
      oappend(SET_F("var bss=d.getElementsByName('BaseballAPI:status');var bs=(bss&&bss.length>1)?bss[1]:null;if(bs){bs.readOnly=true;var sd=bs.nextElementSibling;if(!sd||sd.className!=='bp-status-display'){sd=document.createElement('span');sd.className='bp-status-display';sd.style.marginLeft='8px';sd.style.display='inline-block';sd.style.verticalAlign='middle';bs.insertAdjacentElement('afterend',sd);}sd.innerHTML=(bs.value||'Unavailable');bs.style.display='none';}"));

      oappend(SET_F("var dd=addDropdown('BaseballAPI','team');"));
      oappend(SET_F("addOption(dd,'Select team','');"));
      for (uint8_t i = 0; i < 30; i++) {
        const TeamMap& team = mlbMap[i];
        String opt = "addOption(dd,'" + String(team.teamName) + "','" + String(team.mlbId) + "');";
        oappend(opt.c_str());
      }
    }

    uint16_t getId() override { return USERMOD_ID_BASEBALL_API; }
};

// Static strings
const char UsermodBaseballAPI::_name[] PROGMEM = "BaseballAPI";
const char UsermodBaseballAPI::_enabledKey[] PROGMEM = "enabled";
const char UsermodBaseballAPI::_teamKey[] PROGMEM = "team";
const char UsermodBaseballAPI::_overrideKey[] PROGMEM = "override";
const char UsermodBaseballAPI::_paletteKey[] PROGMEM = "palette";
const char UsermodBaseballAPI::_statusKey[] PROGMEM = "status";

const UsermodBaseballAPI::TeamMap UsermodBaseballAPI::mlbMap[] = {
  {"Arizona Diamondbacks", 109, 5, {0xA71930, 0xE3D4AD, 0x000000, 0x30CED8, 0xFFFFFF}},
  {"Athletics", 133, 4, {0x003831, 0xEFB21E, 0xA2AAAD, 0xFFFFFF, 0x000000}},
  {"Atlanta Braves", 144, 4, {0xCE1141, 0x13274F, 0xEAAA00, 0xFFFFFF, 0x000000}},
  {"Baltimore Orioles", 110, 3, {0xDF4601, 0x000000, 0xFFFFFF, 0x000000, 0x000000}},
  {"Boston Red Sox", 111, 3, {0xBD3039, 0x0C2340, 0xFFFFFF, 0x000000, 0x000000}},
  {"Chicago Cubs", 112, 3, {0x0E3386, 0xCC3433, 0xFFFFFF, 0x000000, 0x000000}},
  {"Chicago White Sox", 145, 3, {0x27251F, 0xC4CED4, 0xFFFFFF, 0x000000, 0x000000}},
  {"Cincinnati Reds", 113, 3, {0xC6011F, 0x000000, 0xFFFFFF, 0x000000, 0x000000}},
  {"Cleveland Guardians", 114, 3, {0x00385D, 0xE50022, 0xFFFFFF, 0x000000, 0x000000}},
  {"Colorado Rockies", 115, 4, {0x333366, 0xC4CED4, 0x131413, 0xFFFFFF, 0x000000}},
  {"Detroit Tigers", 116, 3, {0x0C2340, 0xFA4616, 0xFFFFFF, 0x000000, 0x000000}},
  {"Houston Astros", 117, 4, {0x002D62, 0xEB6E1F, 0xF4911E, 0xFFFFFF, 0x000000}},
  {"Kansas City Royals", 118, 3, {0x004687, 0xBD9B60, 0xFFFFFF, 0x000000, 0x000000}},
  {"Los Angeles Angels", 108, 5, {0x003263, 0xBA0021, 0x862633, 0xC4CED4, 0xFFFFFF}},
  {"Los Angeles Dodgers", 119, 4, {0x005A9C, 0xEF3E42, 0xA5ACAF, 0xFFFFFF, 0x000000}},
  {"Miami Marlins", 146, 5, {0x00A3E0, 0xEF3340, 0x41748D, 0x000000, 0xFFFFFF}},
  {"Milwaukee Brewers", 158, 3, {0x12284B, 0xFFC52F, 0xFFFFFF, 0x000000, 0x000000}},
  {"Minnesota Twins", 142, 4, {0x002B5C, 0xD31145, 0xB9975B, 0xFFFFFF, 0x000000}},
  {"New York Mets", 121, 3, {0x002D72, 0xFF5910, 0xFFFFFF, 0x000000, 0x000000}},
  {"New York Yankees", 147, 5, {0x003087, 0xE4002C, 0x0C2340, 0xC4CED3, 0xFFFFFF}},
  {"Philadelphia Phillies", 143, 3, {0xE81828, 0x002D72, 0xFFFFFF, 0x000000, 0x000000}},
  {"Pittsburgh Pirates", 134, 3, {0x27251F, 0xFDB827, 0xFFFFFF, 0x000000, 0x000000}},
  {"San Diego Padres", 135, 3, {0x2F241D, 0xFFC425, 0xFFFFFF, 0x000000, 0x000000}},
  {"San Francisco Giants", 137, 5, {0xFD5A1E, 0x27251F, 0xEFD19F, 0xAE8F6F, 0xFFFFFF}},
  {"Seattle Mariners", 136, 5, {0x0C2C56, 0x005C5C, 0xC4CED4, 0xD50032, 0xFFFFFF}},
  {"St. Louis Cardinals", 138, 4, {0xC41E3A, 0x0C2340, 0xFEDB00, 0xFFFFFF, 0x000000}},
  {"Tampa Bay Rays", 139, 4, {0x092C5C, 0x8FBCE6, 0xF5D130, 0xFFFFFF, 0x000000}},
  {"Texas Rangers", 140, 3, {0x003278, 0xC0111F, 0xFFFFFF, 0x000000, 0x000000}},
  {"Toronto Blue Jays", 141, 4, {0x134A8E, 0x1D2D5C, 0xE8291C, 0xFFFFFF, 0x000000}},
  {"Washington Nationals", 120, 3, {0xAB0003, 0x14225A, 0xFFFFFF, 0x000000, 0x000000}}
};

static UsermodBaseballAPI baseball_api;
REGISTER_USERMOD(baseball_api);
#endif