// injector/module_stomp.cpp — map-into-legit-module variant (§6.7).
#ifdef SK_MODULE_STOMP
#include "module_stomp.hpp"
#include "../common/ipc.hpp"

#include <winternl.h>
#include <psapi.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "psapi.lib")

using NtQIP_t = NTSTATUS (NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
static NtQIP_t NtQIP() {
    static NtQIP_t fn = reinterpret_cast<NtQIP_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    return fn;
}

namespace injector::stomp {

namespace {

// Shared helpers mirror manual_map.cpp. Lives here to keep the stomp flow
// a self-contained compilation unit (§6.7 is a fallback, kept isolated).

struct Img { const std::uint8_t* raw; const IMAGE_DOS_HEADER* dos;
             const IMAGE_NT_HEADERS64* nt; };
bool ParsePE(const std::vector<std::uint8_t>& bytes, Img& out) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    out.raw = bytes.data();
    out.dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(out.raw);
    if (out.dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    out.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(out.raw + out.dos->e_lfanew);
    return out.nt->Signature == IMAGE_NT_SIGNATURE &&
           out.nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
}

DWORD SectionProt(DWORD c) {
    const bool x = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool r = (c & IMAGE_SCN_MEM_READ) != 0;
    const bool w = (c & IMAGE_SCN_MEM_WRITE) != 0;
    if (c & IMAGE_SCN_MEM_DISCARDABLE) return PAGE_READONLY;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

HMODULE LocalDll(const char* name) {
    size_t n = std::strlen(name) + 1;
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_ACP, 0, name, -1, w.data(), static_cast<int>(n));
    HMODULE h = GetModuleHandleW(w.c_str());
    return h ? h : LoadLibraryW(w.c_str());
}

std::unordered_map<std::string, std::uintptr_t>
RemoteModules(HANDLE proc, std::string* out_candidate_match = nullptr,
              const std::vector<std::string>& candidates = {})
{
    std::unordered_map<std::string, std::uintptr_t> out;
    PROCESS_BASIC_INFORMATION pbi{}; ULONG ret = 0;
    auto fn = NtQIP();
    if (!fn || fn(proc, ProcessBasicInformation, &pbi, sizeof(pbi), &ret) != 0)
        return out;
    PEB peb{}; if (!ReadProcessMemory(proc, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr))
        return out;
    PEB_LDR_DATA ldr{};
    if (!ReadProcessMemory(proc, peb.Ldr, &ldr, sizeof(ldr), nullptr)) return out;

    LIST_ENTRY* head = &reinterpret_cast<PEB_LDR_DATA*>(peb.Ldr)->InMemoryOrderModuleList;
    LIST_ENTRY cur = ldr.InMemoryOrderModuleList;
    for (int i = 0; i < 512 && cur.Flink && cur.Flink != head; ++i) {
        auto entryAddr = reinterpret_cast<std::uint8_t*>(cur.Flink) -
                         offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        LDR_DATA_TABLE_ENTRY e{};
        if (!ReadProcessMemory(proc, entryAddr, &e, sizeof(e), nullptr)) break;
        if (e.FullDllName.Buffer && e.FullDllName.Length) {
            std::wstring w(e.FullDllName.Length / sizeof(wchar_t), 0);
            if (ReadProcessMemory(proc, e.FullDllName.Buffer, w.data(),
                                  e.FullDllName.Length, nullptr)) {
                auto pos = w.find_last_of(L"\\/");
                std::wstring base = (pos == std::wstring::npos) ? w : w.substr(pos + 1);
                std::transform(base.begin(), base.end(), base.begin(), ::towlower);
                std::string a(base.begin(), base.end());
                out[a] = reinterpret_cast<std::uintptr_t>(e.DllBase);
            }
        }
        cur = e.InMemoryOrderLinks;
    }
    (void)out_candidate_match; (void)candidates;
    return out;
}

// Find a contiguous unused RX region of size `need` inside a loaded module.
// We scan .text looking for a long run of 0xCC (int3) padding; most sections
// have 16-byte interior padding but tail padding before the next section
// can be far larger.
struct Cave { std::uintptr_t addr; std::size_t size; };
Cave FindCave(HANDLE proc, std::uintptr_t modBase, std::size_t need) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(modBase),
                           &dos, sizeof(dos), nullptr)) return {0,0};
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return {0,0};

    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(modBase + dos.e_lfanew),
                           &nt, sizeof(nt), nullptr)) return {0,0};

    std::vector<IMAGE_SECTION_HEADER> secs(nt.FileHeader.NumberOfSections);
    auto secBase = modBase + dos.e_lfanew + sizeof(IMAGE_NT_HEADERS64);
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(secBase),
                           secs.data(), secs.size() * sizeof(IMAGE_SECTION_HEADER),
                           nullptr)) return {0,0};

    // Scan each executable section's tail of 0xCC pad.
    for (const auto& s : secs) {
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        std::vector<std::uint8_t> buf(s.Misc.VirtualSize);
        if (!ReadProcessMemory(proc,
                reinterpret_cast<LPCVOID>(modBase + s.VirtualAddress),
                buf.data(), buf.size(), nullptr)) continue;

        std::size_t best_off = 0, best_len = 0, run_off = 0, run_len = 0;
        for (std::size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == 0xCC || buf[i] == 0x90) {
                if (run_len == 0) run_off = i;
                ++run_len;
                if (run_len > best_len) { best_len = run_len; best_off = run_off; }
            } else {
                run_len = 0;
            }
        }
        if (best_len >= need) {
            // Align to page.
            const std::uintptr_t raw = modBase + s.VirtualAddress + best_off;
            const std::uintptr_t aligned = (raw + 0xFFF) & ~static_cast<std::uintptr_t>(0xFFF);
            const std::size_t    slack  = best_len - (aligned - raw);
            if (slack >= need) return { aligned, slack };
        }
    }
    return {0, 0};
}

