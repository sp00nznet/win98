"""hiddenrun.py - run a program on a private, invisible desktop and screenshot its windows.

    python tools/hiddenrun.py --secs 30 --shot 10 --shot 25 --out out/run -- build/johnny.exe /s

Nothing appears on the real screens. Each --shot T writes <out>_T.png: every
visible top-level window of the process, read from its window DC, pasted at its screen position. Stdout/stderr of the
program go to <out>.log. The process tree is killed at --secs.
"""
import argparse, os, ctypes, ctypes.wintypes as W, subprocess, time
from PIL import Image

u32, k32, g32 = ctypes.windll.user32, ctypes.windll.kernel32, ctypes.windll.gdi32
u32.CreateDesktopW.restype = W.HANDLE


def windows_of(pid, desk):
    out = []
    cb = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def f(h, _):
        p = W.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        # Windows 11 adds its UAC input indicator windows (UAC_InputIndicator*,
        # in the program's own process): not the program's
        b = ctypes.create_unicode_buffer(64); u32.GetClassNameW(h, b, 64)
        if p.value != os.getpid() and u32.IsWindowVisible(h) and not b.value.startswith('UAC'):   # the private desktop holds only the run's process tree (hold_image relaunches)
            out.append(h)
        return True
    u32.EnumDesktopWindows(W.HANDLE(desk), cb(f), 0)
    return out


def grab(h):
    r = W.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    w, hh = r.right - r.left, r.bottom - r.top
    if w <= 0 or hh <= 0:
        return None, r
    # (the window DC only makes a compatible bitmap; the capture is PrintWindow)
    sdc = u32.GetWindowDC(h); mdc = g32.CreateCompatibleDC(sdc)
    bmp = g32.CreateCompatibleBitmap(sdc, w, hh); g32.SelectObject(mdc, bmp)
    # PrintWindow renders the window alone; a BitBlt from its DC also reads
    # whatever screen pixels lie under it (the user's desktop).
    u32.PrintWindow(h, mdc, 2)   # PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * hh * 4)
    bi = (ctypes.c_uint32 * 10)(40, w, (-hh) & 0xFFFFFFFF, 1 | (32 << 16), 0, 0, 0, 0, 0, 0)
    g32.GetDIBits(mdc, bmp, 0, hh, buf, bi, 0)
    g32.DeleteObject(bmp); g32.DeleteDC(mdc); u32.ReleaseDC(h, sdc)
    return Image.frombuffer('RGBA', (w, hh), buf, 'raw', 'BGRA', 0, 1).convert('RGB'), r


class STARTUPINFO(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('lpReserved', W.LPWSTR), ('lpDesktop', W.LPWSTR), ('lpTitle', W.LPWSTR),
                ('x', W.DWORD), ('y', W.DWORD), ('cx', W.DWORD), ('cy', W.DWORD), ('cxc', W.DWORD),
                ('cyc', W.DWORD), ('fill', W.DWORD), ('dwFlags', W.DWORD), ('wShow', W.WORD),
                ('cb2', W.WORD), ('r2', ctypes.c_void_p), ('hIn', W.HANDLE), ('hOut', W.HANDLE), ('hErr', W.HANDLE)]


