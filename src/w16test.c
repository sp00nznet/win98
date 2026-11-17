/* w16test.c - milestone 2, chunk 1: lifted 32-bit kernel32 calls lifted
 * 16-bit USER.EXE through its own QT_Thunk, in one address space.
 *
 * After kernel32's boot (k32host.c), the host maps the image ne_link wrote
 * for KRNL386 + GDI + USER (work/w16, NE_IDENTITY_SEL=1) at a flat address,
 * gives segment n the LDT selector (n << 3) | 7, and points native32's
 * 16-bit bridge at runtime/win16/identity.c. Then it calls QT_Thunk
 * (0xBFF727D9) the way a user32 thunk does: a frame with the thunk index at
 * [ebp-4], the 16-bit arguments pushed as words, edx = the 16:16 target.
 *
 * Win16 identity mode: cpu.h RECOMP_W16_IDENTITY. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "cpu.h"
#include "mem_layout.h"

void native32_w9x_set_sel(uint16_t sel, uint32_t base, uint32_t limit, int code);
void native32_call_guest(uint32_t va, int nargs, const uint32_t* args);
extern int (*native32_w16_enter)(uint32_t target);
int w16_enter(uint32_t target);
extern uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
extern uint16_t g_seg_ds;
extern uint32_t g_fs_base;
uint16_t native32_w9x_alloc_sel(uint32_t base, uint32_t limit);
uint8_t *native32_w9x_desc(uint16_t sel);

#define QT_THUNK 0xBFF727D9u
#define MEM32(a) (*(uint32_t*)(uintptr_t)(a))
#define MEM16(a) (*(uint16_t*)(uintptr_t)(a))
#define SEL(n) ((uint16_t)(((n) << 3) | 7))

static uint8_t* g_img;

/* Win16 identity gives segment n the fixed selector SEL(n): stamp them
 * all before kernel32 boots, so the host allocator never hands one of
 * them out (kernel32's boot thread got 00FF, segment 31's, as its TIB). */
uint16_t w16_reserve_sels(void) {
    for (int s = 1; s <= RECOMP_NUM_SEG; s++) native32_w9x_set_sel(SEL(s), 0, 0, 1);
    return (uint16_t)(SEL(RECOMP_NUM_SEG + 1) & ~7);   /* the LDT alias, past them */
}

