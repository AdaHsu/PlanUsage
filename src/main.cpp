#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>

#include "core/carousel.h"
#include "core/settings.h"
#include "hal/battery.h"
#include "hal/board.h"
#include "net/ntp.h"
#include "net/wifi_manager.h"
#include "ui/display.h"
#include "ui/screens.h"
#include "web/provisioning.h"
#include "web/webconfig.h"

// --- button timing ---------------------------------------------------------
static const unsigned long SHORT_MIN_MS         = 50;
static const unsigned long DOUBLE_CLICK_MS      = 280;   // how long a KEY burst stays open
static const unsigned long SCREEN_SLEEP_HOLD_MS = 2000;
static const unsigned long RESET_HOLD_MS        = 10000;
static const unsigned long COMBO_HOLD_MS        = 5000;
static const unsigned long BOOT_FACTORY_HOLD_MS = 2000;

// --- countdowns ------------------------------------------------------------
static const unsigned long ARM_SHORT_MS  = 3000;
static const unsigned long ARM_LONG_MS   = 5000;

static const unsigned long SWITCH_OVERLAY_MS = 800;
static const int           WIFI_GIVE_UP_ROUNDS = 3;

enum class View { Stats, Info };
enum class SleepMode { Normal, Combo, Armed };
enum class PendingAction { None, BacklightOff, LightSleep, DeepSleep, Reboot };

static DeviceSettings cfg;
static Carousel       carousel;
static View           view = View::Stats;

static bool          screenOn          = true;
static bool          fetching          = false;
static unsigned long overlayUntilMs    = 0;
static unsigned long lastUiTickMs      = 0;
static unsigned long lastInteractionMs = 0;

// KEY presses are collected into a burst instead of acted on immediately, so
// one button can carry both actions: the burst ends DOUBLE_CLICK_MS after the
// last press, and its length decides what happened. Exactly two presses is
// the view toggle; anything else walks that many accounts, which keeps fast
// repeated presses doing the obvious thing instead of flipping views.
static int           pendingClicks = 0;
static unsigned long lastClickAt   = 0;
static unsigned long keyPressStart   = 0;
static bool          keyWasDown      = false;
static bool          keyLongFired    = false;
static bool          keyWakeOnly     = false;

static unsigned long bootPressStart = 0;
static bool          bootWasDown    = false;
static bool          bootLongFired  = false;
static bool          bootWakeOnly   = false;

static SleepMode     sleepMode         = SleepMode::Normal;
static PendingAction pendingAction     = PendingAction::None;
static unsigned long modeStartMs       = 0;
static int           lastCountdownSec  = -1;
static int           lastBootSecsShown = -1;
static unsigned long bootHoldStartMs   = 0;
static bool          comboReleasedOnce = false;

static bool inLightSleepCycle    = false;
static bool forceFetchOnNextTick = false;

static void enterLightSleep();
static void enterDeepSleep();

static String adminUrl() {
    return String("http://") + Board::HOSTNAME + ".local";
}

static void markInteraction() { lastInteractionMs = millis(); }

static void redraw() {
    if (!screenOn) return;
    if (millis() < overlayUntilMs) return;  // the switch overlay owns the screen

    int batMv = Battery::millivolts();

    if (view == View::Info) {
        Screens::DeviceInfo info;
        info.ssid       = WifiManager::ssid();
        info.rssi       = WifiManager::rssi();
        info.ip         = WifiManager::ip();
        info.host       = String(Board::HOSTNAME) + ".local";
        info.batteryMv  = batMv;
        info.uptimeMs   = millis();
        info.refreshMin = cfg.refreshMin;
        Screens::info(info);
        return;
    }

    Provider* p = carousel.active();
    if (!p) {
        Screens::noAccounts(adminUrl());
        return;
    }

    Screens::Chrome chrome;
    chrome.index      = carousel.activeIndex();
    chrome.total      = carousel.size();
    chrome.batteryMv  = batMv;
    chrome.refreshSec = carousel.secondsUntilRefresh(millis());
    chrome.loading    = fetching;
    chrome.wifiOk     = WifiManager::isConnected();
    // Numbers on screen but an error on the last attempt: say so rather than
    // letting a stale panel look live.
    chrome.stale      = p->hasData() && !p->lastError().isEmpty();

    Screens::stats(*p, chrome, time(nullptr));
}

