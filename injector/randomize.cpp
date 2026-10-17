// injector/randomize.cpp — self-rename + melt, per §3.1.
#include "randomize.hpp"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <random>
#include <string>
#include <string_view>

#pragma comment(lib, "shell32.lib")

namespace injector::randomize {

namespace {

// Deliberately long lists — ponytail: fixed-at-build-time string pools,
// swap at next build if they become identifiable.
constexpr const wchar_t* kAdj[] = {
    L"Amber", L"Basalt", L"Cobalt", L"Drift", L"Ember", L"Fjord", L"Glass",
    L"Hollow", L"Ivory", L"Jade", L"Kelp", L"Linen", L"Marble", L"Nickel",
    L"Opal", L"Pewter", L"Quartz", L"Rust", L"Sable", L"Teal", L"Umber",
    L"Velvet", L"Walnut", L"Xenon", L"Yarrow", L"Zinc", L"Bramble", L"Cedar",
    L"Dusk", L"Eiger", L"Flint", L"Granite", L"Hemlock", L"Iron", L"Juniper",
    L"Kiln", L"Lichen", L"Mahogany", L"Nocturne", L"Obsidian", L"Pine", L"Rook",
    L"Sage", L"Thorn", L"Umbral", L"Vale", L"Willow", L"Yew", L"Zephyr",
    L"Aster", L"Birch", L"Clover", L"Dune", L"Elm", L"Fern", L"Gale",
    L"Haze", L"Indigo", L"Jet",   L"Kismet", L"Lace", L"Moss", L"Nile",
};
constexpr const wchar_t* kNoun[] = {
    L"Audit", L"Badge", L"Cache", L"Panel", L"Relay", L"Vertex", L"Lattice",
    L"Beacon", L"Cipher", L"Dossier", L"Echo", L"Facet", L"Gauge", L"Harbor",
    L"Index", L"Journal", L"Kernel", L"Ledger", L"Monitor", L"Nexus", L"Oracle",
    L"Prism", L"Query", L"Register", L"Shard", L"Token", L"Union", L"Vector",
    L"Weave", L"Yield", L"Zenith", L"Arcade", L"Blueprint", L"Compass", L"Delta",
    L"Envoy", L"Forge", L"Glyph", L"Horizon", L"Icon", L"Jigsaw", L"Keystone",
    L"Loom", L"Mantle", L"Needle", L"Orbit", L"Pulse", L"Quill", L"Reef",
    L"Signal", L"Threshold", L"Umbra", L"Vista", L"Wrench", L"Yardarm", L"Zone",
    L"Anchor", L"Beam", L"Codex", L"Dial", L"Ember", L"Flume", L"Grid",
};

std::mt19937& Rng() {
    static std::random_device rd;
    static std::mt19937 r(rd() ^ static_cast<uint32_t>(GetTickCount64()));
    return r;
}

std::wstring Mix(std::wstring s) {
    std::uniform_int_distribution<int> flip(0, 1);
    if (flip(Rng())) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    }
    return s;
}

constexpr const wchar_t* kSessionTagEnv = L"SK_LDR_STAGED";

std::wstring& CachedTitle() { static std::wstring s; return s; }
std::wstring& CachedClass() { static std::wstring s; return s; }
std::string&  CachedImGui() { static std::string  s; return s; }

std::wstring ToUtf16(const std::string& narrow) {
    std::wstring w(narrow.begin(), narrow.end()); return w;
}
std::string ToUtf8(const std::wstring& wide) {
    std::string s(wide.begin(), wide.end()); return s;
}

} // namespace

std::wstring MakeRandomName() {
    auto& r = Rng();
    std::uniform_int_distribution<size_t> dA(0, std::size(kAdj) - 1);
    std::uniform_int_distribution<size_t> dN(0, std::size(kNoun) - 1);
    std::uniform_int_distribution<int>    dD(100, 99999); // 3–5 digits
    std::wstring s = kAdj[dA(r)];
    s += kNoun[dN(r)];
    s += std::to_wstring(dD(r));
    return Mix(std::move(s));
}

