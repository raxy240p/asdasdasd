// injector/main.cpp — WinMain. No console. Self-rename first, then UI.
#include "randomize.hpp"
#include "ui_injector.hpp"
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // §3.1: copy to %TEMP%\<rand>\<rand>.exe and relaunch; schedule melt of
    // the original. Returns true immediately when we're already the staged copy.
    injector::randomize::SelfRelaunchAndMelt();
    return injector::ui::Run(hInst);
}
