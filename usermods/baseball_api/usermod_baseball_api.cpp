#include "wled.h"
#ifdef USERMOD_BASEBALL_API

#ifdef ESP8266
  #include <ESP8266HTTPClient.h>
#else
  #include <HTTPClient.h>
#endif
#include <WiFiClient.h>


#include "usermod_baseball_api.h"


const char UsermodBaseballAPI::_name[] PROGMEM = "BaseballAPI";
const char UsermodBaseballAPI::_keyEnabled[] PROGMEM = "enabled";
const char UsermodBaseballAPI::_keyApiKey[]  PROGMEM = "apiKey";
const char UsermodBaseballAPI::_keyTeam[]    PROGMEM = "team";
const char UsermodBaseballAPI::_keyInterval[]PROGMEM = "interval";

// Source: https://teamcolorcodes.com/mlb-color-codes/ (accessed 2026-04-30)
// Stores commonly used MLB abbreviations plus primary/secondary team colors as 0xRRGGBB.
static const BaseballTeamStyle baseballTeamStyles[] = {
  {"2",  "Arizona Diamondbacks", "ARI", 0xA71930, 0xE3D4AD},
  {"3",  "Atlanta Braves",       "ATL", 0xCE1141, 0x13274F},
  {"4",  "Baltimore Orioles",    "BAL", 0xDF4601, 0x000000},
  {"5",  "Boston Red Sox",       "BOS", 0xBD3039, 0x0C2340},
  {"6",  "Chicago Cubs",         "CHC", 0x0E3386, 0xCC3433},
  {"7",  "Chicago White Sox",    "CHW", 0x27251F, 0xC4CED4},
  {"8",  "Cincinnati Reds",      "CIN", 0xC6011F, 0x000000},
  {"9",  "Cleveland Guardians",  "CLE", 0x00385D, 0xE50022},
  {"10", "Colorado Rockies",     "COL", 0x333366, 0xC4CED4},
  {"12", "Detroit Tigers",       "DET", 0x0C2340, 0xFA4616},
  {"15", "Houston Astros",       "HOU", 0x002D62, 0xEB6E1F},
  {"16", "Kansas City Royals",   "KC",  0x004687, 0xBD9B60},
  {"17", "Los Angeles Angels",   "LAA", 0x003263, 0xBA0021},
  {"18", "Los Angeles Dodgers",  "LAD", 0x005A9C, 0xEF3E42},
  {"19", "Miami Marlins",        "MIA", 0x00A3E0, 0xEF3340},
  {"20", "Milwaukee Brewers",    "MIL", 0x12284B, 0xFFC52F},
  {"22", "Minnesota Twins",      "MIN", 0x002B5C, 0xD31145},
  {"24", "New York Mets",        "NYM", 0x002D72, 0xFF5910},
  {"25", "New York Yankees",     "NYY", 0x003087, 0xE4002C},
  {"26", "Oakland Athletics",    "OAK", 0x003831, 0xEFB21E},
  {"27", "Philadelphia Phillies", "PHI", 0xE81828, 0x002D72},
  {"28", "Pittsburgh Pirates",   "PIT", 0x27251F, 0xFDB827},
  {"30", "San Diego Padres",     "SD",  0x2F241D, 0xFFC425},
  {"31", "San Francisco Giants", "SF",  0xFD5A1E, 0x27251F},
  {"32", "Seattle Mariners",     "SEA", 0x0C2C56, 0x005C5C},
  {"33", "St. Louis Cardinals",  "STL", 0xC41E3A, 0x0C2340},
  {"34", "Tampa Bay Rays",       "TB",  0x092C5C, 0x8FBCE6},
  {"35", "Texas Rangers",        "TEX", 0x003278, 0xC0111F},
  {"36", "Toronto Blue Jays",    "TOR", 0x134A8E, 0x1D2D5C},
  {"37", "Washington Nationals", "WSH", 0xAB0003, 0x14225A}
};

static constexpr size_t baseballTeamStyleCount = sizeof(baseballTeamStyles) / sizeof(baseballTeamStyles[0]);



void UsermodBaseballAPI::setup() {
  loadConfig();
  updateTimezone();
  _fetchPending = true;
}

void UsermodBaseballAPI::loop() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  bool due = _fetchPending || (now - _lastFetch >= _intervalMs);
  if (!due) return;
  _fetchPending = false;
  _lastFetch = now;
  _doFetch();
}