std::filesystem::path MakeStagingPath() {
    // GetTempPathW returns the required length when the buffer is too small;
    // using that value as an iterator past the end of a MAX_PATH buffer is
    // the OOB-read the audit flagged. Pre-size with GetTempPathW(0, nullptr).
    DWORD need = GetTempPathW(0, nullptr);
    if (!need) return {};
    std::wstring buf(need, L'\0');
    DWORD n = GetTempPathW(need, buf.data());
    if (!n || n >= need) return {};
    buf.resize(n);
    std::filesystem::path tmp(buf);
    tmp /= MakeRandomName();
    tmp /= (MakeRandomName() + L".exe");
    return tmp;
}

bool SelfRelaunchAndMelt() {
    // If we're already the staged copy, don't recurse. Clear the breadcrumb
    // so any CS2 child process we touch later doesn't inherit it.
    if (GetEnvironmentVariableW(kSessionTagEnv, nullptr, 0) != 0) {
        SetEnvironmentVariableW(kSessionTagEnv, nullptr);
        return true;
    }

    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;

    const std::filesystem::path origExe(self);
    const auto origDir = origExe.parent_path();
    const auto origDll = origDir / L"cheat.dll";
    if (!std::filesystem::exists(origDll)) {
        // Can't inject without the DLL — refuse the stage so the user sees
        // a plain "cheat.dll missing next to loader" from the current path
        // instead of a staged copy that can't find it either.
        return false;
    }

    auto staged = MakeStagingPath();
    if (staged.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(staged.parent_path(), ec);
    if (ec) return false;

    // Stage BOTH the loader and the DLL next to each other.
    const auto stagedDll = staged.parent_path() / L"cheat.dll";
    if (!CopyFileW(self,            staged.c_str(),   FALSE)) return false;
    if (!CopyFileW(origDll.c_str(), stagedDll.c_str(), FALSE)) {
        DeleteFileW(staged.c_str());
        return false;
    }

    SetEnvironmentVariableW(kSessionTagEnv, L"1");

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    // --melt <origExe> --meltDll <origDll>
    std::wstring cmd =
        L"\"" + staged.wstring() + L"\" --melt \"" + origExe.wstring() +
        L"\" --meltDll \"" + origDll.wstring() + L"\"";
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                             &si, &pi);
    if (!ok) {
        // CreateProcess failed — clean up both staged files so we don't
        // leave disk residue after a failed stage.
        DeleteFileW(staged.c_str());
        DeleteFileW(stagedDll.c_str());
        std::error_code rmec;
        std::filesystem::remove(staged.parent_path(), rmec);
        SetEnvironmentVariableW(kSessionTagEnv, nullptr);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ExitProcess(0);
    return true;
}

bool DeleteSelfOrigFromCmdLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool did = false;
    for (int i = 1; i + 1 < argc; ++i) {
        const auto flag = std::wstring_view(argv[i]);
        if (flag == L"--melt" || flag == L"--meltDll") {
            for (int tries = 0; tries < 20; ++tries) {
                if (DeleteFileW(argv[i + 1])) { did = true; break; }
                if (GetLastError() == ERROR_FILE_NOT_FOUND) { did = true; break; }
                Sleep(50);
            }
        }
    }
    LocalFree(argv);
    return did;
}

// Expose the staged DLL path to the UI: either the arg passed on cmdline
// (--stagedDll ... — reserved) or the sibling cheat.dll next to this exe.
std::filesystem::path CheatDllPathForThisRun() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    return std::filesystem::path(self).parent_path() / L"cheat.dll";
}

const wchar_t* RandomWindowTitle() {
    auto& s = CachedTitle();
    if (s.empty()) s = MakeRandomName();
    return s.c_str();
}
const wchar_t* RandomWindowClass() {
    auto& s = CachedClass();
    if (s.empty()) s = L"Wnd_" + MakeRandomName();
    return s.c_str();
}
const char* RandomImGuiTitle() {
    auto& s = CachedImGui();
    if (s.empty()) s = ToUtf8(MakeRandomName());
    return s.c_str();
}

} // namespace injector::randomize
