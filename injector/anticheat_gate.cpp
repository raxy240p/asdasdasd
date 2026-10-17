// injector/anticheat_gate.cpp — process enumeration per §3.2.
#include "anticheat_gate.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
#include <string_view>

namespace injector::ac {

namespace {

struct FamilyDef {
    Family                      family;
    const wchar_t*              label;
    std::vector<std::wstring>   processes;
};

// Data-driven so swap/extend lives here, nowhere else.
const std::vector<FamilyDef>& Table() {
    static const std::vector<FamilyDef> t = {
        { Family::Vanguard, L"Vanguard",
          { L"vgtray.exe", L"vgc.exe", L"vgk.exe" } },
        { Family::EAC,      L"EasyAntiCheat",
          { L"EasyAntiCheat.exe", L"EasyAntiCheat_EOS.exe",
            L"EasyAntiCheat_Service.exe" } },
        { Family::BattlEye, L"BattlEye",
          { L"BEService.exe", L"BEService_x64.exe" } },
        { Family::Faceit,   L"FACEIT",
          { L"faceit.exe", L"faceitclient.exe" } },
        { Family::Steam,    L"Steam",
          { L"steam.exe" } },
    };
    return t;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::vector<std::wstring> SnapshotProcessImages() {
    std::vector<std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do { out.emplace_back(Lower(pe.szExeFile)); }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

} // namespace

std::vector<Status> ScanProcesses(bool checkSteam) {
    auto running = SnapshotProcessImages();
    std::vector<Status> out;
    for (const auto& fam : Table()) {
        if (fam.family == Family::Steam && !checkSteam) continue;
        Status s{ fam.family, fam.label, false, {} };
        for (const auto& needle : fam.processes) {
            auto n = Lower(needle);
            auto it = std::find(running.begin(), running.end(), n);
            if (it != running.end()) {
                s.running = true;
                s.offenderProcess = *it;
                break;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

bool AllClear(const std::vector<Status>& v) {
    return std::all_of(v.begin(), v.end(),
                       [](const Status& s) { return !s.running; });
}

} // namespace injector::ac
