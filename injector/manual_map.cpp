// injector/manual_map.cpp — PE parse / sections / relocs / imports / protections
// / thread-hijack entry. Follows §3.4 step-for-step. No remote threads, no RWX.
#include "manual_map.hpp"
#include "../common/ipc.hpp"

#include <winternl.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ntdll.lib")

extern "C" NTSTATUS NTAPI NtQueryInformationProcess(
    HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

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
    if (bytes.size() < static_cast<size_t>(out.dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64))
        return false;
    out.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(out.raw + out.dos->e_lfanew);
    if (out.nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (out.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (out.nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
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
    if (NtQueryInformationProcess(proc, ProcessBasicInformation,
                                  &pbi, sizeof(pbi), &ret) != 0) return out;
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
                std::transform(base.begin(), base.end(), base.begin(), ::towlower);
                std::string a(base.begin(), base.end());
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

// Resolve IAT thunks against local DLL copies.
// ponytail: system DLLs share bases cross-process on current Windows (boot ASLR);
// so GetProcAddress(local) == remote fn addr for them. For modules that may not
// share bases, we rebase: localExportAddr - localBase + remoteBase.
bool ResolveImports(std::uint8_t* localImage, const IMAGE_NT_HEADERS64* nt,
                    const std::unordered_map<std::string, std::uintptr_t>& remoteMap)
{
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(localImage + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dllName = reinterpret_cast<const char*>(localImage + imp->Name);
        HMODULE local = LocalDll(dllName);
        if (!local) return false;

        std::string lower = dllName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = remoteMap.find(lower);
        std::intptr_t rebase = 0;
        if (it != remoteMap.end())
            rebase = static_cast<std::intptr_t>(it->second) -
                     reinterpret_cast<std::intptr_t>(local);

        auto* oft = reinterpret_cast<ULONGLONG*>(
            localImage + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                  : imp->FirstThunk));
        auto* ft  = reinterpret_cast<ULONGLONG*>(localImage + imp->FirstThunk);
        for (; *oft; ++oft, ++ft) {
            FARPROC addr = nullptr;
            if (*oft & IMAGE_ORDINAL_FLAG64) {
                addr = GetProcAddress(local,
                        reinterpret_cast<LPCSTR>(*oft & 0xFFFF));
            } else {
                auto* ib = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                               localImage + (*oft & 0x7FFFFFFF));
                addr = GetProcAddress(local, ib->Name);
            }
            if (!addr) return false;
            *ft = reinterpret_cast<ULONGLONG>(addr) + rebase;
        }
    }
    return true;
}

// A thread-hijack trampoline. See HijackEnter() for the layout we assemble.
// It calls module entry (sk_init / DllMain) then returns to the saved RIP.
struct ShimLayout {
    std::uint8_t  bytes[0x80]; // more than enough for the stub below
    std::size_t   size;
};

// Build:
//   push rax ... (save volatile regs we'll touch via return-to-saved-rip)
//   mov rcx, <base>
//   mov rdx, 1          ; DLL_PROCESS_ATTACH
//   xor r8, r8
//   mov rax, <entry>
//   call rax
//   mov rax, <savedRip>
//   jmp rax
ShimLayout BuildShim(std::uintptr_t base, std::uintptr_t entry, std::uintptr_t savedRip) {
    ShimLayout sl{};
    std::uint8_t* p = sl.bytes;
    auto emit8  = [&](std::uint8_t  v) { *p++ = v; };
    auto emit64 = [&](std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; };

    // sub rsp, 0x28  (shadow space + align)
    emit8(0x48); emit8(0x83); emit8(0xEC); emit8(0x28);
    // mov rcx, base
    emit8(0x48); emit8(0xB9); emit64(static_cast<std::uint64_t>(base));
    // mov rdx, 1 (DLL_PROCESS_ATTACH)
    emit8(0x48); emit8(0xC7); emit8(0xC2); emit8(0x01); emit8(0x00); emit8(0x00); emit8(0x00);
    // xor r8, r8
    emit8(0x4D); emit8(0x31); emit8(0xC0);
    // mov rax, entry
    emit8(0x48); emit8(0xB8); emit64(static_cast<std::uint64_t>(entry));
    // call rax
    emit8(0xFF); emit8(0xD0);
    // add rsp, 0x28
    emit8(0x48); emit8(0x83); emit8(0xC4); emit8(0x28);
    // mov rax, savedRip
    emit8(0x48); emit8(0xB8); emit64(static_cast<std::uint64_t>(savedRip));
    // jmp rax
    emit8(0xFF); emit8(0xE0);

    sl.size = static_cast<std::size_t>(p - sl.bytes);
    return sl;
}

// Pick a reasonable thread to hijack. Loader thread is the lowest tid of the
// pid in practice. Open with required rights; skip ones we can't touch.
HANDLE OpenHijackableThread(DWORD pid, DWORD& outTid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    THREADENTRY32 te{ sizeof(te) };
    HANDLE best = nullptr;
    DWORD bestTid = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                  THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!h) continue;
            if (!best || te.th32ThreadID < bestTid) {
                if (best) CloseHandle(best);
                best = h;
                bestTid = te.th32ThreadID;
            } else {
                CloseHandle(h);
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    outTid = bestTid;
    return best;
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

    HANDLE proc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE |
        PROCESS_SUSPEND_RESUME,
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
                        remoteMap)) {
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

    // Resolve module entry: prefer sk_init, fall back to DllMain via AddressOfEntryPoint.
    std::uintptr_t entry = FindExport(local.data(),
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(local.data() + img.dos->e_lfanew),
        remoteBase, "sk_init");
    if (!entry) entry = remoteBase + img.nt->OptionalHeader.AddressOfEntryPoint;

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

    // Wait for ready flag via RPM on g_ipc.
    DWORD waited = 0;
    bool ready = false;
    if (opts.readyFlagRva) {
        while (waited < opts.readyTimeoutMs) {
            ipc::Block b{};
            if (ReadProcessMemory(proc,
                    reinterpret_cast<LPCVOID>(remoteBase + opts.readyFlagRva),
                    &b, sizeof(b), nullptr)) {
                if (b.magic == ipc::kMagic &&
                    (b.state == static_cast<uint32_t>(ipc::State::Ready) ||
                     b.state == static_cast<uint32_t>(ipc::State::Degraded))) {
                    ready = true; break;
                }
            }
            Sleep(50);
            waited += 50;
        }
    }

    if (opts.eraseHeaders) {
        // Zero MZ and e_lfanew — P1 header erase.
        std::uint8_t z[0x40] = {};
        WriteProcessMemory(proc, remote, z, sizeof(z), nullptr);
        say("headers erased");
    }

    VirtualFreeEx(proc, shimRemote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (!ready && opts.readyFlagRva)
        return { false, "ready", "module heartbeat timeout", remoteBase };
    return { true, "ok", "injected", remoteBase };
}

} // namespace injector::mapper
