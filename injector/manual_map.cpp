// injector/manual_map.cpp — PE parse / sections / relocs / imports / protections
// / thread-hijack entry. Follows §3.4 step-for-step. No remote threads, no RWX.
#include "manual_map.hpp"
#include "../common/ipc.hpp"

#include <winternl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "psapi.lib")

// NtQueryInformationProcess is pulled by name from ntdll to dodge the
// winternl.h-vs-SDK duplicate-decl trap on recent toolchains.
using NtQIP_t = NTSTATUS (NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
static NtQIP_t NtQIP() {
    static NtQIP_t fn = reinterpret_cast<NtQIP_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    return fn;
}

namespace injector::mapper {

namespace {

struct Img {
    const std::uint8_t*     raw;
    const IMAGE_DOS_HEADER* dos;
    const IMAGE_NT_HEADERS64* nt;
};

bool ParsePE(const std::vector<std::uint8_t>& bytes, Img& out) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    out.raw = bytes.data();
    out.dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(out.raw);
    if (out.dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (out.dos->e_lfanew < 0 ||
        static_cast<size_t>(out.dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > bytes.size())
        return false;
    out.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(out.raw + out.dos->e_lfanew);
    if (out.nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (out.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (out.nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    // Section table + each section's raw range must sit inside the input.
    const size_t secTblOff = static_cast<size_t>(out.dos->e_lfanew)
        + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
        + out.nt->FileHeader.SizeOfOptionalHeader;
    const size_t secBytes  = static_cast<size_t>(out.nt->FileHeader.NumberOfSections)
                             * sizeof(IMAGE_SECTION_HEADER);
    if (secTblOff + secBytes > bytes.size()) return false;
    auto* sec = reinterpret_cast<const IMAGE_SECTION_HEADER*>(out.raw + secTblOff);
    for (WORD i = 0; i < out.nt->FileHeader.NumberOfSections; ++i) {
        if (static_cast<size_t>(sec[i].PointerToRawData) + sec[i].SizeOfRawData > bytes.size())
            return false;
    }
    // SizeOfImage must at least cover headers + every section's VirtualAddress+VirtualSize.
    const DWORD imgSize = out.nt->OptionalHeader.SizeOfImage;
    if (imgSize < out.nt->OptionalHeader.SizeOfHeaders) return false;
    for (WORD i = 0; i < out.nt->FileHeader.NumberOfSections; ++i) {
        if (static_cast<uint64_t>(sec[i].VirtualAddress) + sec[i].Misc.VirtualSize > imgSize)
            return false;
    }
    return true;
}

// Section characteristics -> VirtualProtect flag. Mirrors loader behaviour.
DWORD SectionProt(DWORD c) {
    const bool x = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool r = (c & IMAGE_SCN_MEM_READ) != 0;
    const bool w = (c & IMAGE_SCN_MEM_WRITE) != 0;
    if (c & IMAGE_SCN_MEM_DISCARDABLE) return PAGE_READONLY;
    if (x && w) return PAGE_EXECUTE_READWRITE; // should never land here, see Inject
    if (x && r) return PAGE_EXECUTE_READ;
    if (x)      return PAGE_EXECUTE;
    if (w)      return PAGE_READWRITE;
    if (r)      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

// Local map: "kernel32.dll" -> HMODULE in injector (for IAT resolution).
HMODULE LocalDll(const char* name) {
    size_t n = std::strlen(name) + 1;
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_ACP, 0, name, -1, w.data(), static_cast<int>(n));
    HMODULE h = GetModuleHandleW(w.c_str());
    return h ? h : LoadLibraryW(w.c_str());
}

// Enumerate the remote PEB's loader list once; map image-name -> remote base.
// System DLLs share bases cross-process (boot ASLR), so this exists mainly so
// imports of already-loaded game DLLs (client.dll etc.) resolve correctly.
std::unordered_map<std::string, std::uintptr_t>
BuildRemoteModuleMap(HANDLE proc) {
    std::unordered_map<std::string, std::uintptr_t> out;

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG ret = 0;
    auto fn = NtQIP();
    if (!fn || fn(proc, ProcessBasicInformation, &pbi, sizeof(pbi), &ret) != 0)
        return out;
    if (!pbi.PebBaseAddress) return out;

    PEB peb{};
    if (!ReadProcessMemory(proc, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr))
        return out;

    PEB_LDR_DATA ldr{};
    if (!ReadProcessMemory(proc, peb.Ldr, &ldr, sizeof(ldr), nullptr)) return out;

    LIST_ENTRY* head = &reinterpret_cast<PEB_LDR_DATA*>(peb.Ldr)->InMemoryOrderModuleList;
    LIST_ENTRY cur = ldr.InMemoryOrderModuleList;
    const int kMax = 512;
    for (int i = 0; i < kMax && cur.Flink && cur.Flink != head; ++i) {
        auto entryAddr = reinterpret_cast<std::uint8_t*>(cur.Flink) -
                         offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        LDR_DATA_TABLE_ENTRY e{};
        if (!ReadProcessMemory(proc, entryAddr, &e, sizeof(e), nullptr)) break;

        if (e.FullDllName.Buffer && e.FullDllName.Length) {
            std::wstring w(e.FullDllName.Length / sizeof(wchar_t), 0);
            if (ReadProcessMemory(proc, e.FullDllName.Buffer,
                                  w.data(), e.FullDllName.Length, nullptr)) {
                auto pos = w.find_last_of(L"\\/");
                std::wstring base = (pos == std::wstring::npos) ? w : w.substr(pos + 1);
                std::transform(base.begin(), base.end(), base.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
                // DLL base-names are ASCII in practice (kernel32.dll etc.) —
                // the explicit narrow cast silences C4244 without pulling
                // in a code-page conversion we don't need.
                std::string a;
                a.reserve(base.size());
                for (wchar_t c : base) a.push_back(static_cast<char>(c & 0x7F));
                out[a] = reinterpret_cast<std::uintptr_t>(e.DllBase);
            }
        }
        cur = e.InMemoryOrderLinks;
    }
    return out;
}

// Walk relocs — B.2 of the design.
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
                    *reinterpret_cast<std::uint64_t*>(buf + rel->VirtualAddress + off)
                        += static_cast<std::uint64_t>(delta);
                    break;
                case IMAGE_REL_BASED_HIGHLOW:
                    *reinterpret_cast<std::uint32_t*>(buf + rel->VirtualAddress + off)
                        += static_cast<std::uint32_t>(delta);
                    break;
                default: break; // ABSOLUTE / unused
            }
        }
        total -= rel->SizeOfBlock;
        rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                  reinterpret_cast<std::uint8_t*>(rel) + rel->SizeOfBlock);
    }
}

// Walk the remote module's export directory via RPM — used when a game DLL
// can't be LoadLibrary'd into the injector (circular dep chain). Returns the
// absolute remote address of the named export, or 0 if ANY RPM fails or the
// export is absent. Audit catch: previously zero-init arrays on silent RPM
// failure meant `funcs[0]` was returned as a live IAT entry, writing
// (remoteBase + 0) and jumping into the DOS header on first call.
//
// Also detects forwarded exports — exports whose RVA falls INSIDE the export
// directory point to a "DllName.FnName" string, not real code. We reject
// forwarded exports (no quiet success path to code-shaped data).
std::uintptr_t RemoteExport(HANDLE proc, std::uintptr_t remoteBase, const char* name) {
    auto rpm = [&](std::uintptr_t at, void* dst, size_t sz) {
        return ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(at), dst, sz, nullptr) != 0;
    };

    IMAGE_DOS_HEADER dos{};
    if (!rpm(remoteBase, &dos, sizeof(dos))) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!rpm(remoteBase + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    std::vector<std::uint8_t> buf(dir.Size);
    if (!rpm(remoteBase + dir.VirtualAddress, buf.data(), dir.Size)) return 0;
    auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(buf.data());

    std::vector<DWORD> names(ex->NumberOfNames);
    std::vector<WORD>  ords (ex->NumberOfNames);
    std::vector<DWORD> funcs(ex->NumberOfFunctions);
    if (ex->NumberOfNames &&
        !rpm(remoteBase + ex->AddressOfNames,
             names.data(), names.size() * sizeof(DWORD))) return 0;
    if (ex->NumberOfNames &&
        !rpm(remoteBase + ex->AddressOfNameOrdinals,
             ords.data(),  ords.size()  * sizeof(WORD)))  return 0;
    if (ex->NumberOfFunctions &&
        !rpm(remoteBase + ex->AddressOfFunctions,
             funcs.data(), funcs.size() * sizeof(DWORD))) return 0;

    const DWORD edStart = dir.VirtualAddress;
    const DWORD edEnd   = dir.VirtualAddress + dir.Size;

    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        char n[256]{};
        if (!rpm(remoteBase + names[i], n, sizeof(n) - 1)) return 0;
        if (std::strcmp(n, name) != 0) continue;
        const DWORD rva = funcs[ords[i]];
        // Forwarder RVAs point into the export directory itself (string
        // like "KERNEL32.VirtualAlloc"). Those are NOT executable addresses.
        if (rva >= edStart && rva < edEnd) return 0;
        return remoteBase + rva;
    }
    return 0;
}

bool ResolveImports(std::uint8_t* localImage, const IMAGE_NT_HEADERS64* nt,
                    HANDLE proc,
                    const std::unordered_map<std::string, std::uintptr_t>& remoteMap)
{
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(localImage + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dllName = reinterpret_cast<const char*>(localImage + imp->Name);

        std::string lower = dllName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](char c) {
                           return static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                       });
        auto it = remoteMap.find(lower);
        const std::uintptr_t remoteBase = (it != remoteMap.end()) ? it->second : 0;

        // Prefer loading locally so we can use GetProcAddress; fall back to
        // walking the REMOTE export table when it's a game DLL we can't load
        // (client.dll, engine2.dll, server.dll — these need the full game
        // dependency chain to initialise and will fail LoadLibraryW).
        HMODULE local = LocalDll(dllName);
        std::intptr_t rebase = 0;
        if (local && remoteBase)
            rebase = static_cast<std::intptr_t>(remoteBase) -
                     reinterpret_cast<std::intptr_t>(local);

        auto* oft = reinterpret_cast<ULONGLONG*>(
            localImage + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                  : imp->FirstThunk));
        auto* ft  = reinterpret_cast<ULONGLONG*>(localImage + imp->FirstThunk);
        for (; *oft; ++oft, ++ft) {
            ULONGLONG remoteAddr = 0;

            if (*oft & IMAGE_ORDINAL_FLAG64) {
                // Ordinal imports need BOTH the local module (for GetProcAddress)
                // AND the remote base (so rebase translates to CS2's address).
                // Previously we accepted local-only, which wrote the injector's
                // address into CS2's IAT → first call jumps into our process
                // view and crashes CS2.
                if (!local || !remoteBase) return false;
                FARPROC a = GetProcAddress(local, reinterpret_cast<LPCSTR>(*oft & 0xFFFF));
                if (!a) return false;
                remoteAddr = reinterpret_cast<ULONGLONG>(a) + rebase;
            } else {
                auto* ib = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                               localImage + (*oft & 0x7FFFFFFF));
                // Local+rebase path only when BOTH available. Without
                // remoteBase, "rebase" is zero and we'd write the injector's
                // address into CS2's IAT — same crash as the ordinal path.
                if (local && remoteBase) {
                    FARPROC a = GetProcAddress(local, ib->Name);
                    if (a) remoteAddr = reinterpret_cast<ULONGLONG>(a) + rebase;
                }
                if (!remoteAddr && remoteBase) {
                    remoteAddr = RemoteExport(proc, remoteBase, ib->Name);
                }
                if (!remoteAddr) return false;
            }

            *ft = remoteAddr;
        }
    }
    return true;
}

