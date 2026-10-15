#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <vector>

// The contract every model-provider module implements. A module owns three
// things end to end: its data (how it fetches), its screen (how it lays the
// numbers out) and its settings form (what the admin page asks for). The
// shell knows none of that; it only hands over a rectangle and a clock.
//
// Adding a provider = one directory under src/providers + one line in
// registry.cpp. No core file changes.

// The canvas plus the rectangle the shell reserved for the module. The shell
// has already drawn the header identifying which account this is, so the
// module should not repeat it.
struct RenderCtx {
    TFT_eSPI& g;
    int       x;
    int       y;
    int       w;
    int       h;
    time_t    now;        // UTC
    bool      landscape;
};

enum class FetchResult { Ok, Unauthorized, Network, Parse, NotConfigured };

// Read-only view over submitted form fields. Keeps provider modules free of
// any WebServer dependency; webconfig supplies the adapter.
class FormSource {
public:
    virtual ~FormSource() = default;
    virtual bool   has(const char* name) const = 0;
    virtual String get(const char* name) const = 0;
};

// One workspace reachable with a single credential: a Claude organization
// (personal or Teams), a ChatGPT workspace. Discovery is what keeps the
// promise of "paste once per login, not once per account".
struct Workspace {
    String id;
    String name;
    String kind;   // "Teams" / "Pro" / "Business" - shown as the badge
};

class Provider {
public:
    virtual ~Provider() = default;

    // --- identity -------------------------------------------------------
    virtual const char* typeId()   const = 0;   // stable key stored in NVS
    virtual const char* typeName() const = 0;   // shown in the header
    virtual uint16_t    accent()   const = 0;   // header tint, one per vendor
    virtual String      subLabel() const = 0;   // kind badge, may be empty

    String label() const               { return label_; }
    void   setLabel(const String& l)   { label_ = l; }

    // --- data: only ever called for the account being looked at ---------
    virtual FetchResult refresh(time_t now) = 0;
    // 0 (the default) means "no opinion, use the device-wide cadence".
    // A provider that stores its own refresh setting (in its own settings
    // form/blob, same place a sessionKey lives) returns that value instead,
    // and it is used exactly as given - faster or slower than the device
    // default - because it came from an explicit per-account choice, not
    // from the module silently overriding the user.
    virtual uint32_t    refreshIntervalMs() const { return 0; }
    virtual bool        hasData() const = 0;

    // --- screen: layout is entirely the module's business ---------------
    // Everything about the provider belongs here, including its own
    // warnings (a token nearing expiry, a re-auth prompt). The Info screen
    // is device-level and modules take no part in it.
    virtual void show(const RenderCtx& ctx) = 0;

    // --- settings: the module owns its own form and serialisation -------
    virtual void   loadSettings(const String& json) = 0;
    virtual String saveSettings() const = 0;
    virtual String formHtml() const = 0;
    virtual bool   applyForm(const FormSource& form, String& err) = 0;

    // --- discovery (optional) -------------------------------------------
    virtual bool supportsDiscovery() const { return false; }
    virtual bool discover(std::vector<Workspace>& out, String& err) {
        (void)out; err = "not supported"; return false;
    }
    virtual void selectWorkspace(const Workspace& w) { (void)w; }

    // --- credential sharing ----------------------------------------------
    // Rows with the same credentialId() came from one login, so re-pasting an
    // expired credential can be applied to all of them in one go.
    virtual String credentialId()   const { return String(); }
    virtual String credentialHint() const { return String(); }  // masked tail
    // The raw secret, used only to copy a re-pasted credential onto the other
    // rows that came from the same login. Never rendered.
    virtual String credentialSecret() const { return String(); }
    virtual void   setCredential(const String& cred) { (void)cred; }

    // --- shell-visible state ---------------------------------------------
    time_t        lastFetchAt() const { return lastFetchAt_; }
    unsigned long lastFetchMs() const { return lastFetchMs_; }
    bool          hasFetched()  const { return lastFetchMs_ != 0; }
    String        lastError()   const { return lastError_; }
    bool          needsAuth()   const { return needsAuth_; }

    // True once when the module changed its own persisted settings (an
    // auto-discovered org id, a refreshed token) so the shell writes it back.
    bool consumeDirty() { bool d = dirty_; dirty_ = false; return d; }

protected:
    void markFetched(time_t now) {
        lastFetchAt_ = now;
        lastFetchMs_ = millis();
        if (lastFetchMs_ == 0) lastFetchMs_ = 1;  // 0 means "never fetched"
    }
    void setError(const String& e)  { lastError_ = e; }
    void setNeedsAuth(bool v)       { needsAuth_ = v; }
    void markDirty()                { dirty_ = true; }

    String label_;

private:
    time_t        lastFetchAt_ = 0;
    unsigned long lastFetchMs_ = 0;
    String        lastError_;
    bool          needsAuth_   = false;
    bool          dirty_       = false;
};

// "sk-ant-...a3f9" - what the admin page shows instead of a stored secret.
String maskCredential(const String& secret);

// Shared by the modules when they build their settings forms.
String htmlEscape(const String& s);