// Only ever called for the account currently on screen.
static void doFetch(bool silent = false) {
    Provider* p = carousel.active();
    if (!p) return;
    if (!WifiManager::isConnected()) return;

    fetching = true;
    if (!silent) redraw();

    Serial.printf("[fetch] %s / %s\n", p->typeName(), p->label().c_str());
    FetchResult r = p->refresh(time(nullptr));
    Serial.printf("[fetch] result=%d\n", (int)r);

    // A module may have learned something it wants kept (a discovered org id,
    // a rotated token).
    carousel.persistDirtySettings();

    fetching = false;
    if (!silent) redraw();
}

static void nextAccount(int steps) {
    if (steps < 1) steps = 1;
    if (carousel.size() < 2) {
        view = View::Stats;
        redraw();
        return;
    }

    // Walking several rows at once still costs nothing: the settle timer is
    // restarted by each step, so no account in the middle is ever fetched.
    for (int i = 0; i < steps; i++) carousel.next();
    view = View::Stats;

    Provider* p = carousel.active();
    if (p) {
        Screens::switchOverlay(*p, carousel.activeIndex(), carousel.size());
        overlayUntilMs = millis() + SWITCH_OVERLAY_MS;
    }
}

static void toggleView() {
    view = (view == View::Stats) ? View::Info : View::Stats;
    overlayUntilMs = 0;
    redraw();
}

static void wakeScreen() {
    if (screenOn) return;
    screenOn = true;
    Display::setBacklight(true);
    // Leaving the light-sleep cycle: the next screen-off has to wait out the
    // idle timer again rather than dropping straight back to sleep.
    inLightSleepCycle = false;
    // Coming back from tier 1 or 2: the first frame the user sees should not
    // be built from whatever was cached before the screen went dark.
    forceFetchOnNextTick = true;
    redraw();
}

static void sleepScreen() {
    if (!screenOn) return;
    screenOn = false;
    Display::setBacklight(false);
}

// waitForRelease: a button is already held at the moment of arming, so the
// countdown must not accept it as the cancelling press.
static void armSleep(PendingAction action, bool waitForRelease) {
    sleepMode         = SleepMode::Armed;
    pendingAction     = action;
    modeStartMs       = millis();
    lastCountdownSec  = -1;
    lastBootSecsShown = -1;
    bootHoldStartMs   = 0;
    comboReleasedOnce = !waitForRelease;
    Display::setBacklight(true);
    screenOn = true;
}