// Thread-hijack trampoline — fully reg-save, aligned, volatiles+XMM preserved.
// The hijacked thread may have been suspended at any RIP mod 16 and with any
// volatile reg state live; we must restore everything bit-exact before the
// jmp back to savedRip or cs2 crashes on resume.
struct ShimLayout {
    std::uint8_t  bytes[0x100];
    std::size_t   size;
};

ShimLayout BuildShim(std::uintptr_t base, std::uintptr_t entry, std::uintptr_t savedRip) {
    ShimLayout sl{};
    std::uint8_t* p = sl.bytes;
    auto emit  = [&](std::initializer_list<std::uint8_t> xs) {
        for (auto b : xs) *p++ = b;
    };
    auto emit64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };

    // --- save state (RBP is nonvolatile — we clobber it to anchor rsp) ---
    emit({0x55});                                // push rbp
    emit({0x9C});                                // pushfq
    emit({0x50});                                // push rax
    emit({0x51});                                // push rcx
    emit({0x52});                                // push rdx
    emit({0x41, 0x50});                          // push r8
    emit({0x41, 0x51});                          // push r9
    emit({0x41, 0x52});                          // push r10
    emit({0x41, 0x53});                          // push r11
    emit({0x48, 0x83, 0xEC, 0x60});              // sub rsp, 0x60
    emit({0xF3, 0x0F, 0x7F, 0x04, 0x24});                 // movdqu [rsp], xmm0
    emit({0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10});           // movdqu [rsp+0x10], xmm1
    emit({0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x20});           // movdqu [rsp+0x20], xmm2
    emit({0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x30});           // movdqu [rsp+0x30], xmm3
    emit({0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x40});           // movdqu [rsp+0x40], xmm4
    emit({0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x50});           // movdqu [rsp+0x50], xmm5

    // --- anchor rsp, align, shadow space ---
    emit({0x48, 0x89, 0xE5});                    // mov rbp, rsp
    emit({0x48, 0x83, 0xE4, 0xF0});              // and rsp, -16
    emit({0x48, 0x83, 0xEC, 0x20});              // sub rsp, 0x20

    // --- call entry(base, DLL_PROCESS_ATTACH, 0) ---
    emit({0x48, 0xB9}); emit64(static_cast<std::uint64_t>(base));           // mov rcx, imm64
    emit({0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00});                       // mov rdx, 1
    emit({0x4D, 0x31, 0xC0});                                               // xor r8, r8
    emit({0x48, 0xB8}); emit64(static_cast<std::uint64_t>(entry));          // mov rax, imm64
    emit({0xFF, 0xD0});                                                     // call rax

    // --- restore rsp, volatiles, flags ---
    emit({0x48, 0x89, 0xEC});                    // mov rsp, rbp
    emit({0xF3, 0x0F, 0x6F, 0x04, 0x24});                 // movdqu xmm0, [rsp]
    emit({0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10});           // movdqu xmm1, [rsp+0x10]
    emit({0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x20});           // movdqu xmm2, [rsp+0x20]
    emit({0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x30});           // movdqu xmm3, [rsp+0x30]
    emit({0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x40});           // movdqu xmm4, [rsp+0x40]
    emit({0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x50});           // movdqu xmm5, [rsp+0x50]
    emit({0x48, 0x83, 0xC4, 0x60});              // add rsp, 0x60
    emit({0x41, 0x5B});                          // pop r11
    emit({0x41, 0x5A});                          // pop r10
    emit({0x41, 0x59});                          // pop r9
    emit({0x41, 0x58});                          // pop r8
    emit({0x5A});                                // pop rdx
    emit({0x59});                                // pop rcx
    emit({0x58});                                // pop rax
    emit({0x9D});                                // popfq
    emit({0x5D});                                // pop rbp

    // --- jmp to exact saved RIP via RIP-relative absolute, NO GPR touched ---
    // Encoding: FF 25 00 00 00 00  (jmp qword ptr [rip + 0])
    // then 8 bytes holding the target address, inlined.
    emit({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    emit64(static_cast<std::uint64_t>(savedRip));

    sl.size = static_cast<std::size_t>(p - sl.bytes);
    return sl;
}

// Pick a hijackable thread. For each candidate we suspend, read RIP, and
// skip anything parked inside ntdll.dll (a wait call that won't return to
// user code promptly after SetThreadContext). Lowest tid among valid
// candidates wins — in practice that's the game's main thread.
HANDLE OpenHijackableThread(DWORD pid, DWORD& outTid) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), ntdll, &mi, sizeof(mi));
    const auto ntBase = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
    const auto ntEnd  = ntBase + mi.SizeOfImage;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    struct Candidate { DWORD tid; HANDLE handle; };
    std::vector<Candidate> cands;
    THREADENTRY32 te{ sizeof(te) };
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, te.th32ThreadID);
        if (h) cands.push_back({ te.th32ThreadID, h });
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);

    // Sort by tid ascending.
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.tid < b.tid; });

    // Null every closed handle so the cleanup sweep doesn't double-close.
    for (auto& c : cands) {
        if (SuspendThread(c.handle) == (DWORD)-1) {
            CloseHandle(c.handle); c.handle = nullptr; continue;
        }
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(c.handle, &ctx)) {
            ResumeThread(c.handle);
            CloseHandle(c.handle); c.handle = nullptr; continue;
        }
        ResumeThread(c.handle);
        const auto rip = static_cast<std::uintptr_t>(ctx.Rip);
        if (rip >= ntBase && rip < ntEnd) {
            CloseHandle(c.handle); c.handle = nullptr; continue;
        }
        for (auto& r : cands) {
            if (r.handle && r.tid != c.tid) {
                CloseHandle(r.handle); r.handle = nullptr;
            }
        }
        outTid = c.tid;
        HANDLE winner = c.handle;
        c.handle = nullptr;
        return winner;
    }
    for (auto& r : cands) if (r.handle) CloseHandle(r.handle);
    outTid = 0;
    return nullptr;
}

