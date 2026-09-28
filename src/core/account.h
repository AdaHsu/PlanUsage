#pragma once
#include <Arduino.h>
#include <vector>

static const int MAX_ACCOUNTS = 99;

// One row of the carousel. `settings` is an opaque JSON blob owned by the
// provider module; the shell never looks inside it, which is what keeps
// adding a new provider from touching any core file.
//
// The list is free-form: any mix of types in any quantity. Two Claude
// personal logins, one Claude enterprise org and three Codex accounts is a
// perfectly ordinary list, and nothing in the code pairs types up.
//
// Stored as a single JSON array on LittleFS (/accounts.json), not in NVS:
// a Codex access token alone is 1-2 KB, and a dozen of those plus Claude
// session keys would crowd out the stock ~20 KB nvs partition (and land on
// NVS's own per-value size ceiling besides). LittleFS has megabytes to
// spare on the stock partition table and no such limit.
struct AccountRecord {
    String type;       // "claude" / "openai"
    String label;      // user-facing name, e.g. "Work Teams"
    String settings;   // provider-owned JSON
    bool   enabled = true;
};

namespace Accounts {
    // Mounts LittleFS (formatting it on first boot). Call once, early.
    bool begin();

    void load(std::vector<AccountRecord>& out);
    bool saveAll(const std::vector<AccountRecord>& list);

    // Rewrite one row's settings blob. Used when a module updates its own
    // state during a fetch (a discovered org id, a refreshed token).
    bool saveSettingsAt(int index, const String& settings);

    // Small enough to stay in NVS: one int, no size pressure.
    int  loadActiveIndex();
    void saveActiveIndex(int index);
}
