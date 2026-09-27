#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "../net/wifi_scan.h"

// Shared chrome for both web surfaces. Everything is inline and hand-rolled:
// the setup portal runs with no internet at all, so a CDN stylesheet or font
// would simply hang.
namespace WebUi {
    extern const char HEAD_META[];
    extern const char STYLES[];

    String page(const String& title, const String& body);
    String brand(const char* badge = nullptr);

    // Escapes for embedding inside a single-quoted JS string literal, e.g.
    // an onclick="...'text'..." handler. The result still needs
    // htmlEscape() around the whole attribute value afterwards, since it
    // ends up inside a "..." HTML attribute.
    String jsEscape(const String& s);

    // A row of tappable "SSID (N dBm)" chips that fill the text input with
    // the given id when tapped - the mobile-webview-safe replacement for
    // <datalist>, which iOS's captive-portal browser mostly ignores. Used
    // by both provisioning's WiFi step and the admin page's WiFi section.
    // Returns an empty string when nets is empty; the caller supplies its
    // own "nothing found" message for that case.
    String wifiChips(const std::vector<ScannedNetwork>& nets, const char* targetInputId);

    // Registers GET /logo.png (64x64 favicon) and GET /banner.png (the admin
    // page header image) against the given server, serving the embedded PNGs
    // straight from flash. page() references /logo.png as the favicon and
    // brand() renders /banner.png as the page header, so every page that
    // calls either needs these routes registered once on its WebServer -
    // both provisioning and the admin page do their own setup.
    void registerLogoRoute(WebServer& server);
}
