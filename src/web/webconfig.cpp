#include "webconfig.h"
#include "webui.h"
#include "../core/account.h"
#include "../core/registry.h"
#include "../hal/battery.h"
#include "../hal/board.h"
#include "../net/wifi_scan.h"
#include "../util/timefmt.h"
#include <WebServer.h>
#include <WiFi.h>

namespace {

WebServer       server(80);
DeviceSettings* cfg      = nullptr;
Carousel*       wheel    = nullptr;
WebConfig::Hooks hooks;

// A pending "add account" flow: the credential the user pasted lives here
// rather than in a hidden form field, so the secret never reaches the page.
String                 pendingType;
String                 pendingSettings;
std::vector<Workspace> pendingWorkspaces;

// Populated only on an explicit "Scan for networks" click, never on a plain
// page load: the device is already associated when this page is served, and
// scanning briefly hops channels, causing a small connectivity hiccup that
// should not happen just from opening the admin page.
std::vector<ScannedNetwork> scannedWifi;

// Bridges WebServer arguments into the FormSource the modules consume.
class ServerForm : public FormSource {
public:
    bool   has(const char* name) const override { return server.hasArg(name); }
    String get(const char* name) const override { return server.arg(name); }
};

bool requireAuth() {
    if (server.authenticate(cfg->adminUser.c_str(), cfg->adminPassword.c_str())) return true;
    server.requestAuthentication(BASIC_AUTH, "Plan Usage", "Authentication required");
    return false;
}

void redirectHome(const String& query = String()) {
    server.sendHeader("Location", query.isEmpty() ? "/" : ("/?" + query));
    server.send(303, "text/plain", "");
}

// How recently this row was looked at. Rows that are not on screen are never
// refreshed, so "not looked at yet" is a normal state, not a fault.
String statusOf(Provider* live) {
    if (!live) return String("unknown type");
    if (live->needsAuth()) return String("needs re-auth");
    if (!live->hasFetched()) return String("not looked at yet");

    unsigned long ageSec = (millis() - live->lastFetchMs()) / 1000;
    if (ageSec < 60) return "updated " + String(ageSec) + "s ago";
    return "updated " + String(ageSec / 60) + "m ago";
}

}  // namespace

