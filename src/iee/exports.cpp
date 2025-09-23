#include "hooks.h"
#include "iee/core/version.h"


extern "C" __declspec(dllexport) const char * __stdcall GetIEEVersion() {
    return IEE_VERSION;
}


extern "C" __declspec(dllexport) bool __stdcall IsActive() {
    return iee::hooks::is_active();
}
