// The one piece of network state every page header shows.
//
// WHY THIS IS GLOBAL AND NOT PER-PLUGIN
//
// Plugins already carry an `online` flag, fed to them through onNetworkChanged(), because each
// one needs to know whether its own fetch can succeed. The header icon is a different thing: it
// reports the state of the radio, is identical on every page, and has nothing to do with any
// plugin's data. Fanning the same value into every plugin — and widening a virtual API used by
// all of them to carry a signal strength — would be a lot of plumbing to say one thing once.
//
// So it lives here: written by whoever watches the radio, read on the LVGL thread when a header
// is drawn. Atomic, so neither side needs a lock.

#pragma once

#include <cstdint>

namespace dashboard {

/// What the header's network icon should show.
///
/// Deliberately the *display* states rather than the radio's states: the mapping from WifiState
/// and an RSSI in dBm to something a person can read at a glance belongs to whoever owns the
/// radio, and this enum is the vocabulary they map into.
enum class NetworkIndicator : uint8_t {
    Offline,      ///< No usable connection.
    Connecting,   ///< Association in progress.
    SetupPortal,  ///< Serving the first-run portal; the user is expected to be looking at a phone.
    Weak,         ///< Connected, poor signal.
    Fair,         ///< Connected, workable signal.
    Strong,       ///< Connected, good signal.
};

/// Classify a signal strength in dBm. Thresholds are the conventional ones: about -55 dBm and
/// better is as good as it needs to be, and past roughly -75 dBm throughput starts to suffer.
NetworkIndicator indicatorForRssi(int rssi_dbm);

/// Safe to call from any task.
void setNetworkIndicator(NetworkIndicator value);

/// Reads the current value. Called on the LVGL thread while drawing a header.
NetworkIndicator networkIndicator();

}  // namespace dashboard