void ApplyRelocs(std::uint8_t* buf, const IMAGE_NT_HEADERS64* nt, std::intptr_t delta) {
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !delta) return;
    auto* rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>(buf + dir.VirtualAddress);
    DWORD total = dir.Size;
    while (total && rel->SizeOfBlock) {
        DWORD count = (rel->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        auto* e = reinterpret_cast<WORD*>(rel + 1);
        for (DWORD i = 0; i < count; ++i) {
            DWORD off = e[i] & 0x0FFF;
            switch (e[i] >> 12) {
                case IMAGE_REL_BASED_DIR64:
                    *reinterpret_cast<std::uint64_t*>(buf + rel->VirtualAddress + off) +=
                        static_cast<std::uint64_t>(delta);
                    break;
                case IMAGE_REL_BASED_HIGHLOW:
                    *reinterpret_cast<std::uint32_t*>(buf + rel->VirtualAddress + off) +=
                        static_cast<std::uint32_t>(delta);
                    break;
                default: break;
            }
        }
        total -= rel->SizeOfBlock;
        rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                  reinterpret_cast<std::uint8_t*>(rel) + rel->SizeOfBlock);
    }
}

std::uintptr_t RemoteExport(HANDLE proc, std::uintptr_t remoteBase, const char* name) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase),
                           &dos, sizeof(dos), nullptr) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase + dos.e_lfanew),
                           &nt, sizeof(nt), nullptr)) return 0;
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    std::vector<std::uint8_t> buf(dir.Size);
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase + dir.VirtualAddress),
                           buf.data(), dir.Size, nullptr)) return 0;
    auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(buf.data());
    std::vector<DWORD> names(ex->NumberOfNames);
    std::vector<WORD>  ords (ex->NumberOfNames);
    std::vector<DWORD> funcs(ex->NumberOfFunctions);
    ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase + ex->AddressOfNames),
                      names.data(), names.size()*4, nullptr);
    ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase + ex->AddressOfNameOrdinals),
                      ords.data(),  ords.size()*2, nullptr);
    ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase + ex->AddressOfFunctions),
                      funcs.data(), funcs.size()*4, nullptr);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        char n[256]{};
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteBase + names[i]),
                               n, sizeof(n)-1, nullptr)) continue;
        if (std::strcmp(n, name) == 0) return remoteBase + funcs[ords[i]];
    }
    return 0;
}