class PI(ctypes.Structure):
    _fields_ = [('hProcess', W.HANDLE), ('hThread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]


class Proc:
    def __init__(s, pi): s.h, s.pid = pi.hProcess, pi.pid
    def poll(s):
        c = W.DWORD(); k32.GetExitCodeProcess(s.h, ctypes.byref(c))
        return None if c.value == 259 else c.value


def launch(cmd, desktop, logpath):
    import msvcrt, os
    log = open(logpath, 'w')
    h = msvcrt.get_osfhandle(log.fileno())
    os.set_handle_inheritable(h, True)
    si = STARTUPINFO(); si.cb = ctypes.sizeof(si); si.lpDesktop = desktop
    si.dwFlags = 0x100; si.hOut = si.hErr = h
    pi = PI()
    # loader/hard-error boxes go to the interactive desktop via csrss: suppress them (inherited by the child)
    k32.SetErrorMode(0x0001 | 0x0002 | 0x8000)   # FAILCRITICALERRORS | NOGPFAULTERRORBOX | NOOPENFILEERRORBOX
    if not k32.CreateProcessW(None, ctypes.create_unicode_buffer(subprocess.list2cmdline(cmd)),
                              None, None, True, 0, None, None, ctypes.byref(si), ctypes.byref(pi)):
        raise OSError(ctypes.GetLastError())
    return Proc(pi)


def preview_window(desk, w, h):
    """A plain window standing in for the Control Panel's preview box, owned
    by a thread on the hidden desktop that keeps pumping its messages."""
    import threading
    got = []
    def run():
        u32.SetThreadDesktop(W.HANDLE(desk))
        u32.CreateWindowExW.restype = W.HWND
        hw = u32.CreateWindowExW(0, 'STATIC', 'preview', 0x10000000 | 0x00800000, 50, 50, w, h,
                                 None, None, None, None)   # WS_VISIBLE | WS_BORDER
        got.append(hw)
        msg = W.MSG()
        while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            u32.TranslateMessage(ctypes.byref(msg)); u32.DispatchMessageW(ctypes.byref(msg))
    threading.Thread(target=run, daemon=True).start()
    while not got:
        time.sleep(0.05)
    return got[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--secs', type=float, default=30)
    ap.add_argument('--shot', type=float, action='append', default=[])
    ap.add_argument('--out', default='out/run')
    ap.add_argument('--preview', metavar='WxH', help='make a window this size on the hidden desktop '
                    'and pass its handle where the command says {hwnd} (a screen saver /p)')
    ap.add_argument('--end-at', type=float, help='at this second, close the preview window (--preview) '
                    'or press a key in the window of the program, as a person ending a screen saver would')
    ap.add_argument('--click', action='append', default=[], help='X,Y@T: a left click posted to the main '
                    'window at client X,Y, T seconds in')
    ap.add_argument('--command', action='append', default=[], help='ID@T: WM_COMMAND ID posted to the main window')
    ap.add_argument('--post', action='append', default=[], help='MSG,WPARAM,LPARAM@T: any message posted to the '
                    'main window (0x112,0xF100,0x67@T: WM_SYSCOMMAND SC_KEYMENU, the menu of the letter g)')
    ap.add_argument('--type', action='append', default=[], help='ID=TEXT@T: typed into control ID of the top window '
                    '(a dialog): EM_SETSEL all, then a posted WM_CHAR per character')
    ap.add_argument('--dlgcommand', action='append', default=[], help='ID@T: WM_COMMAND ID posted to the top window')
    ap.add_argument('--drag', action='append', default=[], help='X1,Y1,X2,Y2@T: a left-button drag in the main '
                    'window client coordinates, posted to the deepest child under X1,Y1')
    ap.add_argument('cmd', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd[:1] == ['--'] else a.cmd
    name = os.environ.get('HIDDENRUN_DESKTOP', 'pcrecomp_hidden')
    desk = u32.CreateDesktopW(name, None, None, 0, 0x10000000, None)
    if a.preview:
        hwnd = a.preview_hwnd = preview_window(desk, *map(int, a.preview.split('x')))
        cmd = [c.replace('{hwnd}', str(hwnd)) for c in cmd]
    p = launch(cmd, name, a.out + '.log')
    t0 = time.time()
    import threading
    th = threading.Thread(target=shots, args=(a, p, desk, t0)); th.start()   # joined after the input loop
    ended = False
    posts = sorted([(float(c.split('@')[1]), 'click', c.split('@')[0]) for c in a.click] +
                   [(float(c.split('@')[1]), 'cmd', c.split('@')[0]) for c in a.command] +
                   [(float(c.split('@')[1]), 'post', c.split('@')[0]) for c in a.post] +
                   [(float(c.split('@')[1]), 'type', c.split('@')[0]) for c in a.type] +
                   [(float(c.split('@')[1]), 'dlgcmd', c.split('@')[0]) for c in a.dlgcommand] +
                   [(float(c.split('@')[1]), 'drag', c.split('@')[0]) for c in a.drag])
    while time.time() - t0 < a.secs and p.poll() is None:
        while posts and time.time() - t0 >= posts[0][0]:
            _, kind, what = posts.pop(0)
            ws = windows_of(p.pid, desk)
            main_w = ws[-1] if ws else None          # top-level, bottom of the z-order: the main window
            if main_w and kind == 'click':
                x, y = map(int, what.split(','))
                lp = (y << 16) | (x & 0xFFFF)
                u32.PostMessageW(main_w, 0x0201, 1, lp)   # WM_LBUTTONDOWN, MK_LBUTTON
                u32.PostMessageW(main_w, 0x0202, 0, lp)   # WM_LBUTTONUP
            elif main_w and kind == 'drag':
                x1, y1, x2, y2 = map(int, what.split(','))
                u32.ChildWindowFromPointEx.restype = W.HWND
                h, pt = main_w, W.POINT(x1, y1)
                while True:   # down to the deepest visible child (pt in h's client coordinates)
                    c = u32.ChildWindowFromPointEx(h, pt, 1)
                    if not c or c == h: break
                    u32.MapWindowPoints(h, c, ctypes.byref(pt), 1); h = c
                dx, dy = pt.x - x1, pt.y - y1
                for i in range(11):
                    x, y = x1 + (x2 - x1) * i // 10 + dx, y1 + (y2 - y1) * i // 10 + dy
                    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
                    u32.PostMessageW(h, 0x0201 if i == 0 else 0x0200, 1, lp)   # WM_LBUTTONDOWN, then WM_MOUSEMOVE
                u32.PostMessageW(h, 0x0202, 0, lp)                              # WM_LBUTTONUP
            elif ws and kind == 'type':
                cid, text = what.split('=', 1)
                u32.GetDlgItem.restype = W.HWND
                e = u32.GetDlgItem(ws[0], int(cid, 0))
                u32.FindWindowExW.restype = W.HWND
                for _ in range(3):                                       # a combo box: its Edit (Open dialog)
                    sub = u32.FindWindowExW(e, None, 'Edit', None) or u32.FindWindowExW(e, None, 'ComboBox', None)
                    if not sub: break
                    e = sub
                u32.PostMessageW(e, 0x00B1, 0, -1)                    # EM_SETSEL all
                for ch in text: u32.PostMessageW(e, 0x0102, ord(ch), 1)  # WM_CHAR
            elif ws and kind == 'dlgcmd':
                u32.PostMessageW(ws[0], 0x0111, int(what, 0), 0)
            elif main_w and kind == 'post':
                m, wp, lp = (int(v, 0) for v in what.split(','))
                u32.PostMessageW(main_w, m, wp, lp)
            elif main_w:
                u32.PostMessageW(main_w, 0x0111, int(what, 0), 0)   # WM_COMMAND
            print(f'{time.time() - t0:.1f}s: {kind} {what}')
        if a.end_at is not None and not ended and time.time() - t0 >= a.end_at:
            ended = True
            for h in ([a.preview_hwnd] if a.preview else windows_of(p.pid, desk)):
                if a.preview: u32.PostMessageW(h, 0x0010, 0, 0)                 # WM_CLOSE
                else: u32.PostMessageW(h, 0x0100, 0x20, 0x00390001)             # WM_KEYDOWN space
            print(f'{a.end_at}s: ended')
        time.sleep(0.2)
    th.join()
    print('exit', p.poll(), f'after {time.time() - t0:.1f}s')
    k32.TerminateProcess(p.h, 1)
    # taskkill /T times out on a guest host stuck in a wait (and leaves its child,
    # which holds work/root files the next run rewrites): also end every process of
    # this exe path. ponytail: by path, so two runs of one exe can't overlap.
    exe = os.path.abspath(cmd[0]).replace("'", "''")
    subprocess.run(['powershell', '-NoProfile', '-c', f"Get-Process | ? {{ $_.Path -eq '{exe}' }} | Stop-Process -Force"], capture_output=True)
    k32.WaitForSingleObject(p.h, 10000)   # gone (its mapped files released) before the next run starts
    u32.CloseDesktop(desk)


def shots(a, p, desk, t0):
    # GDI reads across desktops come back black: capture from a thread that
    # has joined the hidden desktop.
    u32.SetThreadDesktop(W.HANDLE(desk))
    for t in sorted(a.shot):
        while time.time() - t0 < t and p.poll() is None:
            time.sleep(0.1)
        hs = [a.preview_hwnd] if a.preview else windows_of(p.pid, desk)
        # the top window active, so its menu bar and caption are drawn active
        # whatever the run's timing left (an inactive menu bar: a flake)
        if hs and not a.preview and u32.GetForegroundWindow() != hs[0]:
            u32.SetForegroundWindow(hs[0]); time.sleep(0.3)
        shots = [grab(h) for h in hs]
        shots = [s for s in shots if s[0]]
        if os.environ.get('HIDDENRUN_RECTS'):
            for _, r in shots: print(f'  window {r.left},{r.top} {r.right - r.left}x{r.bottom - r.top}')
        if not shots:
            print(f'{t}s: no window'); continue
        x0 = min(r.left for _, r in shots); y0 = min(r.top for _, r in shots)
        W_ = max(r.right for _, r in shots) - x0; H_ = max(r.bottom for _, r in shots) - y0
        canvas = Image.new('RGB', (W_, H_))
        for img, r in reversed(shots):   # EnumWindows is top-first; paint bottom-up
            canvas.paste(img, (r.left - x0, r.top - y0))
        path = f'{a.out}_{int(t)}.png'; canvas.save(path); print(f'{t}s: {path} {canvas.size}')


if __name__ == '__main__':
    main()
