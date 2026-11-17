/*
 * k32host: a Windows 98 console program on Windows 98's own lifted
 * kernel32 (milestone 1, docs/survey.md). kernel32 is booted the way VWIN32
 * boots it: its _FREQASM section starts with a "KEXP" header whose third
 * dword is the boot entry, called with a boot-info block. Every VxD service
 * kernel32 asks for is answered by pcrecomp's vxd32.c.
 *
 *   build\xcopy.exe [--mute] [--watchdog S] [-- args]
 *
 * kernel32 has no relocations: it is mapped at its own 0xBFF70000 (the host
 * is /LARGEADDRESSAWARE, so that is user space under WOW64).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include "native32.h"
extern const char* g_cur_import;
int recomp_trace_arg(int argc, char** argv, int i);
void recomp_trace_flush(void);
uint32_t native32_vwin32_r0_table(void);   /* vxd32.c */
void native32_w9x_ldt(uint16_t alias_sel, uint32_t* ldt_base);
uint16_t native32_w9x_alloc_sel(uint32_t base, uint32_t limit);
extern uint16_t g_w9x_psp_sel;
uint32_t native32_w9x_krnl386_thunk(void), native32_w9x_user_signal_proc(void), native32_w9x_message_box(void);

#define K32_BASE 0xBFF70000u
static native32_module_t g_mods[] = { { APP_EXE, APP_BASE }, { "kernel32.dll", K32_BASE } };
#define NMODS ((int)(sizeof g_mods / sizeof g_mods[0]))
static DWORD g_watchdog_s;

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

recomp_func_t native32_w9x_lookup(uint32_t va);   /* vxd32.c: VWIN32's ring-0 entries */
recomp_func_t recomp_lookup_manual(uint32_t va) { return native32_w9x_lookup(va); }

/* The C callers from a thread context (dbghelp, with lines). */
static void dump_stack(HANDLE th, CONTEXT c, int n) {
    IMAGEHLP_LINE64 ln = { sizeof ln };
    DWORD disp = 0;
    STACKFRAME64 sf = { 0 };
    sf.AddrPC.Offset = c.Eip; sf.AddrFrame.Offset = c.Ebp; sf.AddrStack.Offset = c.Esp;
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < n && StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), th, &sf, &c,
                                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
        char sb[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO* si = (SYMBOL_INFO*)sb; DWORD64 d64 = 0;
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
        fprintf(stderr, "    #%d %s", i, SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &d64, si) ? si->Name : "?");
        if (SymGetLineFromAddr64(GetCurrentProcess(), sf.AddrPC.Offset, &disp, &ln)) fprintf(stderr, " %s:%lu", ln.FileName, ln.LineNumber);
        fputc('\n', stderr);
    }
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    HMODULE hm;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)r->ExceptionAddress, &hm) && hm != GetModuleHandleA(NULL)
        && !GetProcAddress(hm, "recomp_part_dispatch"))   /* a part DLL's lifted code is ours too */
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at %p (addr %08lX) thread %lu ===\n  in lifted sub_%08X, last native call %s\n",
            r->ExceptionCode, r->ExceptionAddress,
            r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0ul, GetCurrentThreadId(), g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    if (r->ExceptionCode == 0xC0000005u && !IsBadReadPtr(r->ExceptionAddress, 48)) {   /* guest bytes reached as code (data, a run-time stub): shown, never run */
        fprintf(stderr, "  bytes:");
        for (int i = 0; i < 48; i++) fprintf(stderr, " %02X", ((const uint8_t*)r->ExceptionAddress)[i]);
        fprintf(stderr, "\n");
    }
    {   /* the lifted C line: the register globals are stale for a body's locals */
        IMAGEHLP_LINE64 ln = { sizeof ln };
        DWORD disp = 0;
        SymSetOptions(SYMOPT_LOAD_LINES);
        if (SymInitialize(GetCurrentProcess(), NULL, TRUE) &&
            SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)(uintptr_t)r->ExceptionAddress, &disp, &ln))
            fprintf(stderr, "  at %s:%lu\n", ln.FileName, ln.LineNumber);
        /* the callers: which lifted 16-bit block the inlined accessor was in */
        dump_stack(GetCurrentThread(), *ep->ContextRecord, getenv("W98_DEPTH") ? atoi(getenv("W98_DEPTH")) : 8);
    }
    native32_dump_icalls(12);
    fprintf(stderr, "  9CE0=%08X 9CE8=%08X 9D10=%08X 9CEC=%08X\n", MEM32(0xBFFC9CE0u), MEM32(0xBFFC9CE8u), MEM32(0xBFFC9D10u), MEM32(0xBFFC9CECu));
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* W98_LDTW=sel: every write that changes sel's descriptor, with the writing
 * thread and its C stack (the LDT page is read-only; a write steps once). Debug only. */