bool ResolveImports(std::uint8_t* local, const IMAGE_NT_HEADERS64* nt,
                    HANDLE proc,
                    const std::unordered_map<std::string, std::uintptr_t>& remote)
{
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(local + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* name = reinterpret_cast<const char*>(local + imp->Name);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = remote.find(lower);
        const std::uintptr_t remoteBase = (it != remote.end()) ? it->second : 0;

        HMODULE dll = LocalDll(name);
        std::intptr_t rebase = 0;
        if (dll && remoteBase)
            rebase = static_cast<std::intptr_t>(remoteBase) -
                     reinterpret_cast<std::intptr_t>(dll);

        auto* oft = reinterpret_cast<ULONGLONG*>(
            local + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* ft  = reinterpret_cast<ULONGLONG*>(local + imp->FirstThunk);
        for (; *oft; ++oft, ++ft) {
            ULONGLONG remoteAddr = 0;
            if (*oft & IMAGE_ORDINAL_FLAG64) {
                if (!dll) return false;
                FARPROC a = GetProcAddress(dll, reinterpret_cast<LPCSTR>(*oft & 0xFFFF));
                if (!a) return false;
                remoteAddr = reinterpret_cast<ULONGLONG>(a) + rebase;
            } else {
                auto* ib = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(local + (*oft & 0x7FFFFFFF));
                if (dll) {
                    FARPROC a = GetProcAddress(dll, ib->Name);
                    if (a) remoteAddr = reinterpret_cast<ULONGLONG>(a) + rebase;
                }
                if (!remoteAddr && remoteBase)
                    remoteAddr = RemoteExport(proc, remoteBase, ib->Name);
                if (!remoteAddr) return false;
            }
            *ft = remoteAddr;
        }
    }
    return true;
}

std::uintptr_t FindExport(const std::uint8_t* local, const IMAGE_NT_HEADERS64* nt,
                          std::uintptr_t remoteBase, const char* name)
{
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(local + dir.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(local + ex->AddressOfNames);
    auto* ords  = reinterpret_cast<const WORD*>(local + ex->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(local + ex->AddressOfFunctions);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        auto* n = reinterpret_cast<const char*>(local + names[i]);
        if (std::strcmp(n, name) == 0) return remoteBase + funcs[ords[i]];
    }
    return 0;
}

struct ShimLayout { std::uint8_t bytes[0x80]; std::size_t size; };
ShimLayout BuildShim(std::uintptr_t base, std::uintptr_t entry, std::uintptr_t savedRip) {
    ShimLayout sl{};
    std::uint8_t* p = sl.bytes;
    auto emit8  = [&](std::uint8_t  v) { *p++ = v; };
    auto emit64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    emit8(0x48); emit8(0x83); emit8(0xEC); emit8(0x28);
    emit8(0x48); emit8(0xB9); emit64((std::uint64_t)base);
    emit8(0x48); emit8(0xC7); emit8(0xC2); emit8(1); emit8(0); emit8(0); emit8(0);
    emit8(0x4D); emit8(0x31); emit8(0xC0);
    emit8(0x48); emit8(0xB8); emit64((std::uint64_t)entry);
    emit8(0xFF); emit8(0xD0);
    emit8(0x48); emit8(0x83); emit8(0xC4); emit8(0x28);
    emit8(0x48); emit8(0xB8); emit64((std::uint64_t)savedRip);
    emit8(0xFF); emit8(0xE0);
    sl.size = (std::size_t)(p - sl.bytes);
    return sl;
}

HANDLE OpenHijackableThread(DWORD pid, DWORD& outTid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    THREADENTRY32 te{ sizeof(te) };
    HANDLE best = nullptr; DWORD bestTid = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, te.th32ThreadID);
        if (!h) continue;
        if (!best || te.th32ThreadID < bestTid) {
            if (best) CloseHandle(best);
            best = h; bestTid = te.th32ThreadID;
        } else CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    outTid = bestTid;
    return best;
}

} // namespace