namespace {

String sectionStatus() {
    int mv = Battery::millivolts();

    String s = "<section><h2>Device</h2>";
    s += "<div class=row><div class=grow><div class=name>";
    if (WiFi.status() == WL_CONNECTED) {
        s += htmlEscape(WiFi.SSID());
        s += "</div><div class=sub>";
        s += WiFi.localIP().toString() + " &middot; " + String(Board::HOSTNAME) + ".local &middot; ";
        s += String(WiFi.RSSI()) + " dBm";
    } else {
        s += "Offline</div><div class=sub>no WiFi link";
    }
    s += "</div></div></div>";

    char bat[64];
    if (Battery::isCharging(mv)) {
        snprintf(bat, sizeof(bat), "USB-C charging &middot; %d.%02d V", mv / 1000, (mv / 10) % 100);
    } else {
        snprintf(bat, sizeof(bat), "%d%% &middot; %d.%02d V",
                 Battery::percent(mv), mv / 1000, (mv / 10) % 100);
    }
    s += "<div class=row><div class=grow><div class=name>Battery</div>"
         "<div class=sub>";
    s += bat;
    s += "</div></div><span class=pill>up ";
    s += TimeFmt::uptime(millis());
    s += "</span></div>";

    if (wheel && !wheel->empty()) {
        Provider* p = wheel->active();
        s += "<div class=row><div class=grow><div class=name>Showing ";
        s += String(wheel->activeIndex() + 1) + "/" + String(wheel->size());
        s += "</div><div class=sub>";
        s += htmlEscape(p ? p->label() : String("-"));
        s += "</div></div>";
        s += "<form class=inline method=post action=/refresh>"
             "<button class=\"btnGhost mini\" type=submit>Refresh now</button></form></div>";
    }
    s += "</section>";
    return s;
}

String sectionAccounts(const std::vector<AccountRecord>& records) {
    String s = "<section><h2>Accounts</h2>";

    if (records.empty()) {
        s += "<p class=hint>No accounts yet. Add one below; the device shows whichever "
             "one you switch to with the KEY button.</p>";
    }

    bool isActive = false;
    for (size_t i = 0; i < records.size(); i++) {
        Provider* live = wheel ? wheel->at((int)i) : nullptr;
        isActive = wheel && wheel->activeIndex() == (int)i;

        s += "<div class=row><div class=grow><div class=name>";
        s += htmlEscape(records[i].label.isEmpty() ? String("(unnamed)") : records[i].label);
        s += " <span class=pill>";
        s += htmlEscape(live ? String(live->typeName()) : records[i].type);
        s += "</span>";
        if (live && !live->subLabel().isEmpty()) {
            s += " <span class=pill>" + htmlEscape(live->subLabel()) + "</span>";
        }
        if (isActive) {
            s += " <span class=\"pill active\">on screen</span>";
        }
        s += "</div><div class=sub>";
        if (live && !live->credentialHint().isEmpty()) {
            s += htmlEscape(live->credentialHint()) + " &middot; ";
        }
        bool warn = live && live->needsAuth();
        s += warn ? "<span class=\"pill warn\">" : "";
        s += htmlEscape(statusOf(live));
        s += warn ? "</span>" : "";
        s += "</div></div>";

        s += "<form class=inline method=post action=/account/move>"
             "<input type=hidden name=i value=" + String(i) + ">"
             "<input type=hidden name=d value=-1>"
             "<button class=\"btnGhost mini\" type=submit>&uarr;</button></form>";
        s += "<form class=inline method=post action=/account/move>"
             "<input type=hidden name=i value=" + String(i) + ">"
             "<input type=hidden name=d value=1>"
             "<button class=\"btnGhost mini\" type=submit>&darr;</button></form>";
        if (!isActive) {
            s += "<form class=inline method=post action=/account/switch>"
                 "<input type=hidden name=i value=" + String(i) + ">"
                 "<button class=\"btnGhost mini\" type=submit>Show</button></form>";
        }
        s += "<a class=\"btn btnGhost mini\" href=\"/account/edit?i=" + String(i) + "\">Edit</a>";
        s += "<form class=inline method=post action=/account/delete "
             "onsubmit=\"return confirm('Delete this account?')\">"
             "<input type=hidden name=i value=" + String(i) + ">"
             "<button class=\"btnDanger mini\" type=submit>Delete</button></form>";
        s += "</div>";
    }

    s += "<div class=actions>";
    for (const auto& t : Registry::types()) {
        s += "<a class=\"btn btnGhost\" href=\"/account/new?type=";
        s += t.id;
        s += "\">Add ";
        s += t.name;
        s += "</a>";
    }
    s += "</div>";
    s += "<p class=hint>The list order is the order the KEY button walks through. "
         "Only the account on screen is ever fetched.</p>";
    s += "</section>";
    return s;
}

}  // namespace

namespace {

String sectionWifi() {
    String s = "<section><h2>WiFi</h2>";
    for (int i = 0; i < cfg->wifiCount; i++) {
        s += "<div class=row><div class=grow><div class=name>";
        s += htmlEscape(cfg->wifis[i].ssid);
        s += "</div><div class=sub>priority " + String(i + 1);
        if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == cfg->wifis[i].ssid) {
            s += " &middot; connected";
        }
        s += "</div></div>";
        s += "<form class=inline method=post action=/wifi/move>"
             "<input type=hidden name=i value=" + String(i) + ">"
             "<input type=hidden name=d value=-1>"
             "<button class=\"btnGhost mini\" type=submit>&uarr;</button></form>";
        s += "<form class=inline method=post action=/wifi/move>"
             "<input type=hidden name=i value=" + String(i) + ">"
             "<input type=hidden name=d value=1>"
             "<button class=\"btnGhost mini\" type=submit>&darr;</button></form>";
        s += "<form class=inline method=post action=/wifi/delete>"
             "<input type=hidden name=i value=" + String(i) + ">"
             "<button class=\"btnDanger mini\" type=submit>Delete</button></form>";
        s += "</div>";
    }

    // Scanning is opt-in here (unlike the captive portal, which scans as
    // soon as it opens): the device is already online when this page loads,
    // and hopping channels to scan briefly interrupts that link, so it
    // should only happen when asked for.
    String chips = WebUi::wifiChips(scannedWifi, "newSsid");
    if (!chips.isEmpty()) {
        s += "<label>Networks in range</label>" + chips;
    }

