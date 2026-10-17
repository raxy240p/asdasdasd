#pragma once
#include <string>
#include <vector>

namespace injector::ac {

enum class Family { Vanguard, EAC, BattlEye, Faceit, Steam };

struct Status {
    Family           family;
    const wchar_t*   label;
    bool             running;
    std::wstring     offenderProcess; // first match's image name, empty if clear
};

// Snapshot every known AC process family. Called by the UI every N frames or
// on RECHECK. Steam row is checked only when `checkSteam` is true (first scan).
std::vector<Status> ScanProcesses(bool checkSteam);

// True iff every row in the vector reports !running.
bool AllClear(const std::vector<Status>& v);

} // namespace injector::ac
