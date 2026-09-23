#include "util/network.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace network {

bool connected() {
#ifdef __SWITCH__
    static bool nifmReady = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    if (!nifmReady) return true;  // servizio non disponibile: si prova comunque
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus status;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
#else
    return true;
#endif
}

}  // namespace network
