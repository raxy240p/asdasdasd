#pragma once
#include <string>
#include <filesystem>

namespace injector::randomize {

// Big adjective/noun wordlists live in randomize.cpp. 60+ each so names
// never look dictionary-fixed. Returns something like "DriftRelay4213".
std::wstring MakeRandomName();

// Fresh %TEMP%\<rand>\<rand>.exe path, directories not yet created.
std::filesystem::path MakeStagingPath();

// On first run (own path NOT under %TEMP%\<SessionTag>\), relaunch from a
// random staging path and schedule melt of the current binary. The current
// process exits cleanly on success. Returns false only on first-run attempt
// failures (second run is the fresh exe and returns true immediately).
bool SelfRelaunchAndMelt();

// Window identity: title/class randomized per launch. Both strings persist
// for the lifetime of the process so SetWindowText / RegisterClassW agree.
const wchar_t* RandomWindowTitle();
const wchar_t* RandomWindowClass();
const char*    RandomImGuiTitle();

} // namespace injector::randomize
