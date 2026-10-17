#pragma once
#include <windows.h>

namespace cheat::hooks {

// Idempotent: safe to call every frame until the subclass is in place.
// Uses SetWindowLongPtrW(GWLP_WNDPROC) — not VMT, not VEH. §4.1.
void EnsureWndProcSubclass();
void UninstallWndProcSubclass();

bool IsMenuSwallowingInput();   // true while the menu is open and keyboard-captured

} // namespace cheat::hooks