    s += "<form method=post action=/wifi/add>";
    s += "<label>Network name<input id=newSsid name=ssid autocomplete=off required></label>";
    s += "<label>Password<input type=password name=password autocomplete=off></label>";
    s += "<div class=actions><button type=submit>Add network</button>";
    s += "<a class=\"btn btnGhost\" href=/wifi/scan>Scan for networks</a></div>";
    s += "</form>";
    s += "<p class=hint>Networks are tried top down, by list position rather than signal "
         "strength. Changes take effect on the next connection attempt. Scanning briefly "
         "interrupts the WiFi link while the radio hops channels.</p>";
    s += "</section>";
    return s;
}

String sectionDevice() {
    String s = "<section><h2>Settings</h2><form method=post action=/settings>";

    s += "<label>Refresh every (minutes)";
    s += "<input type=number name=refreshMin min=1 max=60 value=" + String(cfg->refreshMin) + "></label>";
    s += "<p class=hint>How often the account on screen refetches. Switching accounts "
         "does not refetch unless what is cached is older than 3 minutes.</p>";

    s += "<label>Idle light sleep after (minutes, 0 disables)";
    s += "<input type=number name=idleSleepMin min=0 max=120 value=" + String(cfg->idleSleepMin) + "></label>";

    s += "<label>Screen rotation<select name=rotation>";
    for (int r = 0; r < 4; r++) {
        s += "<option value=" + String(r);
        if (cfg->rotation == r) s += " selected";
        s += ">" + String(r * 90) + String("&deg;") + (r % 2 ? " (landscape)" : " (portrait)") + "</option>";
    }
    s += "</select></label>";

    s += "<label>Admin user<input name=adminUser value=\"" + htmlEscape(cfg->adminUser) + "\"></label>";
    s += "<label>Admin password<input type=password name=adminPassword autocomplete=off "
         "placeholder=\"unchanged\"></label>";

    s += "<div class=actions><button type=submit>Save settings</button></div>";
    s += "</form></section>";
    return s;
}

void handleRoot() {
    if (!requireAuth()) return;

    std::vector<AccountRecord> records;
    Accounts::load(records);

    String body = WebUi::brand("config");
    if (server.hasArg("err")) body += "<div class=err>" + htmlEscape(server.arg("err")) + "</div>";
    if (server.hasArg("msg")) body += "<div class=ok>" + htmlEscape(server.arg("msg")) + "</div>";

    body += sectionStatus();
    body += sectionAccounts(records);
    body += sectionWifi();
    body += sectionDevice();

    server.send(200, "text/html", WebUi::page("Plan Usage", body));
}

}  // namespace

