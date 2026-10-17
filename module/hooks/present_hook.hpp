#pragma once
#include <dxgi.h>

namespace cheat::hooks {

bool InstallPresent();     // resolves vtable, patches Present + ResizeBuffers
void UninstallPresent();
bool PresentInFlight();    // true while the detour body is running — used by Teardown

// Signatures match IDXGISwapChain:: for pass-through trampoline typing.
using PresentFn       = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

} // namespace cheat::hooks