static uint8_t* g_ldtw; static uint8_t* g_ldtw_pg; static uint32_t g_ldtw_last[2];
static __declspec(thread) int t_ldtw_step;
static LONG CALLBACK ldtw(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord; DWORD o;
    if (r->ExceptionCode == 0xC0000005u && r->ExceptionInformation[0] == 1
        && (uint8_t*)r->ExceptionInformation[1] - g_ldtw_pg < 0x1000u) {
        VirtualProtect(g_ldtw_pg, 0x1000, PAGE_READWRITE, &o);
        ep->ContextRecord->EFlags |= 0x100; t_ldtw_step = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == 0x80000004u && t_ldtw_step) {
        t_ldtw_step = 0;
        uint32_t* d = (uint32_t*)g_ldtw;
        if (d[0] != g_ldtw_last[0] || d[1] != g_ldtw_last[1]) {
            g_ldtw_last[0] = d[0]; g_ldtw_last[1] = d[1];
            fprintf(stderr, "LDTW %08X %08X thread %lu in sub_%08X\n", d[0], d[1], GetCurrentThreadId(), g_cur_func);
            SymSetOptions(SYMOPT_LOAD_LINES); static int si; if (!si) si = SymInitialize(GetCurrentProcess(), NULL, TRUE);
            dump_stack(GetCurrentThread(), *ep->ContextRecord, 10);
            fflush(stderr);
        }
        VirtualProtect(g_ldtw_pg, 0x1000, PAGE_READONLY, &o);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void k32host_ldtw_arm(void) {
    const char* e = getenv("W98_LDTW"); DWORD o;
    if (!e || g_ldtw) return;
    uint8_t* native32_w9x_desc(uint16_t sel);
    g_ldtw = native32_w9x_desc((uint16_t)strtoul(e, NULL, 16));
    g_ldtw_pg = (uint8_t*)((uintptr_t)g_ldtw & ~(uintptr_t)0xFFF);
    memcpy(g_ldtw_last, g_ldtw, 8);
    AddVectoredExceptionHandler(1, ldtw);
    VirtualProtect(g_ldtw_pg, 0x1000, PAGE_READONLY, &o);
}

static HANDLE g_main_th;
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s\n", g_watchdog_s, g_cur_func, g_cur_import);
    {   void native32_w9x_dump_threads(void);
        uint32_t w16m = MEM32(0xBFFC9470u);
        fprintf(stderr, "  Win16Mutex %08X owner %08X, crst 9D10 owner %08X\n", w16m, w16m ? MEM32(w16m + 8) : 0, MEM32(0xBFFC9D10u) ? MEM32(MEM32(0xBFFC9D10u) + 8) : 0);
        native32_w9x_dump_threads();
    }
    SymSetOptions(SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);   /* where every thread is (a deadlock has several) */
    THREADENTRY32 te = { sizeof te };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        CONTEXT c = { 0 };
        c.ContextFlags = CONTEXT_FULL;
        if (th && SuspendThread(th) != (DWORD)-1 && GetThreadContext(th, &c)) { fprintf(stderr, "  thread %lu:\n", te.th32ThreadID); dump_stack(th, c, 48); }
    }
    fflush(stderr);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* VWIN32's boot-info block for kernel32's boot entry. Known fields (read off
 * the entry): +08 a selector, +0C the VMM Win32 service gate (offset:16,
 * selector:16), +60 a byte flag. The rest is learnt as the boot reads it. */
static uint8_t g_boot[0x200];
static char g_tail[126];   /* the guest's command tail: what follows "--" */
uint8_t* g_krnl_dbg;

/* A host DLL the guest imports (xcopy32: USER32) is found by kernel32's own
 * loader, so the sandbox gets a stand-in of the host's making: a PE with
 * only an export table, each name's RVA pointing at the host's function of
 * that name (base + RVA wraps to it), the same addresses native32_bind put
 * in the IAT. `extra` adds names answered elsewhere (vxd32's stand-ins).
 * No code, no entry point, nothing of Microsoft's. */
static int write_proxy_dll(const char* path, const char* dll, uint32_t base, const char* const* names, int n,
                           const char* const* xnames, const uint32_t* xva, int nx) {
    enum { HDR = 0x200, SEC = 0x1000 };
    static uint8_t img[HDR + 0x8000];
    const char* all[512];
    uint32_t va[512];
    int m = 0;
    HMODULE h = LoadLibraryA(dll);
    for (int i = 0; i < n && m < 512; i++) {
        int x = 0;
        while (x < nx && strcmp(xnames[x], names[i])) x++;
        if (x < nx) continue;                /* a stand-in replaces it below */
        FARPROC f = h ? GetProcAddress(h, names[i]) : NULL;
        if (f) { all[m] = names[i]; va[m++] = (uint32_t)(uintptr_t)f; }
    }
    for (int i = 0; i < nx && m < 512; i++) { all[m] = xnames[i]; va[m++] = xva[i]; }
    for (int i = 1; i < m; i++)              /* the loader binary-searches the names */
        for (int j = i; j > 0 && strcmp(all[j - 1], all[j]) > 0; j--) {
            const char* t = all[j]; all[j] = all[j - 1]; all[j - 1] = t;
            uint32_t v = va[j]; va[j] = va[j - 1]; va[j - 1] = v;
        }
    memset(img, 0, sizeof img);
    uint8_t* e = img + HDR;                  /* the section: export directory, then tables, then strings */
    uint32_t funcs = 40, nms = funcs + 4 * m, ords = nms + 4 * m, str = ords + 2 * m;
    uint32_t p = str;
    *(uint32_t*)(e + 12) = SEC + p; strcpy((char*)e + p, dll); p += (uint32_t)strlen(dll) + 1;
    *(uint32_t*)(e + 16) = 1;
    *(uint32_t*)(e + 20) = m; *(uint32_t*)(e + 24) = m;
    *(uint32_t*)(e + 28) = SEC + funcs; *(uint32_t*)(e + 32) = SEC + nms; *(uint32_t*)(e + 36) = SEC + ords;
    for (int i = 0; i < m; i++) {
        *(uint32_t*)(e + funcs + 4 * i) = va[i] - base;
        *(uint32_t*)(e + nms + 4 * i) = SEC + p;
        *(uint16_t*)(e + ords + 2 * i) = (uint16_t)i;
        strcpy((char*)e + p, all[i]); p += (uint32_t)strlen(all[i]) + 1;
        if (p > 0x7F00) return 0;
    }
    uint32_t sz = (p + 0x1FF) & ~0x1FFu;
    img[0] = 'M'; img[1] = 'Z'; *(uint32_t*)(img + 0x3C) = 0x40;
    uint8_t* pe = img + 0x40;
    memcpy(pe, "PE\0\0", 4);
    *(uint16_t*)(pe + 4) = 0x14C; *(uint16_t*)(pe + 6) = 1; *(uint16_t*)(pe + 20) = 0xE0; *(uint16_t*)(pe + 22) = 0x2102;
    uint8_t* o = pe + 24;
    *(uint16_t*)(o + 0) = 0x10B; *(uint32_t*)(o + 28) = base;
    *(uint32_t*)(o + 32) = 0x1000; *(uint32_t*)(o + 36) = 0x200;
    *(uint16_t*)(o + 40) = 4; *(uint16_t*)(o + 48) = 4;             /* OS and subsystem 4.0 */
    *(uint32_t*)(o + 56) = SEC + ((sz + 0xFFF) & ~0xFFFu); *(uint32_t*)(o + 60) = HDR;
    *(uint16_t*)(o + 68) = 2;                                       /* GUI */
    *(uint32_t*)(o + 72) = 0x100000; *(uint32_t*)(o + 76) = 0x1000; *(uint32_t*)(o + 80) = 0x100000; *(uint32_t*)(o + 84) = 0x1000;
    *(uint32_t*)(o + 92) = 16;
    *(uint32_t*)(o + 96) = SEC; *(uint32_t*)(o + 100) = p;          /* the export directory */
    uint8_t* s = o + 0xE0;
    memcpy(s, ".edata", 6);
    *(uint32_t*)(s + 8) = p; *(uint32_t*)(s + 12) = SEC; *(uint32_t*)(s + 16) = sz; *(uint32_t*)(s + 20) = HDR;
    *(uint32_t*)(s + 36) = 0x40000040;                              /* initialised data, readable */
    /* the same bytes already there: leave the file alone (another guest process,
     * e.g. w16test next to check_m1's xcopy, may have it mapped; "wb" would fail) */
    FILE* f = fopen(path, "rb");
    if (f) {
        static uint8_t old[sizeof img];
        size_t got = fread(old, 1, sizeof old, f);
        fclose(f);
        if (got == HDR + sz && !memcmp(old, img, got)) return m;
    }
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(img, 1, HDR + sz, f);
    fclose(f);
    return m;
}

/* The names a module imports from one DLL, from its import table. */
static int imports_from(uint32_t base, const char* dll, const char** out, int max) {
    uint32_t pe = base + MEM32(base + 0x3C), imp = MEM32(pe + 0x80), n = 0;
    if (!imp) return 0;
    for (uint32_t d = base + imp; MEM32(d + 12); d += 20) {
        if (_stricmp((const char*)(uintptr_t)(base + MEM32(d + 12)), dll)) continue;
        for (uint32_t t = base + (MEM32(d) ? MEM32(d) : MEM32(d + 16)); MEM32(t) && (int)n < max; t += 4)
            if (!(MEM32(t) & 0x80000000u)) out[n++] = (const char*)(uintptr_t)(base + MEM32(t) + 2);
    }
    return (int)n;
}

static char g_root_host[MAX_PATH];

static int host_main(int argc, char** argv) {
    const char* sys = "work\\sys", *root = "work\\root";
    char dir[MAX_PATH];
    int k;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mute")) SetEnvironmentVariableA("PCRECOMP_MUTE", "1");
        else if (!strcmp(argv[i], "--sys") && i + 1 < argc) sys = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--")) {
            while (++i < argc) { strncat(g_tail, " ", 124 - strlen(g_tail)); strncat(g_tail, argv[i], 124 - strlen(g_tail)); }
            break;
        }
        else if ((k = recomp_trace_arg(argc, argv, i)) > 0) i += k - 1;   /* --calltrace FILE ... */
        else { printf("usage: %s [--mute] [--sys DIR] [--watchdog S] [-- args]\n", APP_EXE); return 1; }
    }
    GetFullPathNameA(sys, MAX_PATH, dir, NULL);
    {   /* The guest's C:\ (vxd32 boxes every path under it): a Windows 98
         * layout with the two modules this program is made of. Built from
         * your work/sys on first run; nothing outside it is touched. */
        static const char* const dirs[] = { "", "\\WINDOWS", "\\WINDOWS\\SYSTEM", "\\WINDOWS\\COMMAND", "\\WINDOWS\\TEMP",
            "\\My Documents", "\\Program Files" };   /* what Win98 setup makes at C:\ (and nothing host-shaped: survey s39) */
        char r[MAX_PATH], p[MAX_PATH], src[MAX_PATH];
        GetFullPathNameA(root, MAX_PATH, r, NULL);
        SetEnvironmentVariableA("NATIVE32_W9X_ROOT", r);
        strcpy(g_root_host, r);
        for (int i = 0; i < (int)(sizeof dirs / sizeof *dirs); i++) { _snprintf(p, MAX_PATH, "%s%s", r, dirs[i]); p[MAX_PATH - 1] = 0; CreateDirectoryA(p, NULL); }
        _snprintf(src, MAX_PATH, "%s\\kernel32.dll", dir); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\KERNEL32.DLL", r);
        CopyFileA(src, p, TRUE);
        _snprintf(src, MAX_PATH, "%s\\%s", dir, APP_EXE); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\COMMAND\\%s", r, APP_EXE);
        CopyFileA(src, p, TRUE);
#ifdef W16_IMAGE
        {   /* the 16-bit modules, which KRNL386 opens by path (its own EXE first) */
            /* and the 32-bit halves GDI.EXE/USER.EXE LoadLibrary32 (lifted into this host;
             * MPR.DLL: USER's DllEntryPoint ThunkConnect16s USER.EXE <-> MPR.DLL) */
            static const char* const m16[] = { "KRNL386.EXE", "GDI.EXE", "USER.EXE", "GDI32.DLL", "ADVAPI32.DLL", "DDEML.DLL", "MPR.DLL", "MSGSRV32.EXE",
                /* MPR's WNetInitialize CreateProcesses MPREXE.EXE and waits for it to hand
                 * MPR its object (MPR ordinal 7); MPREXE loads MPRSERV, MPRSERV MSPWL32 */
                "MPREXE.EXE", "MPRSERV.DLL", "MSPWL32.DLL",
                /* the shell (SYSTEM.INI shell=calc.exe, MSGSRV32 WinExecs it) and its DLLs */
                "MSVCRT.DLL", "SHELL32.DLL", "SHLWAPI.DLL", "COMCTL32.DLL", "COMDLG32.DLL",   /* + notepad's */
                "SHDOCVW.DLL", "BROWSEUI.DLL",   /* + Explorer's integrated shell (ieshell.inf) */
                "SHDOC401.DLL",   /* the desktop: SHDOCVW 119 SHCreateDesktop goes to its 200 while SHELL32 is below 5.0 */
                "..\\rebased\\OLE32.DLL" };   /* rebased to 0x7F400000: its own base runs into 0x7FFE0000 (the host's shared user page) */
            for (int i = 0; i < (int)(sizeof m16 / sizeof *m16); i++) {
                const char* bn = strrchr(m16[i], '\\') ? strrchr(m16[i], '\\') + 1 : m16[i];
                _snprintf(src, MAX_PATH, "%s\\%s", dir, m16[i]); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\%s", r, bn);
                CopyFileA(src, p, bn == m16[i]);   /* a rebased one replaces an older copy */
            }
            /* Win98's own USER32.DLL (lifted into this host): USER.EXE's LibEntry LoadLibrary32s it
             * and calls InitSharedTable/InitUsrMprThunks. Over the proxy a plain xcopy run leaves. */
            _snprintf(src, MAX_PATH, "%s\\USER32.DLL", dir); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\USER32.DLL", r);
            if (getenv("W16_LIBMAIN")) CopyFileA(src, p, FALSE);
            /* COMCTL32's ThunkConnect32 partner (Cctl1632_ThunkData16): setup installs
             * KOMMCTRL.DLL as COMMCTRL.DLL (subase.inf); the CABs' commctrl.dll is setup's own */
            _snprintf(src, MAX_PATH, "%s\\kommctrl.dll", dir); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\COMMCTRL.DLL", r);
            CopyFileA(src, p, FALSE);
            /* and SHELL.NEW as SHELL.DLL (shell.inf), SHELL32's partner (Shl3216_ThunkData16) */
            _snprintf(src, MAX_PATH, "%s\\shell.new", dir); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\SHELL.DLL", r);
            CopyFileA(src, p, FALSE);
            {   /* the accessories a W16_SHELL may name, under their installed names (accessor.inf, games.inf);
                 * notepad as rebased off 0x400000 (xcopy32.mod, this host's own EXE, is there):
                 * tools/rebase.py work/sys work/rebased notepad.exe=0x01100000, then renamed
                 * notepad_1100.exe so its catalog is not the 0x400000 one) */
                static const char *const prog[][2] = { { "calc.exe", "WINDOWS\\CALC.EXE" }, { "..\\rebased\\notepad_1100.exe", "WINDOWS\\NOTEPAD.EXE" },
                    { "..\\rebased\\explorer_1400.exe", "WINDOWS\\EXPLORER.EXE" },   /* rebased as notepad: explorer.exe=0x01400000 */
                    { "winmine.exe", "WINDOWS\\WINMINE.EXE" }, { "sol.exe", "WINDOWS\\SOL.EXE" }, { "cards.dll", "WINDOWS\\SYSTEM\\CARDS.DLL" } };
                for (int i = 0; i < (int)(sizeof prog / sizeof *prog); i++) {
                    _snprintf(src, MAX_PATH, "%s\\%s", dir, prog[i][0]); _snprintf(p, MAX_PATH, "%s\\%s", r, prog[i][1]);
                    CopyFileA(src, p, FALSE);
                }
            }
            {   /* the raster fonts GDI.EXE's init opens (vgasys/vgafix/vgaoem.fon, by search path) */
                WIN32_FIND_DATAA fd;
                HANDLE f;
                _snprintf(p, MAX_PATH, "%s\\WINDOWS\\FONTS", r); CreateDirectoryA(p, NULL);
                _snprintf(src, MAX_PATH, "%s\\*.fon", dir);
                if ((f = FindFirstFileA(src, &fd)) != INVALID_HANDLE_VALUE) {
                    do {
                        _snprintf(src, MAX_PATH, "%s\\%s", dir, fd.cFileName);
                        _snprintf(p, MAX_PATH, "%s\\WINDOWS\\FONTS\\%s", r, fd.cFileName);
                        CopyFileA(src, p, TRUE);
                    } while (FindNextFileA(f, &fd));
                    FindClose(f);
                }
                /* and Marlett, which the display stand-in rasterizes for USER's caption glyphs; not
                 * as FONTS\*.TTF, where GDI's own TrueType side finds it (and calls VMM 0001:0009) */
                _snprintf(src, MAX_PATH, "%s\\marlett.ttf", dir); _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\MARLETT.TT_", r);
                CopyFileA(src, p, TRUE);
            }
        }
#endif
        {   /* the NLS tables kernel32 maps at boot (unicode.nls, c_<cp>.nls, ...) */
            WIN32_FIND_DATAA fd;
            HANDLE f;
            _snprintf(src, MAX_PATH, "%s\\*.nls", dir);
            if ((f = FindFirstFileA(src, &fd)) != INVALID_HANDLE_VALUE) {
                do {
                    _snprintf(src, MAX_PATH, "%s\\%s", dir, fd.cFileName);
                    _snprintf(p, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\%s", r, fd.cFileName);
                    CopyFileA(src, p, TRUE);
                } while (FindNextFileA(f, &fd));
                FindClose(f);
            }
        }
    }
#ifdef W16_IMAGE
    {   /* The lifted EXEs kernel32's own loader maps later (CreateProcess): their
         * bases are held from the host's start (native32_hold_image's relaunch),
         * or a host heap lands there first and the image is relocated where no
         * lifted code is (vxd32's PageReserve takes a held base as the image's). */
        static const char* const later[] = { "mprexe.exe", "calc.exe", "..\\rebased\\notepad_1100.exe", "..\\rebased\\explorer_1400.exe" };
        for (int i = 0; i < (int)(sizeof later / sizeof *later); i++) {
            char img[MAX_PATH];
            IMAGE_DOS_HEADER dh;
            IMAGE_NT_HEADERS32 nh;
            _snprintf(img, sizeof img - 1, "%s\\%s", dir, later[i]);
            img[MAX_PATH - 1] = 0;
            FILE* f = fopen(img, "rb");
            if (f && fread(&dh, sizeof dh, 1, f) == 1 && !fseek(f, dh.e_lfanew, SEEK_SET) && fread(&nh, sizeof nh, 1, f) == 1)
                native32_hold_range(nh.OptionalHeader.ImageBase, nh.OptionalHeader.SizeOfImage);
            if (f) fclose(f);
        }
    }
#endif
    for (int i = 0; i < 1; i++) {   /* the EXE only: hold_image relaunches once per call; 0xBFF70000 is free anyway */
        char img[MAX_PATH];
        _snprintf(img, sizeof img - 1, "%s\\%s", dir, g_mods[i].name);
        img[MAX_PATH - 1] = 0;
#ifdef W16_IMAGE
        native32_hold_range(0x10000, 0x100000);   /* Win16 identity: real-mode memory at paragraph * 16 */
#endif
        native32_hold_image(img, g_mods[i].base);
    }
#ifdef W16_IMAGE
    { uint32_t w16_real_base(void); w16_real_base(); }
#endif
    {   /* the guest never writes the user's registry (pcrecomp regbox32.c) */
        if (!native32_registry_sandbox("w98\\" APP_NAME)) { fprintf(stderr, "no registry sandbox: not running\n"); return 1; }
    }
    native32_init();
    AddVectoredExceptionHandler(0, crash);
    k32host_ldtw_arm();
    if (!native32_modules_map(dir, dir, g_mods, NMODS)) return 1;
    int missing = native32_modules_bind(NULL, 0);
    if (missing) fprintf(stderr, "warning: %d imports unresolved\n", missing);
    if (g_watchdog_s) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_th, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    }

    uint32_t* kexp = (uint32_t*)(uintptr_t)(K32_BASE + 0x1000);
    if (kexp[0] != 0x5058454Bu) { fprintf(stderr, "kernel32: no KEXP header\n"); return 1; }
    /* Every dword is a pointer to a VWIN32-side object kernel32 fills or
     * reads, except the scalars named below: each pointer gets its own
     * zeroed page. ponytail: one page each, size them when a field is known. */
    for (int off = 4; off < 0x9C; off += 4)
        *(uint32_t*)(g_boot + off) = (uint32_t)(uintptr_t)VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    *(uint32_t*)(g_boot + 0x00) = 0x9C;          /* the block's size: the boot checks it */
    uint16_t alias = 0x0030;