namespace {

String accountFormPage(const String& type, Provider& p, int editIndex,
                       const String& err,
                       const std::vector<AccountRecord>& records) {
    String body = WebUi::brand(editIndex >= 0 ? "edit account" : "new account");
    if (!err.isEmpty()) body += "<div class=err>" + htmlEscape(err) + "</div>";

    body += "<section><h2>";
    body += p.typeName();
    body += "</h2><form method=post action=/account/save>";
    body += "<input type=hidden name=type value=" + type + ">";
    body += "<input type=hidden name=i value=" + String(editIndex) + ">";

    body += "<label>Name<input name=label value=\"" + htmlEscape(p.label()) + "\" "
            "placeholder=\"Personal / Work Teams\"></label>";

    if (editIndex < 0) {
        // Same login, another workspace: pick an existing row and the long
        // credential is carried over instead of being pasted again.
        bool any = false;
        String opts;
        for (size_t i = 0; i < records.size(); i++) {
            if (records[i].type != type) continue;
            any = true;
            opts += "<option value=" + String(i) + ">";
            opts += htmlEscape(records[i].label);
            opts += "</option>";
        }
        if (any) {
            body += "<label>Reuse the credential from<select name=reuse>";
            body += "<option value=-1>paste a new one below</option>";
            body += opts;
            body += "</select></label>";
        }
    }

    body += p.formHtml();

    if (editIndex >= 0 && !p.credentialId().isEmpty()) {
        body += "<label class=check><input type=checkbox name=applyAll value=1> "
                "Apply this credential to every row from the same login</label>";
    }

    body += "<div class=actions><button type=submit>Save</button>";
    if (p.supportsDiscovery()) {
        body += "<button class=btnGhost type=submit formaction=/account/discover>"
                "Discover workspaces</button>";
    }
    body += "<a class=\"btn btnGhost\" href=\"/\">Cancel</a></div>";
    body += "</form></section>";

    if (p.supportsDiscovery()) {
        body += "<p class=hint>Discover lists every organization this login can see and "
                "lets you add one row per organization, so a personal plan and a Teams "
                "plan under the same login only need the credential pasted once.</p>";
    }
    return WebUi::page("Plan Usage", body);
}

// Builds the provider the form is editing: an existing row, a row copied from
// another one, or a blank instance.
std::unique_ptr<Provider> providerForForm(const String& type, int editIndex,
                                          const std::vector<AccountRecord>& records) {
    auto p = Registry::create(type);
    if (!p) return nullptr;

    if (editIndex >= 0 && editIndex < (int)records.size()) {
        p->loadSettings(records[editIndex].settings);
        p->setLabel(records[editIndex].label);
        return p;
    }
    if (server.hasArg("reuse")) {
        int reuse = server.arg("reuse").toInt();
        if (reuse >= 0 && reuse < (int)records.size() && records[reuse].type == type) {
            p->loadSettings(records[reuse].settings);
        }
    }
    return p;
}

}  // namespace

namespace {

void handleAccountNew() {
    if (!requireAuth()) return;
    String type = server.arg("type");
    if (!Registry::isKnownType(type)) { redirectHome("err=Unknown+account+type"); return; }

    std::vector<AccountRecord> records;
    Accounts::load(records);

    auto p = Registry::create(type);
    server.send(200, "text/html", accountFormPage(type, *p, -1, "", records));
}

void handleAccountEdit() {
    if (!requireAuth()) return;
    std::vector<AccountRecord> records;
    Accounts::load(records);

    int i = server.arg("i").toInt();
    if (i < 0 || i >= (int)records.size()) { redirectHome("err=No+such+account"); return; }

    auto p = Registry::fromRecord(records[i]);
    if (!p) { redirectHome("err=Unknown+account+type"); return; }
    server.send(200, "text/html", accountFormPage(records[i].type, *p, i, "", records));
}

void handleAccountSave() {
    if (!requireAuth()) return;

    String type = server.arg("type");
    int    idx  = server.arg("i").toInt();
    if (!Registry::isKnownType(type)) { redirectHome("err=Unknown+account+type"); return; }

    std::vector<AccountRecord> records;
    Accounts::load(records);

    auto p = providerForForm(type, idx, records);
    if (!p) { redirectHome("err=Unknown+account+type"); return; }

    String previousCredential = p->credentialId();

    ServerForm form;
    String err;
    if (!p->applyForm(form, err)) {
        server.send(200, "text/html", accountFormPage(type, *p, idx, err, records));
        return;
    }

    String label = server.arg("label");
    label.trim();
    if (label.isEmpty()) label = p->typeName();
    p->setLabel(label);

    AccountRecord rec;
    rec.type     = type;
    rec.label    = label;
    rec.settings = p->saveSettings();

    if (idx >= 0 && idx < (int)records.size()) {
        records[idx] = rec;
    } else if ((int)records.size() >= MAX_ACCOUNTS) {
        redirectHome("err=Account+list+is+full");
        return;
    } else {
        records.push_back(rec);
    }

    // One login can back several rows. When the credential behind it is
    // replaced, offer to carry the new one across instead of re-pasting it
    // into each row by hand.
    int touched = 0;
    if (server.hasArg("applyAll") && !previousCredential.isEmpty()) {
        String secret = p->credentialSecret();
        for (size_t i = 0; i < records.size(); i++) {
            if ((int)i == idx || records[i].type != type) continue;
            auto other = Registry::fromRecord(records[i]);
            if (!other || other->credentialId() != previousCredential) continue;
            other->setCredential(secret);
            records[i].settings = other->saveSettings();
            touched++;
        }
    }

    Accounts::saveAll(records);
    if (hooks.onAccountsChanged) hooks.onAccountsChanged();

    String msg = "msg=Saved";
    if (touched > 0) msg += "+(" + String(touched) + "+other+rows+updated)";
    redirectHome(msg);
}

}  // namespace