mapper::Result InjectStomp(DWORD pid,
                           const std::vector<std::uint8_t>& dllBytes,
                           const Options& opts,
                           std::function<void(const char*)> log)
{
    auto say = [&](const char* s) { if (log) log(s); };

    Img img{};
    if (!ParsePE(dllBytes, img))
        return { false, "parse", "PE invalid", 0 };

    HANDLE proc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE |
        PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!proc) return { false, "open", "OpenProcess failed", 0 };

    auto remoteMap = RemoteModules(proc);

    // Pick a candidate.
    std::uintptr_t stompBase = 0;
    Cave cave{0,0};
    std::string stompName;
    for (const auto& cand : opts.candidates) {
        auto it = remoteMap.find(cand);
        if (it == remoteMap.end()) continue;
        cave = FindCave(proc, it->second, img.nt->OptionalHeader.SizeOfImage);
        if (cave.size) { stompBase = cave.addr; stompName = cand; break; }
    }
    if (!stompBase) {
        CloseHandle(proc);
        return { false, "stomp-cave", "no module with enough RX padding", 0 };
    }
    say(("stomping into " + stompName).c_str());

    const DWORD imgSize = img.nt->OptionalHeader.SizeOfImage;
    std::vector<std::uint8_t> local(imgSize, 0);
    std::memcpy(local.data(), img.raw, img.nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(const_cast<IMAGE_NT_HEADERS64*>(img.nt));
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        DWORD copy = (std::min)(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize);
        if (copy)
            std::memcpy(local.data() + sec[i].VirtualAddress,
                        img.raw + sec[i].PointerToRawData, copy);
    }

    const std::intptr_t delta =
        static_cast<std::intptr_t>(stompBase - img.nt->OptionalHeader.ImageBase);
    ApplyRelocs(local.data(),
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    local.data() + img.dos->e_lfanew),
                delta);

    if (!ResolveImports(local.data(),
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(local.data() + img.dos->e_lfanew),
            proc, remoteMap)) {
        CloseHandle(proc);
        return { false, "imports", "IAT resolution failed", 0 };
    }

    // Flip stomped pages to RW first (they are RX), write, flip back per section.
    DWORD oldPage;
    VirtualProtectEx(proc, reinterpret_cast<LPVOID>(stompBase),
                     imgSize, PAGE_EXECUTE_READWRITE, &oldPage);
    if (!WriteProcessMemory(proc, reinterpret_cast<LPVOID>(stompBase),
                            local.data(), imgSize, nullptr)) {
        VirtualProtectEx(proc, reinterpret_cast<LPVOID>(stompBase),
                         imgSize, oldPage, &oldPage);
        CloseHandle(proc);
        return { false, "write", "WriteProcessMemory failed", 0 };
    }

    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        DWORD prot = SectionProt(sec[i].Characteristics);
        DWORD old = 0;
        VirtualProtectEx(proc,
            reinterpret_cast<LPVOID>(stompBase + sec[i].VirtualAddress),
            (std::max<DWORD>)(sec[i].Misc.VirtualSize, 1), prot, &old);
    }

    std::uintptr_t entry = FindExport(local.data(),
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(local.data() + img.dos->e_lfanew),
        stompBase, "sk_init");
    if (!entry) entry = stompBase + img.nt->OptionalHeader.AddressOfEntryPoint;

    LPVOID shimRemote = VirtualAllocEx(proc, nullptr, 0x1000,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!shimRemote) { CloseHandle(proc);
        return { false, "shim-alloc", "scratch alloc failed", stompBase }; }

    DWORD tid = 0;
    HANDLE th = OpenHijackableThread(pid, tid);
    if (!th) { VirtualFreeEx(proc, shimRemote, 0, MEM_RELEASE); CloseHandle(proc);
        return { false, "thread", "no hijackable thread", stompBase }; }

    SuspendThread(th);
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &ctx)) {
        ResumeThread(th); CloseHandle(th);
        VirtualFreeEx(proc, shimRemote, 0, MEM_RELEASE); CloseHandle(proc);
        return { false, "ctx", "GetThreadContext failed", stompBase };
    }
    auto shim = BuildShim(stompBase, entry, ctx.Rip);
    WriteProcessMemory(proc, shimRemote, shim.bytes, shim.size, nullptr);
    DWORD oldProt = 0;
    VirtualProtectEx(proc, shimRemote, shim.size, PAGE_EXECUTE_READ, &oldProt);
    ctx.Rip = reinterpret_cast<DWORD64>(shimRemote);
    SetThreadContext(th, &ctx);
    ResumeThread(th);
    CloseHandle(th);
    say("thread hijack dispatched (stomped)");

    DWORD waited = 0; bool ready = false;
    if (opts.readyFlagRva) {
        while (waited < opts.readyTimeoutMs) {
            ipc::Block b{};
            if (ReadProcessMemory(proc,
                    reinterpret_cast<LPCVOID>(stompBase + opts.readyFlagRva),
                    &b, sizeof(b), nullptr)) {
                if (b.magic == ipc::kMagic &&
                    (b.state == static_cast<uint32_t>(ipc::State::Ready) ||
                     b.state == static_cast<uint32_t>(ipc::State::Degraded))) {
                    ready = true; break;
                }
            }
            Sleep(50); waited += 50;
        }
    }

    // See manual_map.cpp: shim stays alive, 4KB RX inside cs2 — avoids UAF.
    CloseHandle(proc);
    if (!ready && opts.readyFlagRva)
        return { false, "ready", "module heartbeat timeout", stompBase };
    return { true, "ok", "stomped", stompBase };
}

} // namespace injector::stomp

#endif // SK_MODULE_STOMP