void UsermodBaseballAPI::triggerFetch() {
  _fetchPending = true;
}

bool UsermodBaseballAPI::subscribe(const char* path, BaseballValueCallback callback) {
  if (_subCount >= BASEBALL_API_MAX_SUBS) return false;
  _subs[_subCount].path = path;
  _subs[_subCount].callback = callback;
  _subCount++;
  return true;
}

const BaseballTeamStyle* UsermodBaseballAPI::_findTeamStyle(const String& teamId) {
  for (size_t index = 0; index < baseballTeamStyleCount; index++) {
    if (teamId.equals(baseballTeamStyles[index].teamId)) return &baseballTeamStyles[index];
  }
  return nullptr;
}

const char* UsermodBaseballAPI::getTeamInitials(const String& teamId) const {
  const BaseballTeamStyle* style = _findTeamStyle(teamId);
  return style ? style->initials : "MLB";
}

const char* UsermodBaseballAPI::getFavoriteTeamInitials() const {
  return getTeamInitials(favoriteTeam);
}

bool UsermodBaseballAPI::getTeamColors(const String& teamId, uint32_t& primaryColor, uint32_t& secondaryColor) const {
  const BaseballTeamStyle* style = _findTeamStyle(teamId);
  if (!style) return false;
  primaryColor = style->primaryColor;
  secondaryColor = style->secondaryColor;
  return true;
}

bool UsermodBaseballAPI::getFavoriteTeamColors(uint32_t& primaryColor, uint32_t& secondaryColor) const {
  return getTeamColors(favoriteTeam, primaryColor, secondaryColor);
}

void UsermodBaseballAPI::_setFilterPath(JsonObject root, const char* path) {
  char buf[64];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  JsonObject cur = root;
  char* seg = strtok(buf, ".");
  while (seg) {
    char* next = strtok(nullptr, ".");
    if (next == nullptr) {
      cur[seg] = true;
    } else {
      if (cur[seg].is<JsonObject>()) {
        cur = cur[seg].as<JsonObject>();
      } else {
        cur = cur.createNestedObject(seg);
      }
    }
    seg = next;
  }
}

JsonVariant UsermodBaseballAPI::_getByPath(JsonVariant root, const char* path) {
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

void UsermodBaseballAPI::_doFetch() {
  if (_subCount == 0) return;
  if (favoriteTeam.length() == 0 || apiKey.length() < 10) {
    _lastError = "No API key or team";
    _lastFetchOk = false;
    return;
  }
  String url = "https://v1.baseball.api-sports.io/games?team=" + favoriteTeam + "&next=1&timezone=" + timezone;
  strncpy(_lastUrl, url.c_str(), sizeof(_lastUrl) - 1);
  _lastUrl[sizeof(_lastUrl) - 1] = '\0';
  WiFiClient wifiClient;
  HTTPClient http;
  if (!http.begin(wifiClient, url)) {
    _lastError = "HTTP begin failed";
    _lastFetchOk = false;
    return;
  }
  http.addHeader("x-apisports-key", apiKey);
  http.setTimeout(5000);
  int httpCode = http.GET();
  if (httpCode != 200) {
    _lastError = String("HTTP ") + httpCode;
    _lastFetchOk = false;
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  DynamicJsonDocument filterDoc(BASEBALL_API_FILTER_DOC_SIZE);
  JsonObject filterRoot = filterDoc.to<JsonObject>();
  for (uint8_t i = 0; i < _subCount; i++) {
    _setFilterPath(filterRoot, _subs[i].path);
  }
  DynamicJsonDocument doc(BASEBALL_API_RESPONSE_DOC_SIZE);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filterDoc));
  if (err) {
    _lastError = err.c_str();
    _lastFetchOk = false;
    return;
  }
  JsonVariant docVar = doc.as<JsonVariant>();
  for (uint8_t i = 0; i < _subCount; i++) {
    JsonVariant val = _getByPath(docVar, _subs[i].path);
    if (!val.isNull() && _subs[i].callback) {
      _subs[i].callback(val);
    }
  }
  _lastFetchOk = true;
  _lastError = "";
}

