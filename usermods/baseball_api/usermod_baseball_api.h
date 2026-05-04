#pragma once
#ifdef USERMOD_BASEBALL_API

#include "wled.h"

class UsermodBaseballAPI : public Usermod {
private:
  static const char _name[];
  static const char _enabledKey[];
  static const char _teamKey[];
  static const char _overrideKey[];

  bool enabled = false;
  bool initDone = false;
  unsigned long lastFetch = 0;
  unsigned long intervalMs = 600000;

  String favoriteTeam = "";
  bool overrideLightsOnGame = false;

  bool gameLive = false;
  String lastScore = "No data";
  String nextGameInfo = "";
  String lastRequestUrl = "";
  String lastFetchError = "";
  int lastHttpCode = 0;

  uint8_t savedMode = 0;
  uint8_t savedPalette = 0;
  uint8_t savedSpeed = 128;
  uint8_t savedIntensity = 128;
  bool gameOverrideActive = false;
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
  String _buildTeamPaletteName(const TeamMap& team) const;
  void _loadPaletteColors(CRGBPalette16& palette, const TeamMap& team) const;
  void _removeTeamPalette();
  void _ensureTeamPalette();
  String _buildDateYmd(int dayOffset) const;
  String _buildScheduleUrl(int mlbId) const;

  void _doFetch();
  void _parseMLB(const String& json);
  void _applyGameOverride();
  void _restoreGameOverride();

public:
  bool isGameLive() const { return gameLive; }
  const String& getLastScore() const { return lastScore; }
  const String& getFavoriteTeam() const { return favoriteTeam; }
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
