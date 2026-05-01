# Baseball API Usermod for WLED

This usermod integrates the API-Sports Baseball API with WLED, allowing you to:
- Subscribe to your favorite baseball teams
- Display live scores and next-game info
- Poll the API hourly for next-game, and every 15s during live games
- Use WLED's timezone settings for accurate scheduling

## Features
- Favorite teams selection (up to 3, configurable)
- Efficient polling and JSON filtering
- Subscription/filter pattern (see weather_api usermod)
- Configurable via WLED Usermod UI

## Setup
1. Copy this folder to `usermods/baseball_api` in your WLED source tree.
2. Add your API key and favorite teams in the WLED Usermod config UI or via config file.
3. Enable the usermod in your build config (see platformio_override.ini).
4. Build and upload WLED firmware.

## API Key
Get your free API key at [api-sports.io](https://api-sports.io/).

## References
- [API-Sports Baseball API Docs](https://api-sports.io/documentation/baseball/v1)
- [WLED Usermod Guide](https://kno.wled.ge/development/usermods/)
- [weather_api usermod](../weather_api)