void UsermodBaseballAPI::requestNextGame() {
  String url = "https://v1.baseball.api-sports.io/games?team=" + favoriteTeam + "&next=1&timezone=" + timezone;
  WiFiClient wifiClient;
  HTTPClient http;
  if (!http.begin(wifiClient, url)) return;
  http.addHeader("x-apisports-key", apiKey);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    _handleApiResponse(payload);
  }
  http.end();
}

void UsermodBaseballAPI::requestLiveScore(const String& gameId) {
  String url = "https://v1.baseball.api-sports.io/games?id=" + gameId + "&timezone=" + timezone;
  WiFiClient wifiClient;
  HTTPClient http;
  if (!http.begin(wifiClient, url)) return;
  http.addHeader("x-apisports-key", apiKey);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    _handleApiResponse(payload);
  }
  http.end();
}

void UsermodBaseballAPI::_handleApiResponse(const String& json) {
  // Parse JSON and update state
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;
  JsonArray games = doc["response"];
  if (!games.isNull() && games.size() > 0) {
    JsonObject game = games[0];
    String status = game["status"].as<String>();
    String gameId = String(game["id"].as<int>());
    String home = game["teams"]["home"]["name"].as<String>();
    String away = game["teams"]["away"]["name"].as<String>();
    String score = String(game["scores"]["home"]["total"].as<int>()) + "-" + String(game["scores"]["away"]["total"].as<int>());
    String start = game["date"].as<String>();
    if (status == "NS") { // Not Started
      showNextGame(home + " vs " + away + " @ " + start);
      setPollInterval(false);
      gameLive = false;
      lastGameId = gameId;
    } else if (status == "1H" || status == "2H" || status == "LIVE" || status == "INPROGRESS") {
      showScore(home + " vs " + away + ": " + score + " (" + status + ")");
      setPollInterval(true);
      gameLive = true;
      lastGameId = gameId;
    } else if (status == "FT" || status == "POSTPONED" || status == "CANCELLED") {
      showScore(home + " vs " + away + ": Final " + score);
      setPollInterval(false);
      gameLive = false;
      lastGameId = "";
    }
  }
}

void UsermodBaseballAPI::setPollInterval(bool live) {
  _intervalMs = live ? 15000 : 3600000;
}

void UsermodBaseballAPI::updateTimezone() {
  timezone = getIanaTimezone(currentTimezone);
}

String UsermodBaseballAPI::getIanaTimezone(uint8_t wledTzId) {
  // Map WLED timezone ID to IANA string (partial example)
  switch (wledTzId) {
    case 0: return "Europe/London";
    case 1: return "Europe/Berlin";
    case 2: return "Europe/Helsinki";
    case 3: return "Europe/Moscow";
    case 4: return "Asia/Dubai";
    case 5: return "Asia/Karachi";
    case 6: return "Asia/Dhaka";
    case 7: return "Asia/Bangkok";
    case 8: return "Asia/Hong_Kong";
    case 9: return "Asia/Tokyo";
    case 10: return "Australia/Sydney";
    case 11: return "Pacific/Auckland";
    case 12: return "America/Anchorage";
    case 13: return "America/Los_Angeles";
    case 14: return "America/Denver";
    case 15: return "America/Chicago";
    case 16: return "America/New_York";
    default: return "UTC";
  }
}

void UsermodBaseballAPI::showScore(const String& score) {
  // TODO: Display score on LEDs or UI
  lastScore = score;
  // Example: Serial.println(score);
}

void UsermodBaseballAPI::showNextGame(const String& info) {
  // TODO: Display next game info on LEDs or UI
  // Example: Serial.println(info);
}


void UsermodBaseballAPI::addToConfig(JsonObject& root) {
  JsonObject mod = root.createNestedObject(FPSTR(_name));
  mod[FPSTR(_keyEnabled)]  = _enabled;
  mod[FPSTR(_keyApiKey)]   = apiKey;
  mod[FPSTR(_keyTeam)]     = favoriteTeam;
  mod[FPSTR(_keyInterval)] = _intervalMs / 1000UL;
}


bool UsermodBaseballAPI::readFromConfig(JsonObject& root) {
  JsonObject mod = root[FPSTR(_name)];
  if (mod.isNull()) return false;
  bool ok = true;
  ok &= getJsonValue(mod[FPSTR(_keyEnabled)],  _enabled);
  apiKey = mod[FPSTR(_keyApiKey)] | apiKey;
  favoriteTeam = mod[FPSTR(_keyTeam)] | favoriteTeam;
  unsigned long interval = mod[FPSTR(_keyInterval)] | (_intervalMs / 1000UL);
  if (interval < 15) interval = 15;
  _intervalMs = interval * 1000UL;
  return ok;
}