// Returns true when the combo/armed machine consumed this tick, in which case
// the individual button handlers are skipped.
static bool handleSleepModes() {
    bool bp = digitalRead(Board::PIN_BOOT) == LOW;
    bool kp = digitalRead(Board::PIN_KEY)  == LOW;
    bool any  = bp || kp;
    bool both = bp && kp;
    unsigned long now = millis();

    switch (sleepMode) {
        case SleepMode::Normal:
            if (both) {
                sleepMode   = SleepMode::Combo;
                modeStartMs = now;
                // Drop any in-flight single-press state so the releases do not
                // also switch account or rotate.
                keyWasDown      = false;
                bootWasDown     = false;
                pendingClicks   = 0;
                return true;
            }
            return false;

        case SleepMode::Combo:
            if (!both) {
                sleepMode   = SleepMode::Normal;
                keyWasDown  = false;
                bootWasDown = false;
            } else if (now - modeStartMs >= COMBO_HOLD_MS) {
                armSleep(PendingAction::DeepSleep, true);
            }
            return true;

        case SleepMode::Armed: {
            if (!any) comboReleasedOnce = true;
            const bool isReboot = (pendingAction == PendingAction::Reboot);

            // While a reboot is armed, BOOT escalates to a factory reset.
            if (isReboot) {
                if (bp) {
                    if (bootHoldStartMs == 0) bootHoldStartMs = now;
                } else {
                    bootHoldStartMs = 0;
                }
                if (bootHoldStartMs && now - bootHoldStartMs >= BOOT_FACTORY_HOLD_MS) {
                    Serial.println("[reset] factory reset, clearing NVS");
                    Screens::resetting();
                    delay(500);
                    Settings::clearAll();
                    ESP.restart();
                }
            }

            bool cancelNow = isReboot ? (kp && comboReleasedOnce)
                                      : (any && comboReleasedOnce);
            if (cancelNow) {
                sleepMode      = SleepMode::Normal;
                pendingAction  = PendingAction::None;
                // Swallow the cancelling press so its release does nothing else.
                keyWasDown      = kp;
                bootWasDown     = bp;
                keyWakeOnly     = kp;
                bootWakeOnly    = bp;
                keyPressStart   = now;
                bootPressStart  = now;
                keyLongFired    = true;
                bootLongFired   = true;
                pendingClicks   = 0;
                bootHoldStartMs = 0;
                markInteraction();
                redraw();
                return true;
            }

            unsigned long duration = (pendingAction == PendingAction::DeepSleep ||
                                      pendingAction == PendingAction::Reboot)
                                   ? ARM_LONG_MS : ARM_SHORT_MS;
            unsigned long elapsed = now - modeStartMs;

            if (elapsed >= duration) {
                PendingAction a = pendingAction;
                sleepMode     = SleepMode::Normal;
                pendingAction = PendingAction::None;
                switch (a) {
                    case PendingAction::BacklightOff: sleepScreen(); break;
                    case PendingAction::LightSleep:
                        sleepScreen();
                        inLightSleepCycle = true;
                        enterLightSleep();
                        break;
                    case PendingAction::DeepSleep: enterDeepSleep(); break;
                    case PendingAction::Reboot:    ESP.restart();    break;
                    default: break;
                }
                return true;
            }

            int secsLeft = (int)((duration - elapsed + 999) / 1000);

            if (isReboot && bootHoldStartMs) {
                unsigned long held = now - bootHoldStartMs;
                int bootSecs = (int)((BOOT_FACTORY_HOLD_MS - held + 999) / 1000);
                if (bootSecs < 0) bootSecs = 0;
                if (bootSecs != lastBootSecsShown) {
                    Screens::sleepArmed("Factory reset in", bootSecs, "keep holding BOOT");
                    lastBootSecsShown = bootSecs;
                    lastCountdownSec  = -1;
                }
            } else if (secsLeft != lastCountdownSec) {
                const char* title    = "Sleeping in";
                const char* subtitle = nullptr;
                switch (pendingAction) {
                    case PendingAction::BacklightOff: title = "Screen off in";  break;
                    case PendingAction::LightSleep:   title = "Light sleep in"; break;
                    case PendingAction::DeepSleep:    title = "Deep sleep in";  break;
                    case PendingAction::Reboot:
                        title    = "Reboot in";
                        subtitle = "KEY cancel / BOOT reset";
                        break;
                    default: break;
                }
                Screens::sleepArmed(title, secsLeft, subtitle);
                lastCountdownSec  = secsLeft;
                lastBootSecsShown = -1;
            }
            return true;
        }
    }
    return false;
}

static void handleKeyButton() {
    bool pressed = digitalRead(Board::PIN_KEY) == LOW;
    unsigned long now = millis();

    if (pressed && !keyWasDown) {
        keyPressStart = now;
        keyWasDown    = true;
        keyLongFired  = false;
        keyWakeOnly   = !screenOn;
        wakeScreen();
        markInteraction();
    } else if (pressed && keyWasDown && !keyLongFired && !keyWakeOnly &&
               now - keyPressStart >= RESET_HOLD_MS) {
        keyLongFired    = true;
        pendingClicks   = 0;
        armSleep(PendingAction::Reboot, true);
    } else if (!pressed && keyWasDown) {
        unsigned long held = now - keyPressStart;
        keyWasDown = false;
        if (keyLongFired || keyWakeOnly || held < SHORT_MIN_MS) return;

        // Add it to the burst; what it means is decided once the burst ends.
        pendingClicks++;
        lastClickAt = now;
    }
}