#ifdef W16_IMAGE
    /* before kernel32 takes its own (its TIB's fs); the alias goes past them
     * too (0030 is segment 6's selector, 0037, in Win16 identity) */
    { uint16_t w16_reserve_sels(void); alias = w16_reserve_sels(); }
#endif
    *(uint32_t*)(g_boot + 0x10) = alias;         /* the LDT alias (kernel32 writes descriptors through it) */
    { uint32_t ldt; native32_w9x_ldt(alias, &ldt); *(uint32_t*)(g_boot + 0x14) = ldt; }   /* +14: the LDT's flat address (MapSL reads it) */
    *(uint32_t*)(g_boot + 0x18) = 0x0040;
    {   /* +20/+24: the Windows and system directories (flat strings), +28/+2A
         * their lengths (_INIT 0xBFFC6CA4 -> 0xBFFC9588/9584, 9590/958C) */
        static char windir[0x104] = "C:\\WINDOWS", sysdir[0x104] = "C:\\WINDOWS\\SYSTEM";   /* written in place */
        *(uint32_t*)(g_boot + 0x20) = (uint32_t)(uintptr_t)windir;
        *(uint32_t*)(g_boot + 0x24) = (uint32_t)(uintptr_t)sysdir;
        *(uint16_t*)(g_boot + 0x28) = (uint16_t)strlen(windir);
        *(uint16_t*)(g_boot + 0x2A) = (uint16_t)strlen(sysdir);
    }
    *(uint32_t*)(g_boot + 0x60) = 0;             /* a byte flag */
    *(uint32_t*)(g_boot + 0x78) = 0x00580040;    /* +78: a size in KB (64), +7A: a selector */
    *(uint32_t*)(g_boot + 0x0C) = 0x0028FFF0u;   /* a gate far pointer; vxd32 answers every call through it */
    /* kernel32's 32->16 thunks into KRNL386 all go through one slot
     * (`call [0xBFF9AD46]` in the stubs' common tail, normally a stub that
     * patches itself into QT_Thunk): vxd32 answers them by thunk index. */
    {   /* W16_LIBMAIN=KERNEL: KRNL386 is real, so the slot stays kernel32's own */
        const char* lm = getenv("W16_LIBMAIN");
        if (!lm || _stricmp(lm, "KERNEL")) *(uint32_t*)(uintptr_t)0xBFF9AD46u = native32_w9x_krnl386_thunk();
    }
    /* USER32!UserSignalProc, which kernel32 calls unchecked (vxd32) */
    *(uint32_t*)(uintptr_t)0xBFFC95C0u = native32_w9x_user_signal_proc();
    /* VWIN32 stores its ring-0 entry table where the KEXP header's second dword points */
    *(uint32_t*)(uintptr_t)kexp[1] = native32_vwin32_r0_table();
    {   /* boot block +3C: KRNL386's half of the handshake, a 16:16 pointer to a
         * 54h-byte block KRNL386 builds at run time. Its 16:16 entry points
         * (+14..+28, +3C..+44, +50) are tagged FEEE:<field> so a call down to
         * KRNL386 names itself; +10, +48, +4C are 16:16 data blocks. */
        static uint8_t krnl[0x54];
        extern uint8_t* g_krnl_dbg; g_krnl_dbg = krnl;
        uint32_t* k = (uint32_t*)krnl;
        uint16_t ksel = native32_w9x_alloc_sel((uint32_t)(uintptr_t)krnl, sizeof krnl - 1);
        static const int ent[] = { 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x3C, 0x40, 0x44, 0x50 };
        static const int dat[] = { 0x10, 0x48, 0x4C };
        k[0] = 0x54;
        k[0x30 / 4] = 0x0035AA69;
        k[0x34 / 4] = *(uint32_t*)(uintptr_t)0xBFFC9EC8u;   /* kernel32's own id: the boot compares it */
        for (int i = 0; i < 10; i++) k[ent[i] / 4] = 0xFEEE0000u | ent[i];
        for (int i = 0; i < 3; i++) {
            void* blk = VirtualAlloc(NULL, 0x10000, MEM_COMMIT, PAGE_READWRITE);
            k[dat[i] / 4] = (uint32_t)native32_w9x_alloc_sel((uint32_t)(uintptr_t)blk, 0xFFFF) << 16;
        }
        *(uint32_t*)(g_boot + 0x3C) = (uint32_t)ksel << 16;
        {   /* +08: the PSP selector. kernel32 builds the first process's
             * environment and command line from PSP:2C (the environment's
             * selector: "VAR=val\0...\0", a word 1, the program's path) and
             * the command tail at PSP:80. */
            /* A Windows 98 environment, not the host's: the guest's C:\ is
             * the sandbox root, and host paths mean nothing there. */
            static const char* const w98_env[] = {
                "TMP=C:\\WINDOWS\\TEMP", "TEMP=C:\\WINDOWS\\TEMP", "PROMPT=$p$g", "winbootdir=C:\\WINDOWS",
                "COMSPEC=C:\\WINDOWS\\COMMAND.COM", "PATH=C:\\WINDOWS;C:\\WINDOWS\\COMMAND", "windir=C:\\WINDOWS", NULL };
            static uint8_t psp[0x100], env[0x1000];
            char* e = (char*)env;
            for (int v = 0; w98_env[v]; v++) { strcpy(e, w98_env[v]); e += strlen(w98_env[v]) + 1; }
            *e++ = 0;
            *e++ = 1; *e++ = 0;
            strcpy(e, "C:\\WINDOWS\\COMMAND\\" APP_EXE);
            psp[0] = 0xCD; psp[1] = 0x20;
            *(uint16_t*)(psp + 0x2C) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)env, sizeof env - 1);
            psp[0x80] = (uint8_t)strlen(g_tail);
            memcpy(psp + 0x81, g_tail, psp[0x80]);
            psp[0x81 + psp[0x80]] = 0x0D;
            *(uint32_t*)(g_boot + 0x08) = g_w9x_psp_sel = native32_w9x_alloc_sel((uint32_t)(uintptr_t)psp, sizeof psp - 1);
        }
        /* +44, +54, +58: 16:16 pointers; +50: a selector, KRNL386's global
         * arena (MapSL bumps an arena entry's lock count there). +54 -> the
         * offset of the selector table in the arena, +58 -> its limit. */
        static const int far_ptrs[] = { 0x44, 0x54, 0x58 };
        for (int i = 0; i < 3; i++) {
            void* blk = VirtualAlloc(NULL, 0x10000, MEM_COMMIT, PAGE_READWRITE);
            *(uint32_t*)(g_boot + far_ptrs[i]) = (uint32_t)native32_w9x_alloc_sel((uint32_t)(uintptr_t)blk, 0xFFFF) << 16;
        }
        void* arena = VirtualAlloc(NULL, 0x10000, MEM_COMMIT, PAGE_READWRITE);
        *(uint32_t*)(g_boot + 0x50) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)arena, 0xFFFF);
        {   /* +1C -> a word: the boot task's Win16 TDB selector (_INIT's 5th
             * argument; it lands in PDB+38). kernel32 MapSLs it and reads
             * +16 flags (10h = Win32 task), +66 the drive (bit 7: the
             * directory at +100 is valid), +100 the current directory, and
             * 'TD' sits at +FA. The host's task is on C:\. */
            static uint8_t tdb[0x200];
            tdb[0x66] = 0x80 | 2;
            tdb[0xFA] = 'T'; tdb[0xFB] = 'D';
            strcpy((char*)tdb + 0x100, "\\");
            **(uint16_t**)(g_boot + 0x1C) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)tdb, sizeof tdb - 1);
        }
        /* +7A: a selector kernel32 reads gs:[0] through (a VWIN32 dword) */
        *(uint16_t*)(g_boot + 0x7A) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE), 0xFFF);
    }
    uint32_t bp = (uint32_t)(uintptr_t)g_boot;
    printf("%s + kernel32: %u lifted functions, booting kernel32 at 0x%08X\n", APP_EXE, recomp_dispatch_count, kexp[3]);
    fflush(stdout);
