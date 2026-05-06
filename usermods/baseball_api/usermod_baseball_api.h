#pragma once
#ifdef USERMOD_BASEBALL_API

#include "wled.h"

class UsermodBaseballAPI : public Usermod {
private:
  static const uint32_t TEAM_COLOR_BLANK = 0xFFFFFFFF;
  static const char _name[];
  static const char _enabledKey[];
  static const char _teamKey[];
  static const char _overrideKey[];
  static const char _paletteKey[];
  static const char _statusKey[];

  bool enabled = false;
  bool initDone = false;
  unsigned long lastFetch = 0;
  unsigned long intervalMs = 600000;
  bool wasConnected = false;

  String favoriteTeam = "";
  bool overrideLightsOnGame = false;

  bool gameLive = false;
  int liveGamePk = 0;
  String lastScore = "No data";
  String nextGameInfo = "";
  String lastRequestUrl = "";
  String lastLiveDataUrl = "";
  String lastFetchError = "";
  int lastHttpCode = 0;

  uint8_t savedMode = 0;
  uint8_t savedPalette = 0;
  uint8_t savedSpeed = 128;
  uint8_t savedIntensity = 128;
  bool gameOverrideActive = false;
  bool favoriteIsHome = false;
  int8_t teamPaletteIndex = -1;
  String teamPaletteName = "";

  struct TeamMap {
    const char* teamName;
    int mlbId;
    uint8_t colorCount;
    uint32_t colors[5];
  };
  static const TeamMap mlbMap[];

  const TeamMap* _resolveTeamMap(int mlbId) const;
  int _resolveMlbId() const;
  const char* _resolveTeamName(int mlbId) const;
  String _buildTeamPaletteName(const TeamMap& team) const;
  uint8_t _buildEffectiveTeamColors(const TeamMap& team, uint32_t outColors[5]) const;
  void _loadPaletteColors(CRGBPalette16& palette, const TeamMap& team) const;
  void _removeTeamPalette();
  void _ensureTeamPalette();
  bool _hasValidClock() const;
  time_t _currentUtcApprox() const;
  int32_t _currentLocalOffsetSecs() const;
  bool _parseApiUtc(const String& apiDate, time_t& utcTime) const;
  String _formatApiUtcToLocal(const String& apiDate) const;
  unsigned long _upcomingPollInterval(time_t gameUtcTime) const;
  bool _isLiveState(const String& abstractState, const String& codedState) const;
  bool _isFinalState(const String& abstractState, const String& codedState) const;
  bool _isGameHappeningNow(time_t nowUtc, time_t gameUtcTime) const;
  String _formatUtcDebug(time_t t) const;
  String _buildDateYmd(int dayOffset) const;
  String _buildScheduleUrl(int mlbId) const;
  String _buildLiveDataUrl(int gamePk) const;
  String _buildStatusLine() const;
  String _buildPaletteDisplay() const;
  String _buildStatusValue() const;

  void _doFetch();
  void _parseMLB(Stream& stream);
  void _applyGameOverride();
  void _restoreGameOverride();

public:
  bool isGameLive() const { return gameLive; }
  const String& getLastScore() const { return lastScore; }
  const String& getFavoriteTeam() const { return favoriteTeam; }
  bool isFavoriteHomeTeam() const { return favoriteIsHome; }
  bool isGameOverrideActive() const { return gameOverrideActive; }

  void setup() override;
  void loop() override;
  void addToJsonInfo(JsonObject& root) override;
  void addToConfig(JsonObject& root) override;
  bool readFromConfig(JsonObject& root) override;
  void appendConfigData() override;
  uint16_t getId() override { return USERMOD_ID_BASEBALL_API; }
};

#endif // USERMOD_BASEBALL_API
