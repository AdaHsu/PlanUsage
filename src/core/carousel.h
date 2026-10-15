#pragma once
#include "provider.h"
#include "registry.h"
#include <memory>
#include <vector>

// Settle window after a switch: nothing is fetched until the user has stayed
// put this long, so scrolling past several accounts costs no requests.
static const unsigned long SWITCH_SETTLE_MS  = 5000;
// Once settled, only refetch when what we already hold is older than this.
static const unsigned long SWITCH_MIN_AGE_MS = 3UL * 60 * 1000;

// Holds one live Provider per account row and decides when the one being
// looked at is allowed to hit the network. Everything else stays cold: a
// provider that is not on screen never fetches, never runs a timer and is
// never asked to draw.
class Carousel {
public:
    // Rebuilds every provider instance from NVS. Called at boot and whenever
    // the admin page changes the account list.
    void rebuild();

    int  size()        const { return (int)providers_.size(); }
    bool empty()       const { return providers_.empty(); }
    int  activeIndex() const { return activeIndex_; }
    Provider* active();
    Provider* at(int index);

    // Advance one row. Does not fetch: it only restarts the settle timer, so
    // holding the button through several accounts costs zero requests.
    void next();
    void setActiveIndex(int index);

    // Refresh cadence from device settings, in minutes. A provider's own
    // refreshIntervalMs() wins outright (faster or slower) whenever it
    // returns something other than 0; 0 means "no opinion, use this".
    void     setRefreshMinutes(int minutes);
    uint32_t effectiveIntervalMs() const;

    // True while the settle window after a switch is still running.
    bool settling(unsigned long nowMs) const;

    // The two gates: settle for 5 s after a switch, then refetch only when
    // the cache is older than 3 minutes. Outside a switch the normal interval
    // applies. Clears switch state as a side effect, so call once per tick.
    bool dueForFetch(unsigned long nowMs);

    // Ignores both gates. Backs the refresh-now button on the admin page.
    void forceFetchNext() { forceFetch_ = true; }

    int  secondsUntilRefresh(unsigned long nowMs) const;

    // Writes back settings a module changed during a fetch, such as an
    // auto-discovered org id or a refreshed token.
    void persistDirtySettings();

private:
    std::vector<std::unique_ptr<Provider>> providers_;
    int           activeIndex_   = 0;
    bool          switchPending_ = false;
    unsigned long switchedAtMs_  = 0;
    int           refreshMin_    = 5;
    bool          forceFetch_    = false;
};
