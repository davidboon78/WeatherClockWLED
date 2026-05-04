#pragma once
#ifdef USERMOD_TM1637_DISPLAY

/*
 * TM1637 Display Usermod — header stub
 *
 * The full implementation (class definition, display rendering, NTP time
 * display, weather data reception, and REGISTER_USERMOD) lives in
 * usermod_tm1637_display.cpp.  This file is intentionally minimal so the
 * WLED usermod loader's generated #include does not create a duplicate class
 * definition alongside the .cpp translation unit.
 *
 * Connections (NodeMCU v3):
 *   TM1637 CLK -> D5 (GPIO14)
 *   TM1637 DIO -> D6 (GPIO12)
 *   TM1637 VCC -> 3.3V
 *   TM1637 GND -> GND
 */

// Show a short 4-character message on the TM1637 display for a limited time.
// Returns true when the message was accepted by the display usermod instance.
bool tm1637DisplayShowMessage(const char* msg, uint16_t durationMs = 3000);

#endif // USERMOD_TM1637_DISPLAY