namespace {

void handleAccountDiscover() {
    if (!requireAuth()) return;

    String type = server.arg("type");
    int    idx  = server.arg("i").toInt();
    if (!Registry::isKnownType(type)) { redirectHome("err=Unknown+account+type"); return; }

    std::vector<AccountRecord> records;
    Accounts::load(records);

    auto p = providerForForm(type, idx, records);
    if (!p) { redirectHome("err=Unknown+account+type"); return; }

    ServerForm form;
    String err;
    if (!p->applyForm(form, err)) {
        server.send(200, "text/html", accountFormPage(type, *p, idx, err, records));
        return;
    }

    std::vector<Workspace> found;
    if (!p->discover(found, err)) {
        server.send(200, "text/html", accountFormPage(type, *p, idx, err, records));
        return;
    }

    // Park the credential here rather than in a hidden field: the page should
    // never carry the secret, even out of sight.
    pendingType       = type;
    pendingSettings   = p->saveSettings();
    pendingWorkspaces = found;

    String body = WebUi::brand("discover");
    body += "<section><h2>";
    body += p->typeName();
    body += " workspaces</h2>";
    body += "<form method=post action=/account/addmany>";

    for (size_t i = 0; i < found.size(); i++) {
        body += "<label class=check><input type=checkbox name=w" + String(i) + " value=1 checked> ";
        body += htmlEscape(found[i].name);
        if (!found[i].kind.isEmpty()) {
            body += " <span class=pill>" + htmlEscape(found[i].kind) + "</span>";
        }
        body += "</label>";
    }

    body += "<div class=actions><button type=submit>Add selected</button>";
    body += "<a class=\"btn btnGhost\" href=\"/\">Cancel</a></div></form>";
    body += "<p class=hint>Each one becomes its own row sharing the credential you just "
            "pasted.</p></section>";

    server.send(200, "text/html", WebUi::page("Plan Usage", body));
}

void handleAccountAddMany() {
    if (!requireAuth()) return;
    if (pendingWorkspaces.empty() || pendingSettings.isEmpty()) {
        redirectHome("err=Nothing+to+add");
        return;
    }

    std::vector<AccountRecord> records;
    Accounts::load(records);

    int added = 0;
    for (size_t i = 0; i < pendingWorkspaces.size(); i++) {
        if (!server.hasArg(("w" + String(i)).c_str())) continue;
        if ((int)records.size() >= MAX_ACCOUNTS) break;

        auto p = Registry::create(pendingType);
        if (!p) break;
        p->loadSettings(pendingSettings);
        p->selectWorkspace(pendingWorkspaces[i]);

        AccountRecord rec;
        rec.type     = pendingType;
        rec.label    = pendingWorkspaces[i].name;
        rec.settings = p->saveSettings();
        records.push_back(rec);
        added++;
    }

    pendingSettings = "";
    pendingWorkspaces.clear();

    Accounts::saveAll(records);
    if (hooks.onAccountsChanged) hooks.onAccountsChanged();
    redirectHome("msg=Added+" + String(added) + "+accounts");
}

void handleAccountDelete() {
    if (!requireAuth()) return;
    std::vector<AccountRecord> records;
    Accounts::load(records);

    int i = server.arg("i").toInt();
    if (i < 0 || i >= (int)records.size()) { redirectHome("err=No+such+account"); return; }

    records.erase(records.begin() + i);
    Accounts::saveAll(records);
    if (hooks.onAccountsChanged) hooks.onAccountsChanged();
    redirectHome("msg=Account+deleted");
}

void handleAccountMove() {
    if (!requireAuth()) return;
    std::vector<AccountRecord> records;
    Accounts::load(records);

    int i = server.arg("i").toInt();
    int d = server.arg("d").toInt();
    int j = i + d;
    if (i < 0 || i >= (int)records.size() || j < 0 || j >= (int)records.size()) {
        redirectHome();
        return;
    }
    std::swap(records[i], records[j]);
    Accounts::saveAll(records);
    if (hooks.onAccountsChanged) hooks.onAccountsChanged();
    redirectHome();
}

void handleAccountSwitch() {
    if (!requireAuth()) return;
    int i = server.arg("i").toInt();
    if (hooks.onSwitchTo) hooks.onSwitchTo(i);
    redirectHome();
}

}  // namespace

