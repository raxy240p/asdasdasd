// injector/main.cpp — WinMain. No console. Self-rename first, then UI.
#include "randomize.hpp"
#include "ui_injector.hpp"
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // §3.1: first run → stage to %TEMP%\<rand>\<rand>.exe and relaunch.
    // Staged run → delete the original named by --melt, then UI.
    injector::randomize::SelfRelaunchAndMelt();
    injector::randomize::DeleteSelfOrigFromCmdLine();
    return injector::ui::Run(hInst);
}
