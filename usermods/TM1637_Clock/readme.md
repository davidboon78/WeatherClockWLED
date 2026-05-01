# TM1637 Clock Display Usermod

This usermod adds support for a TM1637 4-digit 7-segment display that shows:
- Current time from NTP server (HH:MM format with blinking colon)  
- Status messages for different connection states

## Features

- **Time Display**: Shows current time in 12-hour format with AM/PM indicator
- **Blinking Colon**: Colon blinks every second to indicate time updates
- **Status Messages**:
  - `WiFi` - When WiFi is not connected
  - `noIP` - When WiFi is connected but no internet access
  - `ntP.` - When internet is available but NTP is not configured
  - `----` - When time is not yet synchronized

## Wiring (NodeMCU v3)

| TM1637 Pin | NodeMCU v3 Pin | GPIO | Description |
|------------|----------------|------|-------------|
| VCC        | 3.3V          | -    | Power (3.3V or 5V) |
| GND        | GND           | -    | Ground |
| CLK        | D5            | GPIO14 | Clock signal |
| DIO        | D6            | GPIO12 | Data signal |

## Configuration

The usermod can be configured through the WLED web interface:

- **CLK Pin**: Clock signal pin (default: GPIO14/D5)
- **DIO Pin**: Data signal pin (default: GPIO12/D6) 
- **Brightness**: Display brightness level (0-7, default: 2)
- **Enabled**: Enable/disable the usermod

## Usage

1. Connect the TM1637 display according to the wiring diagram above
2. The display will automatically show status messages during boot
3. Once WiFi is connected and NTP is synced, it will display the current time
4. Configure NTP server in WLED Settings > Time & Macros > Time zone/UTC offset

## Dependencies

- `avishorp/TM1637@^1.2.0` - TM1637 display library (automatically included)

## Notes

- The display uses 12-hour time format
- Pin assignments can be changed in the WLED web interface
- The usermod respects WLED's pin allocation system to avoid conflicts