#include "util/platform.hpp"

#include <atomic>
#include <mutex>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace platform {

bool isAppletMode() {
#ifdef __SWITCH__
    AppletType t = appletGetAppletType();
    return t != AppletType_Application && t != AppletType_SystemApplication;
#else
    return false;
#endif
}

namespace {
std::mutex awakeMutex;
int awakeMask = 0;
}  // namespace

void setAwake(AwakeReason reason, bool on) {
    std::lock_guard<std::mutex> lock(awakeMutex);
    int before = awakeMask;
    if (on)
        awakeMask |= reason;
    else
        awakeMask &= ~reason;
    if ((before != 0) == (awakeMask != 0)) return;
#ifdef __SWITCH__
    // "riproduzione multimediale in corso": il sistema non va in standby automatico
    appletSetMediaPlaybackState(awakeMask != 0);
#endif
}

bool downloadActive() {
    std::lock_guard<std::mutex> lock(awakeMutex);
    return (awakeMask & AWAKE_DOWNLOAD) != 0;
}

}  // namespace platform
