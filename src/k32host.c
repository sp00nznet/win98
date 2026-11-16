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

#include "native32.h"
extern const char* g_cur_import;
int recomp_trace_arg(int argc, char** argv, int i);
void recomp_trace_flush(void);
uint32_t native32_vwin32_r0_table(void);   /* vxd32.c */
void native32_w9x_ldt(uint16_t alias_sel, uint32_t* ldt_base);
uint16_t native32_w9x_alloc_sel(uint32_t base, uint32_t limit);

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

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    HMODULE hm;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)r->ExceptionAddress, &hm) && hm != GetModuleHandleA(NULL))
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at %p (addr %08lX) ===\n  in lifted sub_%08X, last native call %s\n",
            r->ExceptionCode, r->ExceptionAddress,
            r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0ul, g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s\n", g_watchdog_s, g_cur_func, g_cur_import);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* VWIN32's boot-info block for kernel32's boot entry. Known fields (read off
 * the entry): +08 a selector, +0C the VMM Win32 service gate (offset:16,
 * selector:16), +60 a byte flag. The rest is learnt as the boot reads it. */
static uint8_t g_boot[0x200];
static char g_tail[126];   /* the guest's command tail: what follows "--" */
uint8_t* g_krnl_dbg;

static int host_main(int argc, char** argv) {
    const char* sys = "work\\sys";
    char dir[MAX_PATH];
    int k;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mute")) SetEnvironmentVariableA("PCRECOMP_MUTE", "1");
        else if (!strcmp(argv[i], "--sys") && i + 1 < argc) sys = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--")) {
            while (++i < argc) { strncat(g_tail, " ", 124 - strlen(g_tail)); strncat(g_tail, argv[i], 124 - strlen(g_tail)); }
            break;
        }
        else if ((k = recomp_trace_arg(argc, argv, i)) > 0) i += k - 1;   /* --calltrace FILE ... */
        else { printf("usage: %s [--mute] [--sys DIR] [--watchdog S] [-- args]\n", APP_EXE); return 1; }
    }
    GetFullPathNameA(sys, MAX_PATH, dir, NULL);
    for (int i = 0; i < 1; i++) {   /* the EXE only: hold_image relaunches once per call; 0xBFF70000 is free anyway */
        char img[MAX_PATH];
        _snprintf(img, sizeof img - 1, "%s\\%s", dir, g_mods[i].name);
        img[MAX_PATH - 1] = 0;
        native32_hold_image(img, g_mods[i].base);
    }
    {   /* the guest never writes the user's registry (pcrecomp regbox32.c) */
        if (!native32_registry_sandbox("w98\\" APP_NAME)) { fprintf(stderr, "no registry sandbox: not running\n"); return 1; }
    }
    native32_init();
    AddVectoredExceptionHandler(0, crash);
    if (!native32_modules_map(dir, dir, g_mods, NMODS)) return 1;
    int missing = native32_modules_bind(NULL, 0);
    if (missing) fprintf(stderr, "warning: %d imports unresolved\n", missing);
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));

    uint32_t* kexp = (uint32_t*)(uintptr_t)(K32_BASE + 0x1000);
    if (kexp[0] != 0x5058454Bu) { fprintf(stderr, "kernel32: no KEXP header\n"); return 1; }
    /* Every dword is a pointer to a VWIN32-side object kernel32 fills or
     * reads, except the scalars named below: each pointer gets its own
     * zeroed page. ponytail: one page each, size them when a field is known. */
    for (int off = 4; off < 0x9C; off += 4)
        *(uint32_t*)(g_boot + off) = (uint32_t)(uintptr_t)VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    *(uint32_t*)(g_boot + 0x00) = 0x9C;          /* the block's size: the boot checks it */
    *(uint32_t*)(g_boot + 0x10) = 0x0030;        /* the LDT alias (kernel32 writes descriptors through it) */
    { uint32_t ldt; native32_w9x_ldt(0x0030, &ldt); *(uint32_t*)(g_boot + 0x14) = ldt; }   /* +14: the LDT's flat address (MapSL reads it) */
    *(uint32_t*)(g_boot + 0x18) = 0x0040;
    *(uint32_t*)(g_boot + 0x28) = 0x00480050;    /* +28, +2A: words */
    *(uint32_t*)(g_boot + 0x60) = 0;             /* a byte flag */
    *(uint32_t*)(g_boot + 0x78) = 0x00580040;    /* +78: a size in KB (64), +7A: a selector */
    *(uint32_t*)(g_boot + 0x0C) = 0x0028FFF0u;   /* a gate far pointer; vxd32 answers every call through it */
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
            static uint8_t psp[0x100], env[0x8000];
            char* e = (char*)env;
            LPCH host_env = GetEnvironmentStringsA();
            for (const char* v = host_env; *v && e - (char*)env < 0x7000; v += strlen(v) + 1)
                if (*v != '=') { strcpy(e, v); e += strlen(v) + 1; }
            FreeEnvironmentStringsA(host_env);
            *e++ = 0;
            *e++ = 1; *e++ = 0;
            _snprintf(e, 0x7F00 - (e - (char*)env), "%s\\%s", dir, APP_EXE);
            psp[0] = 0xCD; psp[1] = 0x20;
            *(uint16_t*)(psp + 0x2C) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)env, sizeof env - 1);
            psp[0x80] = (uint8_t)strlen(g_tail);
            memcpy(psp + 0x81, g_tail, psp[0x80]);
            psp[0x81 + psp[0x80]] = 0x0D;
            *(uint32_t*)(g_boot + 0x08) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)psp, sizeof psp - 1);
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
        /* +7A: a selector kernel32 reads gs:[0] through (a VWIN32 dword) */
        *(uint16_t*)(g_boot + 0x7A) = native32_w9x_alloc_sel((uint32_t)(uintptr_t)VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE), 0xFFF);
    }
    uint32_t bp = (uint32_t)(uintptr_t)g_boot;
    printf("%s + kernel32: %u lifted functions, booting kernel32 at 0x%08X\n", APP_EXE, recomp_dispatch_count, kexp[3]);
    fflush(stdout);
    native32_call_guest(kexp[3], 1, &bp);
    recomp_trace_flush();
    fprintf(stderr, "[boot] kernel32 boot returned %08X; KRNL386 block +4 = %08X\n", g_eax, *(uint32_t*)(g_krnl_dbg + 4));
    return 0;
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
