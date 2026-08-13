#include "dashboard/network_indicator.hpp"

#include <atomic>

namespace dashboard {
namespace {

// Starts Offline rather than Connecting: at boot nothing has been attempted, and showing a
// connecting state before the radio is even powered would be a small lie on the very first frame.
std::atomic<uint8_t> g_indicator{static_cast<uint8_t>(NetworkIndicator::Offline)};

}  // namespace

NetworkIndicator indicatorForRssi(int rssi_dbm) {
    if (rssi_dbm >= -55) {
        return NetworkIndicator::Strong;
    }
    if (rssi_dbm >= -75) {
        return NetworkIndicator::Fair;
    }
    return NetworkIndicator::Weak;
}

void setNetworkIndicator(NetworkIndicator value) {
    g_indicator.store(static_cast<uint8_t>(value), std::memory_order_relaxed);
}

NetworkIndicator networkIndicator() {
    return static_cast<NetworkIndicator>(g_indicator.load(std::memory_order_relaxed));
}

}  // namespace dashboard