static void handleBootButton() {
    bool pressed = digitalRead(Board::PIN_BOOT) == LOW;
    unsigned long now = millis();

    if (pressed && !bootWasDown) {
        bootPressStart = now;
        bootWasDown    = true;
        bootLongFired  = false;
        bootWakeOnly   = !screenOn;
        wakeScreen();
        markInteraction();
    } else if (pressed && bootWasDown && !bootLongFired && !bootWakeOnly &&
               now - bootPressStart >= SCREEN_SLEEP_HOLD_MS) {
        bootLongFired = true;
        armSleep(PendingAction::BacklightOff, true);
    } else if (!pressed && bootWasDown) {
        unsigned long held = now - bootPressStart;
        bootWasDown = false;
        if (bootLongFired || bootWakeOnly || held < SHORT_MIN_MS) return;

        int r = (Display::rotation() + 1) & 3;
        Display::setRotation(r);
        cfg.rotation = r;
        Settings::saveRotation(r);
        redraw();
    }
}

// A burst of KEY presses is acted on once it has clearly ended.
static void resolveClickBurst() {
    if (pendingClicks == 0) return;
    if (millis() - lastClickAt <= DOUBLE_CLICK_MS) return;

    int clicks    = pendingClicks;
    pendingClicks = 0;

    if (clicks == 2) toggleView();
    else             nextAccount(clicks);
}

static void configureButtonWakeup() {
    esp_sleep_enable_ext1_wakeup(
        (1ULL << Board::PIN_BOOT) | (1ULL << Board::PIN_KEY),
        ESP_EXT1_WAKEUP_ANY_LOW);
}

static void enterLightSleep() {
    Serial.println("[sleep] light sleep");
    Display::panelSleep(true);

    // Wake on the same cadence the screen would have refreshed at, so the
    // numbers are current when the user picks the device back up.
    uint64_t timerUs = (uint64_t)cfg.refreshMin * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(timerUs);
    configureButtonWakeup();

    esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    Display::panelSleep(false);
    Serial.printf("[sleep] light wake cause=%d\n", (int)cause);

    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        // Silent refresh of the account on screen, then straight back to sleep.
        if (WifiManager::isConnected()) doFetch(true);
        return;
    }

    // After a light sleep the GPIO pull-ups, the parallel bus and the WiFi
    // stack can all come back half-initialised, which users experience as
    // dead buttons. A clean restart is quick and always lands in a known state.
    Serial.println("[sleep] button wake, restarting");
    ESP.restart();
}

static void enterDeepSleep() {
    Serial.println("[sleep] deep sleep");
    Screens::deepSleep();
    delay(400);
    Display::panelSleep(true);
    digitalWrite(TFT_BL, LOW);
    digitalWrite(Board::PIN_LCD_POWER, LOW);

    configureButtonWakeup();
    esp_deep_sleep_start();
}

static bool connectWifi() {
    return WifiManager::connectAny(cfg, [](const String& ssid, int attempt, int total) {
        Screens::connecting(ssid, attempt, total);
    });
}

static void applySettings() {
    carousel.setRefreshMinutes(cfg.refreshMin);
    if (Display::rotation() != (cfg.rotation & 3)) {
        Display::setRotation(cfg.rotation);
    }
    redraw();
}