void UsermodBaseballAPI::appendConfigData() {
  oappend(F("addInfo('BaseballAPI:enabled',1,'Enable baseball schedule and score lookups');"));
  oappend(F("addInfo('BaseballAPI:apiKey',1,'API-Sports key');"));
  oappend(F("addInfo('BaseballAPI:team',1,'MLB team');"));
  oappend(F("addInfo('BaseballAPI:interval',1,'seconds (min 15)');"));
  oappend(F("(function(){"));
  oappend(F("var k=document.getElementsByName('BaseballAPI:apiKey');"));
  oappend(F("if(k&&k.length>1&&k[1]){k[1].type='password';k[1].placeholder='API-Sports key';k[1].style.width='250px';}"));
  oappend(F("var dd=addDropdown('BaseballAPI','team');if(!dd)return;"));
  oappend(F("addOption(dd,'Select MLB team','');"));
  oappend(F("addOption(dd,'Arizona Diamondbacks','2');"));
  oappend(F("addOption(dd,'Atlanta Braves','3');"));
  oappend(F("addOption(dd,'Baltimore Orioles','4');"));
  oappend(F("addOption(dd,'Boston Red Sox','5');"));
  oappend(F("addOption(dd,'Chicago Cubs','6');"));
  oappend(F("addOption(dd,'Chicago White Sox','7');"));
  oappend(F("addOption(dd,'Cincinnati Reds','8');"));
  oappend(F("addOption(dd,'Cleveland Guardians','9');"));
  oappend(F("addOption(dd,'Colorado Rockies','10');"));
  oappend(F("addOption(dd,'Detroit Tigers','12');"));
  oappend(F("addOption(dd,'Houston Astros','15');"));
  oappend(F("addOption(dd,'Kansas City Royals','16');"));
  oappend(F("addOption(dd,'Los Angeles Angels','17');"));
  oappend(F("addOption(dd,'Los Angeles Dodgers','18');"));
  oappend(F("addOption(dd,'Miami Marlins','19');"));
  oappend(F("addOption(dd,'Milwaukee Brewers','20');"));
  oappend(F("addOption(dd,'Minnesota Twins','22');"));
  oappend(F("addOption(dd,'New York Mets','24');"));
  oappend(F("addOption(dd,'New York Yankees','25');"));
  oappend(F("addOption(dd,'Oakland Athletics','26');"));
  oappend(F("addOption(dd,'Philadelphia Phillies','27');"));
  oappend(F("addOption(dd,'Pittsburgh Pirates','28');"));
  oappend(F("addOption(dd,'San Diego Padres','30');"));
  oappend(F("addOption(dd,'San Francisco Giants','31');"));
  oappend(F("addOption(dd,'Seattle Mariners','32');"));
  oappend(F("addOption(dd,'St. Louis Cardinals','33');"));
  oappend(F("addOption(dd,'Tampa Bay Rays','34');"));
  oappend(F("addOption(dd,'Texas Rangers','35');"));
  oappend(F("addOption(dd,'Toronto Blue Jays','36');"));
  oappend(F("addOption(dd,'Washington Nationals','37');"));
  oappend(F("dd.style.minWidth='250px';"));
  oappend(F("})();"));
}

void UsermodBaseballAPI::addToJsonInfo(JsonObject& root) {
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");
  JsonArray arr = user.createNestedArray(FPSTR(_name));
  if (!_lastFetchOk) {
    if (_lastError.length() > 0) arr.add("Error: " + _lastError);
    else arr.add("No data");
  } else {
    arr.add("Last fetch OK");
  }
  if (_lastUrl[0] != '\0') {
    String urlEntry = F("url:");
    urlEntry += _lastUrl;
    arr.add(urlEntry);
  }
}

void UsermodBaseballAPI::loadConfig() {
  // TODO: Load config from persistent storage if needed
}

void UsermodBaseballAPI::saveConfig() {
  // TODO: Save config to persistent storage if needed
}

// Static registration for WLED usermod system
static UsermodBaseballAPI usermod_baseball_api;
REGISTER_USERMOD(usermod_baseball_api);

#endif