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
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (!n) return {};
    std::filesystem::path tmp(buf, buf + n);
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

    auto staged = MakeStagingPath();
    if (staged.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(staged.parent_path(), ec);
    if (ec) return false;

    if (!CopyFileW(self, staged.c_str(), FALSE)) return false;

    SetEnvironmentVariableW(kSessionTagEnv, L"1");

    // Launch staged copy with the original path on its command line. The
    // staged process does the delete directly on first run — avoids the
    // cmd.exe-ping-del dance that lights up Sysmon/EDR rules.
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd =
        L"\"" + staged.wstring() + L"\" --melt \"" + std::wstring(self) + L"\"";
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                             &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ExitProcess(0);
    return true;
}

bool DeleteSelfOrigFromCmdLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--melt") {
            // Retry a few times: the parent may still hold the file briefly.
            for (int tries = 0; tries < 20; ++tries) {
                if (DeleteFileW(argv[i + 1])) break;
                Sleep(50);
            }
            LocalFree(argv);
            return true;
        }
    }
    LocalFree(argv);
    return false;
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
