#pragma once
#include <Arduino.h>

// LilyGo T-Display S3 pin map. The TFT data/control pins live in
// platformio.ini as TFT_eSPI build flags; only the ones we drive ourselves
// are listed here.
namespace Board {
    static const int PIN_KEY       = 14;  // active low, internal pull-up
    static const int PIN_BOOT      = 0;   // active low
    static const int PIN_LCD_POWER = 15;  // must be HIGH or the panel is dark on battery
    static const int PIN_BAT_ADC   = 4;   // battery voltage through a 2:1 divider

    static const char* const HOSTNAME = "planusage";
    static const char* const AP_SSID  = "Plan Usage";
    static const char* const VERSION  = "0.1";
}