#ifdef W16_IMAGE
    /* W16_LIBMAIN=KERNEL: KRNL386's own BootStrap builds the boot block and
     * calls this entry (KEXP 3, seg001_6E6x), as on Windows 98; the host's
     * block above then only fed the stand-in path. */
    { const char* lm = getenv("W16_LIBMAIN"); if (lm && !_stricmp(lm, "KERNEL")) { int w16_test(void); return w16_test(); } }
#endif
    native32_call_guest(kexp[3], 1, &bp);
    recomp_trace_flush();
    fprintf(stderr, "[boot] kernel32 boot returned %08X; KRNL386 block +4 = %08X; Win16Mutex %08X (owner %08X), crst 9D10 owner %08X\n", g_eax, *(uint32_t*)(g_krnl_dbg + 4), MEM32(0xBFFC9470u), MEM32(MEM32(0xBFFC9470u) + 8), MEM32(MEM32(0xBFFC9D10u) + 8));
    {   /* Start the program the way Windows 98 does: kernel32's own
         * CreateProcessA, from the boot thread, then wait for it. kernel32's
         * loader maps the image over the copy native32 already holds (the
         * same bytes, at the same base); its main thread runs on a host
         * thread (vxd32). */
        /* The boot comes back owning the Win16Mutex: on Windows 98 the boot
         * thread goes on as KRNL386's task, which gives it up when it
         * yields. The host is that task: let it go (_LeaveSysLevel,
         * kernel32 ordinal 98, 0xBFF741ED), or every Win32 thread that
         * takes it waits for ever. */
        {   /* USER32: the host's, behind a proxy kernel32 can load */
            const char* nm[256];
            int n = imports_from(APP_BASE, "USER32.DLL", nm, 256);
            /* MessageBoxA: kernel32's hard errors, answered in vxd32 (logged, never shown) */
            static const char* const xn[] = { "UserSignalProc", "MessageBoxA" };
            uint32_t xv[] = { native32_w9x_user_signal_proc(), native32_w9x_message_box() };
            char path[MAX_PATH];
            _snprintf(path, MAX_PATH, "%s\\WINDOWS\\SYSTEM\\USER32.DLL", g_root_host);
            if (n && !write_proxy_dll(path, "user32.dll", 0x30000000u, nm, n, xn, xv, 2)) { fprintf(stderr, "no USER32 proxy\n"); return 1; }
        }
#ifdef W16_IMAGE
        { int w16_test(void); return w16_test(); }   /* milestone 2, chunk 1 (src/w16test.c) */
#endif
        uint32_t w16 = MEM32(0xBFFC9470u);
        native32_call_guest(0xBFF741EDu, 1, &w16);
        static char cmd[300];
        static uint32_t si[0x44 / 4], pi[4], code;
        uint32_t cpa = native32_export(K32_BASE, "CreateProcessA");
        uint32_t wfso = native32_export(K32_BASE, "WaitForSingleObject");
        uint32_t gecp = native32_export(K32_BASE, "GetExitCodeProcess");
        _snprintf(cmd, sizeof cmd - 1, "C:\\WINDOWS\\COMMAND\\%s%s", APP_EXE, g_tail);
        si[0] = 0x44;
        uint32_t a[10] = { 0, (uint32_t)(uintptr_t)cmd, 0, 0, 1, 0, 0, 0, (uint32_t)(uintptr_t)si, (uint32_t)(uintptr_t)pi };
        native32_call_guest(cpa, 10, a);
        fprintf(stderr, "[boot] CreateProcessA(\"%s\") = %u, process %08X thread %08X\n", cmd, g_eax, pi[0], pi[1]);
        if (!g_eax) return 1;
        uint32_t w[2] = { pi[0], 0xFFFFFFFFu };
        native32_call_guest(wfso, 2, w);
        uint32_t c[2] = { pi[0], (uint32_t)(uintptr_t)&code };
        native32_call_guest(gecp, 2, c);
        fprintf(stderr, "[boot] %s exited with %u\n", APP_EXE, code);
        return (int)code;
    }
}

static int g_argc;
static char** g_argv;
static DWORD WINAPI host_thread(LPVOID unused) { (void)unused; return (DWORD)host_main(g_argc, g_argv); }

int main(int argc, char** argv) {
    DWORD code = 1;
    /* no loader or hard-error box may reach the user's desktop */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    g_argc = argc;
    g_argv = argv;
    HANDLE t = CreateThread(NULL, 16u << 20, host_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    GetExitCodeThread(t, &code);
    return (int)code;
}