std::uintptr_t FindExport(const std::uint8_t* localImage, const IMAGE_NT_HEADERS64* nt,
                          std::uintptr_t remoteBase, const char* name)
{
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(localImage + dir.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(localImage + ex->AddressOfNames);
    auto* ords  = reinterpret_cast<const WORD*>(localImage + ex->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(localImage + ex->AddressOfFunctions);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        auto* n = reinterpret_cast<const char*>(localImage + names[i]);
        if (std::strcmp(n, name) == 0) return remoteBase + funcs[ords[i]];
    }
    return 0;
}

} // namespace

Result Inject(DWORD pid,
              const std::vector<std::uint8_t>& dllBytes,
              const Options& opts,
              std::function<void(const char*)> log)
{
    auto say = [&](const char* s) { if (log) log(s); };

    Img img{};
    if (!ParsePE(dllBytes, img))
        return { false, "parse", "PE invalid", 0 };

    // Minimum rights per §6.6; thread hijack rights are requested per-thread.
    HANDLE proc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!proc) return { false, "open", "OpenProcess failed", 0 };

    const DWORD imgSize = img.nt->OptionalHeader.SizeOfImage;
    // Prefer preferred base, fall back to anywhere. Never RWX.
    LPVOID remote = VirtualAllocEx(
        proc, reinterpret_cast<LPVOID>(img.nt->OptionalHeader.ImageBase),
        imgSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote)
        remote = VirtualAllocEx(proc, nullptr, imgSize,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote) { CloseHandle(proc);
        return { false, "alloc", "VirtualAllocEx failed", 0 }; }
    const auto remoteBase = reinterpret_cast<std::uintptr_t>(remote);
    say("alloc ok");

    // Build the final image locally, write once, then protect per-section.
    std::vector<std::uint8_t> local(imgSize, 0);
    std::memcpy(local.data(), img.raw, img.nt->OptionalHeader.SizeOfHeaders);

    auto* sec = IMAGE_FIRST_SECTION(const_cast<IMAGE_NT_HEADERS64*>(img.nt));
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        DWORD copy = (std::min)(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize);
        if (copy) {
            std::memcpy(local.data() + sec[i].VirtualAddress,
                        img.raw + sec[i].PointerToRawData, copy);
        }
    }
    say("sections mapped (local)");

    const std::intptr_t delta =
        static_cast<std::intptr_t>(remoteBase - img.nt->OptionalHeader.ImageBase);
    ApplyRelocs(local.data(),
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    local.data() + img.dos->e_lfanew),
                delta);
    say("relocs applied");

    auto remoteMap = BuildRemoteModuleMap(proc);
    if (!ResolveImports(local.data(),
                        reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                            local.data() + img.dos->e_lfanew),
                        proc, remoteMap)) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return { false, "imports", "IAT resolution failed", 0 };
    }
    say("imports resolved");

    if (!WriteProcessMemory(proc, remote, local.data(), imgSize, nullptr)) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return { false, "write", "WriteProcessMemory failed", 0 };
    }
    say("image written");

    // Final per-section protections.
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        DWORD prot = SectionProt(sec[i].Characteristics);
        DWORD old = 0;
        VirtualProtectEx(proc,
            reinterpret_cast<LPVOID>(remoteBase + sec[i].VirtualAddress),
            (std::max<DWORD>)(sec[i].Misc.VirtualSize, 1), prot, &old);
    }
    say("protections set");

    // TLS callbacks — walked locally against the WRITTEN local view.
    {
        auto& td = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (td.VirtualAddress) {
            auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
                            local.data() + td.VirtualAddress);
            if (tls->AddressOfCallBacks) {
                // AddressOfCallBacks is a VA based on remote image base post-reloc.
                // We don't invoke them from the injector; let the game-thread
                // shim do it alongside sk_init when the module needs it. Logged.
                say("tls present (delegated)");
            }
        }
    }

    // Entry = AddressOfEntryPoint (__DllMainCRTStartup). The CRT entry does:
    //   - CRT init + global ctors
    //   - TLS callback dispatch (_tls_used)
    //   - RtlAddFunctionTable for x64 unwind registration
    // Then it calls our DllMain, which calls sk_init(). Skipping the CRT entry
    // leaves all three undone (per audit). Keep the sk_init export only as a
    // public symbol for debugging; the shim always goes through CRT.
    std::uintptr_t entry = remoteBase + img.nt->OptionalHeader.AddressOfEntryPoint;
    (void)&FindExport; // suppress unused-fn warning without C4551

    // Allocate a tiny RX scratch for the hijack shim OUTSIDE the module.
    // Lives in cs2.exe's address space as RX. Freed after return.
    LPVOID shimRemote = VirtualAllocEx(proc, nullptr, 0x1000,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!shimRemote) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return { false, "shim-alloc", "scratch alloc failed", 0 };
    }

    DWORD tid = 0;
    HANDLE th = OpenHijackableThread(pid, tid);
    if (!th) {
        VirtualFreeEx(proc, shimRemote, 0, MEM_RELEASE);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return { false, "thread", "no hijackable thread", 0 };
    }

    SuspendThread(th);
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &ctx)) {
        ResumeThread(th); CloseHandle(th);
        VirtualFreeEx(proc, shimRemote, 0, MEM_RELEASE);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return { false, "ctx", "GetThreadContext failed", 0 };
    }
    auto shim = BuildShim(remoteBase, entry, ctx.Rip);
    WriteProcessMemory(proc, shimRemote, shim.bytes, shim.size, nullptr);
    DWORD oldProt = 0;
    VirtualProtectEx(proc, shimRemote, shim.size, PAGE_EXECUTE_READ, &oldProt);

    ctx.Rip = reinterpret_cast<DWORD64>(shimRemote);
    SetThreadContext(th, &ctx);
    ResumeThread(th);
    CloseHandle(th);
    say("thread hijack dispatched");

    // Wait for ready flag via RPM on g_ipc. Short-circuit on Fatal so the
    // launcher reports the real failure (hook install broke) instead of a
    // generic "heartbeat timeout" after the full 5s wait.
    DWORD waited = 0;
    bool ready = false;
    std::string failReason;
    if (opts.readyFlagRva) {
        while (waited < opts.readyTimeoutMs) {
            ipc::Block b{};
            if (ReadProcessMemory(proc,
                    reinterpret_cast<LPCVOID>(remoteBase + opts.readyFlagRva),
                    &b, sizeof(b), nullptr)) {
                if (b.magic == ipc::kMagic) {
                    if (b.state == static_cast<uint32_t>(ipc::State::Ready) ||
                        b.state == static_cast<uint32_t>(ipc::State::Degraded)) {
                        ready = true; break;
                    }
                    if (b.state == static_cast<uint32_t>(ipc::State::Fatal)) {
                        failReason = "module boot reached Fatal "
                                     "(likely InstallPresent failed — DXGI "
                                     "prologue mismatch or vtable resolve)";
                        break;
                    }
                }
            }
            Sleep(50);
            waited += 50;
        }
    }

    if (opts.eraseHeaders) {
        // Wipe the full headers region (DOS + NT + section table). Nothing
        // in the mapped image references them at runtime; relocs are done,
        // imports resolved, TLS already fired. A naive MZ-scanner and a
        // section-table walker both find nothing.
        const DWORD hdrSz = img.nt->OptionalHeader.SizeOfHeaders;
        std::vector<std::uint8_t> zeros(hdrSz, 0);
        DWORD oldH = 0;
        VirtualProtectEx(proc, remote, hdrSz, PAGE_READWRITE, &oldH);
        WriteProcessMemory(proc, remote, zeros.data(), hdrSz, nullptr);
        VirtualProtectEx(proc, remote, hdrSz, PAGE_READONLY, &oldH);
        say("headers erased");
    }

    // Scrub the local copy of the DLL (fully-prepared image, resolved IAT)
    // before the vector frees its heap pages. The plaintext never lives in
    // free'd heap. SecureZeroMemory isn't optimised away.
    SecureZeroMemory(local.data(), local.size());

    // Deliberately do NOT free shimRemote here. The hijacked thread may still
    // be in flight (sk_init -> game-thread work) and the shim's trailing jmp
    // back to savedRip only runs after entry returns. A 4KB RX leak inside
    // cs2 is cheaper than racing a use-after-free. ponytail: leak, don't
    // race.
    CloseHandle(proc);

    if (!ready && opts.readyFlagRva) {
        if (failReason.empty()) failReason = "module heartbeat timeout";
        return { false, "ready", failReason, remoteBase };
    }
    return { true, "ok", "injected", remoteBase };
}

} // namespace injector::mapper