void setup() {
    Serial.begin(115200);
    Serial.printf("\n[boot] Plan Usage %s, wake cause=%d\n",
                  Board::VERSION, (int)esp_sleep_get_wakeup_cause());

    pinMode(Board::PIN_KEY,  INPUT_PULLUP);
    pinMode(Board::PIN_BOOT, INPUT_PULLUP);
    Battery::begin();
    Display::begin();

    Settings::load(cfg);
    Display::setRotation(cfg.rotation);
    Screens::splash();
    delay(700);

    // Account credentials live on LittleFS, not NVS - mount it (formatting on
    // first boot) before anything tries to read the account list.
    if (!Accounts::begin()) {
        Screens::message("Storage error", "LittleFS mount failed", Ui::COLOR_DANGER);
        delay(3000);
    }

    // No WiFi on file: the phone only ever has to type a network name here,
    // and the accounts get added later from a real keyboard.
    if (!cfg.hasWifi()) {
        cfg = Provisioning::run(cfg);
        delay(400);
        ESP.restart();
    }

    Screens::loading("Scanning WiFi");
    if (!connectWifi()) {
        Screens::message("No WiFi", "none of the saved networks answered", Ui::COLOR_DANGER);
        delay(2500);
        cfg = Provisioning::run(cfg);
        delay(400);
        ESP.restart();
    }

    Screens::loading("Syncing time");
    Ntp::sync();
    WifiManager::startMdns(Board::HOSTNAME);

    carousel.rebuild();
    carousel.setRefreshMinutes(cfg.refreshMin);

    WebConfig::Hooks hooks;
    hooks.onAccountsChanged = [] {
        carousel.rebuild();
        carousel.setRefreshMinutes(cfg.refreshMin);
        redraw();
    };
    hooks.onSettingsChanged = applySettings;
    hooks.onForceRefresh    = [] { carousel.forceFetchNext(); };
    WebConfig::begin(&cfg, &carousel, hooks);

    if (carousel.empty()) {
        Serial.printf("[boot] no accounts yet, add them at %s\n", adminUrl().c_str());
    } else {
        Screens::loading("Fetching usage");
        doFetch();
    }

    redraw();
    lastUiTickMs = millis();
    markInteraction();
}

void loop() {
    bool modeActive = handleSleepModes();
    if (!modeActive) {
        handleKeyButton();
        handleBootButton();
        resolveClickBurst();
    }

    WebConfig::loop();

    unsigned long nowMs = millis();

    // Reconnect in the background rather than rebooting: cached usage and the
    // carousel position both survive a flaky access point that way.
    if (!WifiManager::keepAlive(cfg) &&
        WifiManager::failedRounds() >= WIFI_GIVE_UP_ROUNDS) {
        WifiManager::resetFailures();
        // The admin server already owns port 80, so setup mode cannot start
        // from here. Restart instead: setup() retries the saved networks and
        // falls into the captive portal itself when they are all gone.
        Screens::message("WiFi lost", "restarting", Ui::COLOR_DANGER);
        delay(1500);
        ESP.restart();
    }

    if (!carousel.empty()) {
        // The two gates live in the carousel: stay on an account for 5 s, and
        // only then refetch if what is cached has gone stale.
        if (forceFetchOnNextTick) {
            forceFetchOnNextTick = false;
            carousel.forceFetchNext();
        }
        if (carousel.dueForFetch(nowMs)) {
            doFetch();
            lastUiTickMs = nowMs;
        }
    }

    // One redraw a second keeps the countdown and reset timers moving without
    // touching the network.
    if (nowMs - lastUiTickMs >= 1000) {
        if (screenOn && !modeActive) redraw();
        lastUiTickMs = nowMs;
    }

    // Tier 2: once the screen has been dark and untouched for long enough,
    // drop into light sleep and keep only the periodic refresh alive.
    if (!modeActive && !screenOn && cfg.idleSleepMin > 0) {
        if (inLightSleepCycle) {
            enterLightSleep();
        } else if (nowMs - lastInteractionMs >=
                   (unsigned long)cfg.idleSleepMin * 60UL * 1000UL) {
            armSleep(PendingAction::LightSleep, false);
        }
    }

    delay(10);
}