namespace {

void handleWifiScan() {
    if (!requireAuth()) return;
    scannedWifi = WifiScan::scan();
    Serial.printf("[web] scan found %d network(s)\n", (int)scannedWifi.size());
    redirectHome();
}

void handleWifiAdd() {
    if (!requireAuth()) return;
    String ssid = server.arg("ssid");
    ssid.trim();
    if (ssid.isEmpty()) { redirectHome("err=A+network+name+is+required"); return; }

    if (!Settings::addOrUpdateWifi(*cfg, ssid, server.arg("password"))) {
        redirectHome("err=The+network+list+is+full");
        return;
    }
    Settings::save(*cfg);
    redirectHome("msg=Network+saved");
}

void handleWifiDelete() {
    if (!requireAuth()) return;
    if (Settings::removeWifiAt(*cfg, server.arg("i").toInt())) Settings::save(*cfg);
    redirectHome();
}

void handleWifiMove() {
    if (!requireAuth()) return;
    if (Settings::moveWifi(*cfg, server.arg("i").toInt(), server.arg("d").toInt())) {
        Settings::save(*cfg);
    }
    redirectHome();
}

void handleSettingsSave() {
    if (!requireAuth()) return;

    cfg->refreshMin   = server.arg("refreshMin").toInt();
    cfg->idleSleepMin = server.arg("idleSleepMin").toInt();
    cfg->rotation     = server.arg("rotation").toInt() & 3;

    String user = server.arg("adminUser");
    user.trim();
    if (!user.isEmpty()) cfg->adminUser = user;

    // Blank means "leave the password alone", matching how the credential
    // fields behave elsewhere.
    String pass = server.arg("adminPassword");
    if (!pass.isEmpty()) cfg->adminPassword = pass;

    if (cfg->refreshMin < 1)    cfg->refreshMin = 1;
    if (cfg->refreshMin > 60)   cfg->refreshMin = 60;
    if (cfg->idleSleepMin < 0)  cfg->idleSleepMin = 0;

    Settings::save(*cfg);
    if (hooks.onSettingsChanged) hooks.onSettingsChanged();
    redirectHome("msg=Settings+saved");
}

void handleForceRefresh() {
    if (!requireAuth()) return;
    if (hooks.onForceRefresh) hooks.onForceRefresh();
    redirectHome("msg=Refreshing");
}

}  // namespace

void WebConfig::begin(DeviceSettings* settings, Carousel* carousel, const Hooks& h) {
    cfg   = settings;
    wheel = carousel;
    hooks = h;

    WebUi::registerLogoRoute(server);
    server.on("/",                  HTTP_GET,  handleRoot);
    server.on("/account/new",       HTTP_GET,  handleAccountNew);
    server.on("/account/edit",      HTTP_GET,  handleAccountEdit);
    server.on("/account/save",      HTTP_POST, handleAccountSave);
    server.on("/account/discover",  HTTP_POST, handleAccountDiscover);
    server.on("/account/addmany",   HTTP_POST, handleAccountAddMany);
    server.on("/account/delete",    HTTP_POST, handleAccountDelete);
    server.on("/account/move",      HTTP_POST, handleAccountMove);
    server.on("/account/switch",    HTTP_POST, handleAccountSwitch);
    server.on("/wifi/scan",         HTTP_GET,  handleWifiScan);
    server.on("/wifi/add",          HTTP_POST, handleWifiAdd);
    server.on("/wifi/delete",       HTTP_POST, handleWifiDelete);
    server.on("/wifi/move",         HTTP_POST, handleWifiMove);
    server.on("/settings",          HTTP_POST, handleSettingsSave);
    server.on("/refresh",           HTTP_POST, handleForceRefresh);
    server.onNotFound([] { server.send(404, "text/plain", "not found"); });

    server.begin();
    Serial.println("[web] admin server started");
}

void WebConfig::loop() {
    server.handleClient();
}
