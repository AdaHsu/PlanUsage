#pragma once
#include <Arduino.h>
#include <functional>
#include "../core/carousel.h"
#include "../core/settings.h"

// The admin page served on the device IP (and on planusage.local). Account
// rows render whatever form their provider module hands over, so this file
// never learns what a sessionKey or an access token is.
namespace WebConfig {
    struct Hooks {
        std::function<void()>    onAccountsChanged;  // rebuild the carousel
        std::function<void()>    onSettingsChanged;  // re-apply rotation, cadence
        std::function<void()>    onForceRefresh;     // ignore both fetch gates once
        std::function<void(int)> onSwitchTo;         // jump the device to account i
    };

    void begin(DeviceSettings* settings, Carousel* carousel, const Hooks& hooks);
    void loop();
}