static int map_image(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[w16] no %s\n", path); return 0; }
    /* a 64 KB tail: a segment's selector reaches 64 KB past its base */
    g_img = VirtualAlloc(NULL, RECOMP_IMAGE_SIZE + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    size_t n = fread(g_img, 1, RECOMP_IMAGE_SIZE, f);
    fclose(f);
    if (n != RECOMP_IMAGE_SIZE) { fprintf(stderr, "[w16] %s: %u bytes\n", path, (unsigned)n); return 0; }
    /* ponytail: every segment is code (execute/read); nothing checks data
     * rights yet. RECOMP_MODULES knows which are code when that matters. */
    for (int s = 1; s <= RECOMP_NUM_SEG; s++)
        native32_w9x_set_sel(SEL(s), (uint32_t)(uintptr_t)g_img + SEG_SEGMENT_BASE[s], 0xFFFF, 1);
    native32_w16_enter = w16_enter;
    {   /* QT_Thunk's 16-bit returns ([0xBFFC979C] for a Win32 thread, [0xBFFC97A4]
         * for the task's thread) come from KRNL386's handshake block (+1C,
         * +24): on Win98, KRNL386 stubs that jump far back into kernel32's
         * tails. The host builds the same two stubs, 66 EA off32 sel16. */
        static uint8_t stubs[16] = { 0x66, 0xEA, 0x6D, 0x28, 0xF7, 0xBF, 0x1B, 0x00,
                                     0x66, 0xEA, 0x13, 0x29, 0xF7, 0xBF, 0x1B, 0x00 };
        uint16_t ss = native32_w9x_alloc_sel((uint32_t)(uintptr_t)stubs, sizeof stubs - 1);
        MEM32(0xBFFC979Cu) = (uint32_t)ss << 16;
        MEM32(0xBFFC97A4u) = (uint32_t)ss << 16 | 8;
    }
    fprintf(stderr, "[w16] %u segments of KRNL386+GDI+USER at %p, selectors %04X..%04X\n",
            RECOMP_NUM_SEG, (void*)g_img, SEL(1), SEL(RECOMP_NUM_SEG));
    return 1;
}

/* QT_Thunk(target, word args...) from a user32-style frame: the 16-bit
 * callee is PASCAL, so args go in declaration order and it pops them. */
void mach_enter(void);
void mach_leave(void);
static uint32_t qt_thunk(uint32_t target, uint16_t ds16, int n, const uint16_t* args) {
    mach_enter();   /* the registers are the boot thread's from here */
    uint32_t save[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    uint16_t sds = g_seg_ds;
    /* The boot thread is KRNL386's task on Win98: QT_Thunk takes its 16-bit
     * stack from TIB+0E (0xBFF728BA), a selector over the 32-bit stack.
     * ponytail: one selector, 48 KB under the stack as it is now; the
     * Win32-thread path (TIB+78/7C, 0xBFF7240B) builds its own. */
    static uint16_t stack16;
    if (!stack16) stack16 = native32_w9x_alloc_sel((g_esp & ~0xFFFu) - 0xC000u, 0xFFFF);
    MEM16(g_fs_base + 0xE) = stack16;
    g_esp -= 4; MEM32(g_esp) = g_ebp; g_ebp = g_esp;   /* push ebp; mov ebp, esp */
    g_esp -= 4; MEM32(g_esp) = 0;                      /* push ecx (thunk index) */
    g_esp -= 0x3C;
    for (int i = 0; i < n; i++) { g_esp -= 2; MEM16(g_esp) = args[i]; }
    g_edx = target;
    g_seg_ds = ds16;   /* USER's exports run on its DGROUP (its thunk dispatcher sets it) */
    native32_call_guest(QT_THUNK, 0, NULL);
    uint32_t r = (g_edx & 0xFFFF) << 16 | (g_eax & 0xFFFF);
    g_eax = save[0]; g_ecx = save[1]; g_edx = save[2]; g_ebx = save[3];
    g_esp = save[4]; g_ebp = save[5]; g_esi = save[6]; g_edi = save[7];
    g_seg_ds = sds;
    mach_leave();
    return r;
}

int w16_test(void) {
    if (!map_image(W16_IMAGE)) return 1;
    const char* lm = getenv("W16_LIBMAIN");
    int krnl_boot = lm && !_stricmp(lm, "KERNEL");   /* kernel32 not booted yet: KRNL386 boots it */
    uint16_t user_ds = SEL(RECOMP_MODULES[2].dgroup);
    if (!krnl_boot) {
    /* USER.GETSYSTEMMETRICS (ordinal 179) = seg 4 of USER (global 55):0FDD */
    static const int sm[] = { 0, 1, 0x51, 0x52 };   /* 4 (SM_CYCAPTION) goes into KRNL386: needs its boot data */
    /* USER's LibMain has not run, so its metrics table (DGROUP:02D8, a word
     * per index, seg055_5936) is the image's zeros. Plant SM_CXSCREEN and
     * SM_CYSCREEN from the 32-bit side, flat, as user32 reads that table:
     * the 16-bit code must see the same bytes. */
    uint8_t* udg = (uint8_t*)(uintptr_t)(recomp_sel_base(user_ds));
    *(uint16_t*)(udg + 0x2D8) = 640;
    *(uint16_t*)(udg + 0x2DA) = 480;
    for (int i = 0; i < (int)(sizeof sm / sizeof *sm); i++) {
        uint16_t a = (uint16_t)sm[i];
        uint32_t r = qt_thunk((uint32_t)SEL(55) << 16 | 0x0FDD, user_ds, 1, &a);
        printf("USER.GetSystemMetrics(%d) = %d\n", sm[i], (int16_t)r);
    }
    /* USER.GETDOUBLECLICKTIME (21) = global seg 54:9C10, no args */
    printf("USER.GetDoubleClickTime() = %u\n", qt_thunk((uint32_t)SEL(54) << 16 | 0x9C10, user_ds, 0, NULL) & 0xFFFF);
    fflush(stdout);
    }
    /* W16_LIBMAIN=GDI|USER: run that module's own entry (LibEntry: ds =
     * hInstance = DGROUP, cx = heap size, es:si = command line) and stop at
     * the first thing it needs that is not there yet. */
    for (int m = 0; lm && m < 3; m++) {
        const RecompModule* rm = &RECOMP_MODULES[m];
        if (_stricmp(lm, rm->name)) continue;
        static uint8_t* stk;
        if (!stk) stk = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CPU r; memset(&r, 0, sizeof r);
        r.ss = native32_w9x_alloc_sel((uint32_t)(uintptr_t)stk, 0xFFFF); r.sp = 0xFFF0;
        r.cs = SEL(rm->cs); r.ip = rm->ip;
        r.ds = r.di = SEL(rm->dgroup); r.cx = rm->heap; r.es = r.ss; r.si = 0xFFF0;
        if (m == 0) {   /* KRNL386 BootStrap: "KO" from its loader, es = its PSP paragraph */
            uint32_t w16_real_base(void);
            uint8_t* rm = (uint8_t*)(uintptr_t)w16_real_base();
            enum { W16_PSP = 0x1000 };            /* identity.c W16_PSP_PARA */
            uint8_t* psp = rm + W16_PSP * 16;
            psp[0] = 0xCD; psp[1] = 0x20;         /* int 20h */
            psp[0x80] = 0; psp[0x81] = 0x0D;      /* empty command tail */
            {   /* the environment, then (DOS 3+) a word 1 and the program's own path,
                 * which KRNL386 opens: at paragraph A000h, above its arena */
                static const char env[] = "PATH=C:\\WINDOWS;C:\\WINDOWS\\COMMAND\0windir=C:\\WINDOWS\0\0\1\0C:\\WINDOWS\\SYSTEM\\KRNL386.EXE";
                memcpy(rm + 0xA0000, env, sizeof env);
                *(uint16_t*)(psp + 0x2C) = 0xA000;
            }
            *(uint16_t*)(psp + 2) = 0xA000;       /* top of the program's DOS block */
            uint16_t lp = 0x1010;                 /* the load module's paragraph */
            FILE* f = fopen("work/sys/KRNL386.EXE", "rb");   /* the load module: the file past its 512-byte MZ header */
            if (f) {
                /* KRNL386's e_minalloc and e_maxalloc are both 0: DOS EXEC
                 * loads such a program high, its image at the top of the
                 * block (PSP:2) with the PSP still at the bottom. BootStrap
                 * relies on it: GlobalInit's first arena is the image, the
                 * space between PSP and image is where LoadSegment puts its
                 * segments, and the moves it then makes (C37E's slide) are
                 * 16-bit paragraph deltas inside conventional memory. */
                uint16_t mz[6];
                fread(mz, 2, 6, f);
                uint32_t img = (mz[2] - (mz[1] ? 1u : 0u)) * 512u + mz[1] - mz[4] * 16u;
                lp = (uint16_t)(0xA000 - (img + 15) / 16);
                fseek(f, 0x200, SEEK_SET); fread(rm + lp * 16u, 1, img, f);
                /* KERNEL's selectors point at its segments where DOS loaded them:
                 * load module + (file offset - 200h), so GlobalInit's
                 * paragraph*16 arithmetic holds (identity low layout). The bytes
                 * stay the file's: KRNL386 applies its own fixups (0DED walks
                 * each chain from the raw bytes), so relocated ones break it. */
                uint8_t h[0x40]; uint16_t ne;
                fseek(f, 0x3C, SEEK_SET); fread(&ne, 2, 1, f);
                fseek(f, ne, SEEK_SET); fread(h, 1, sizeof h, f);
                uint16_t st = *(uint16_t*)(h + 0x22), al = *(uint16_t*)(h + 0x32);
                /* Code segments 1-3 run from a private copy of the file's
                 * bytes, not the DOS-loaded image: BootStrap's LoadSegment
                 * moves each segment down over that image (seg 1 to the load
                 * module's start) while the lifted code still reads cs:[n]
                 * through SEG_n until it is rebound to the new selector
                 * (identity.c w16_rebind). DGROUP (4) stays where DOS put it. */
                static uint8_t* priv;
                if (!priv) priv = VirtualAlloc(NULL, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                memcpy(priv, rm + lp * 16u, img);   /* with each segment's relocation records after it */
                for (int s = 1; s <= 4; s++) {
                    uint16_t e[4];
                    fseek(f, ne + st + (s - 1) * 8, SEEK_SET); fread(e, 2, 4, f);
                    uint8_t* at = (s < 4 ? priv : rm + lp * 16u) + ((uint32_t)e[0] << al) - 0x200;
                    native32_w9x_set_sel(SEL(s), (uint32_t)(uintptr_t)at, 0xFFFF, 1);
                }
                fclose(f);
            }
            {   /* SYSTEM/KEYBOARD/MOUSE/DISPLAY.DRV for KRNL386's own LoadModule: host stand-ins (drv16.c) */
                void w16_drv_write(const char *sysdir);
                w16_drv_write("work/root/WINDOWS/SYSTEM");
                /* SYSTEM.INI [boot] names them (USER's LibEntry LoadLibrarys display.drv=; with
                 * none it writes *DisplayFallback=1 and fails). Rewritten each run: the guest's own. */
                FILE* ini = fopen("work/root/WINDOWS/SYSTEM.INI", "w");
                if (ini) {
                    fputs("[boot]\nsystem.drv=system.drv\nkeyboard.drv=keyboard.drv\nmouse.drv=mouse.drv\n"
                          "display.drv=display.drv\ncomm.drv=comm.drv\nsound.drv=mmsound.drv\n"
                          "fonts.fon=vgasys.fon\nfixedfon.fon=vgafix.fon\noemfonts.fon=vgaoem.fon\n", ini);
                    fprintf(ini, "shell=%s\n", getenv("W16_SHELL") ? getenv("W16_SHELL") : "calc.exe");   /* W16_SHELL=winmine.exe, ... */
                    fputs("\n[boot.description]\ndisplay.drv=pcrecomp host display\n", ini);
                    fclose(ini);
                }
            }
            r.ax = 0x4B4F; r.es = 0x1000;
            /* what KRNL386's MZ stub (the load module's first bytes, entered by
             * DOS) hands BootStrap: ss = the load module's paragraph,
             * sp = si = 200h (its NE header, rounded up from PSP+80h), ds = di =
             * DGROUP, bx = 0 (the NE alignment shift - 4), cx = si + 200h */
            r.ss = native32_w9x_alloc_sel(lp * 16u, 0xFFFF); r.sp = 0x204;   /* w16_call pushes a far return */
            native32_w9x_desc(r.ss)[6] &= (uint8_t)~0x40;   /* DPMI's 16-bit ss (B=0): KRNL386 keeps it as its stack (032F) */
            r.si = 0x200; r.cx = 0x400; r.bx = 0;
        }
        printf("%s LibMain %04X:%04X ds %04X\n", rm->name, r.cs, r.ip, r.ds); fflush(stdout);
        int w16_call(CPU*);
        /* inside the machine lock: a nested mach_enter (a wait, a callback)
         * at depth 0 would reload this thread's saved fs over KRNL386's TIB */
        mach_enter();
        int ran = w16_call(&r);
        mach_leave();
        printf("%s LibMain ran=%d ax=%04X\n", rm->name, ran, r.ax);
    }
    fflush(stdout);
    return 0;
}
