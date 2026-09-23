#include "util/platform.hpp"

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

}  // namespace platform
