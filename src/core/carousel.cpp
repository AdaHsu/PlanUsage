#include "carousel.h"
#include "account.h"

void Carousel::rebuild() {
    std::vector<AccountRecord> records;
    Accounts::load(records);

    providers_.clear();
    for (const auto& rec : records) {
        if (!rec.enabled) continue;
        auto p = Registry::fromRecord(rec);
        if (p) providers_.push_back(std::move(p));
    }

    int saved = Accounts::loadActiveIndex();
    activeIndex_   = (providers_.empty() || saved >= (int)providers_.size()) ? 0 : saved;
    switchPending_ = false;
    forceFetch_    = false;
}

Provider* Carousel::active() {
    if (providers_.empty()) return nullptr;
    return providers_[activeIndex_].get();
}

Provider* Carousel::at(int index) {
    if (index < 0 || index >= (int)providers_.size()) return nullptr;
    return providers_[index].get();
}

void Carousel::next() {
    // A single account has nowhere to go. Leaving the settle timer alone
    // means a stray press cannot postpone its regular refresh.
    if (providers_.size() < 2) return;

    activeIndex_   = (activeIndex_ + 1) % (int)providers_.size();
    switchPending_ = true;
    switchedAtMs_  = millis();
    Accounts::saveActiveIndex(activeIndex_);
}

void Carousel::setActiveIndex(int index) {
    if (index < 0 || index >= (int)providers_.size()) return;
    activeIndex_   = index;
    switchPending_ = true;
    switchedAtMs_  = millis();
    Accounts::saveActiveIndex(activeIndex_);
}

void Carousel::setRefreshMinutes(int minutes) {
    if (minutes < 1)  minutes = 1;
    if (minutes > 60) minutes = 60;
    refreshMin_ = minutes;
}

uint32_t Carousel::effectiveIntervalMs() const {
    const Provider* p = providers_.empty() ? nullptr : providers_[activeIndex_].get();
    if (p) {
        uint32_t moduleWants = p->refreshIntervalMs();
        // 0 means the provider has no cadence of its own configured and
        // defers to the device-wide setting; anything else is an explicit
        // per-account choice (from that provider's own settings form) and
        // wins outright, even when it is faster than the device default.
        if (moduleWants > 0) return moduleWants;
    }
    return (uint32_t)refreshMin_ * 60UL * 1000UL;
}

bool Carousel::settling(unsigned long nowMs) const {
    return switchPending_ && (nowMs - switchedAtMs_ < SWITCH_SETTLE_MS);
}

bool Carousel::dueForFetch(unsigned long nowMs) {
    Provider* p = active();
    if (!p) return false;

    if (forceFetch_) {
        forceFetch_    = false;
        switchPending_ = false;
        return true;
    }

    // Gate 1: stay put for the settle window. Every switch restarts it, so a
    // run of presses walks the whole list without a single request going out.
    if (switchPending_) {
        if (nowMs - switchedAtMs_ < SWITCH_SETTLE_MS) return false;
        switchPending_ = false;
        // Gate 2: the cache has to be genuinely stale before spending a request.
        if (!p->hasFetched()) return true;
        return (nowMs - p->lastFetchMs()) >= SWITCH_MIN_AGE_MS;
    }

    if (!p->hasFetched()) return true;
    return (nowMs - p->lastFetchMs()) >= effectiveIntervalMs();
}

int Carousel::secondsUntilRefresh(unsigned long nowMs) const {
    if (providers_.empty()) return 0;
    const Provider* p = providers_[activeIndex_].get();
    if (!p || !p->hasFetched()) return 0;
    uint32_t interval = effectiveIntervalMs();
    unsigned long elapsed = nowMs - p->lastFetchMs();
    if (elapsed >= interval) return 0;
    return (int)((interval - elapsed) / 1000);
}

void Carousel::persistDirtySettings() {
    for (size_t i = 0; i < providers_.size(); i++) {
        if (providers_[i]->consumeDirty()) {
            Accounts::saveSettingsAt((int)i, providers_[i]->saveSettings());
        }
    }
}
