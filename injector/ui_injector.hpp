#pragma once
#include <windows.h>

namespace injector::ui {

// Blocking message loop. Returns when the user closes the window.
int Run(HINSTANCE hInst);

} // namespace injector::ui
