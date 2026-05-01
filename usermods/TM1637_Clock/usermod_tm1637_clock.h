#pragma once
#ifdef USERMOD_TM1637_CLOCK

/*
 * TM1637 Clock Usermod — header stub
 *
 * The full implementation (class definition, weather fetching, LED patterns,
 * config persistence, and REGISTER_USERMOD) lives in usermod_tm1637_clock.cpp.
 * This file is intentionally minimal so the WLED usermod loader's generated
 * #include does not create a duplicate class definition alongside the .cpp
 * translation unit.
 *
 * Connections (NodeMCU v3):
 * TM1637 CLK -> D5 (GPIO14)
 * TM1637 DIO -> D6 (GPIO12)
 * TM1637 VCC -> 3.3V
 * TM1637 GND -> GND
 */
#endif // USERMOD_TM1637_CLOCK
