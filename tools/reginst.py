#!/usr/bin/env python3
"""Seed the guest's registry box with Windows 98's own INF data.

    py -3 tools/reginst.py BOX            (e.g. w98\\xcopy, the w16test box)

What Win98 setup writes for the shell: shell.inf's [shell] AddReg sections,
ieshell.inf's [MSIE4shell_Inst] (the integrated shell), and the REGINST
resources of the DLLs ieshell.inf registers (shell32, shdocvw, browseui:
their DllRegisterServer hands advpack these INFs). All into
HKCU\\Software\\pcrecomp\\BOX\\{HKLM,HKCU} (pcrecomp regbox32.c; HKCR is
HKLM\\SOFTWARE\\Classes there). Paths stay the guest's (C:\\WINDOWS...),
which vxd32 boxes under work/root.
ponytail: AddReg only (no DelReg, no UpdateInis); flag bits 1 (binary),
0x10001 (dword), 0x10000 (multi-sz), 0x20000 (expand), 2 (no clobber) read, the rest ignored.
"""
import csv, os, re, sys, winreg
import pefile

SYS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work', 'sys')
LDID = {'10': r'C:\WINDOWS', '11': r'C:\WINDOWS\SYSTEM', '25': r'C:\WINDOWS', '30': 'C:\\', '24': 'C:\\',
        '17': r'C:\WINDOWS\INF', '18': r'C:\WINDOWS\HELP', '20': r'C:\WINDOWS\FONTS', '12': r'C:\WINDOWS\SYSTEM\IOSUBSYS'}


def sections(text):
    sec, cur = {}, None
    for l in re.sub(r'\\[ \t]*\r?\n\s*', '', text).splitlines():   # INF line continuations
        s = l.strip()
        if not s.startswith('"'):
            s = re.sub(r';.*$', '', s) if '"' not in s else s
        s = s.strip()
        if s.startswith('['): cur = sec.setdefault(s[1:s.index(']')].lower(), [])
        elif s and not s.startswith(';') and cur is not None: cur.append(s)
    return sec


def strings(sec, extra):
    strs = dict(LDID, **extra)
    for l in sec.get('strings', []):
        m = re.match(r'"?(\w+)"?\s*=\s*(?:"([^"]*)"|([^;]*?))\s*(?:;.*)?$', l)   # a quoted value may have a comment after it
        if m: strs[m.group(1).lower()] = m.group(2) if m.group(2) is not None else m.group(3)
    return lambda s: re.sub(r'%(\w+)%', lambda m: strs.get(m.group(1).lower(), m.group(0)), s).replace('%%', '%')


def addreg(sec, names, sub):
    for n in names:
        for l in sec.get(n.strip().lower(), []):
            f = [x.strip() for x in next(csv.reader([sub(l)], skipinitialspace=True))]
            if len(f) >= 2 and f[0].upper() in ('HKLM', 'HKCR', 'HKCU'):
                yield f + [''] * (5 - len(f))


def inf_install(path, install):
    sec = sections(open(path, encoding='latin-1').read())
    sub = strings(sec, {})
    for l in sec.get(install.lower(), []):
        k, _, v = l.partition('=')
        if k.strip().lower() == 'addreg':
            yield from addreg(sec, v.split(','), sub)


def reginst(dll):
    p = pefile.PE(os.path.join(SYS, dll), fast_load=True)
    p.parse_data_directories([pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    for t in getattr(p, 'DIRECTORY_ENTRY_RESOURCE', type('', (), {'entries': []})).entries:
        if t.name and str(t.name) == 'REGINST':
            e = t.directory.entries[0].directory.entries[0].data.struct
            sec = sections(p.get_data(e.OffsetToData, e.Size).decode('latin-1'))
            mod = LDID['11'] + '\\' + dll
            sub = strings(sec, {'_sys_mod_path': mod, '_mod_path': mod})
            # the install sections advpack runs on DllRegisterServer: every AddReg= not under an "un"/"del" one
            for name, ls in sec.items():
                if re.search(r'unreg|uninst|del|remove', name): continue
                for l in ls:
                    k, _, v = l.partition('=')
                    if k.strip().lower() == 'addreg':
                        yield from addreg(sec, v.split(','), sub)


def main():
    box = sys.argv[1]
    items = list(inf_install(os.path.join(SYS, 'shell.inf'), 'shell')) \
        + list(inf_install(os.path.join(SYS, 'ieshell.inf'), 'MSIE4shell_Inst'))         + list(inf_install(os.path.join(SYS, 'timezone.inf'), 'Install_TimeZone'))
    for d in ('shell32.dll', 'shdocvw.dll', 'browseui.dll'):
        items += list(reginst(d))
    n = 0
    for hive, key, name, flags, val, *data in items:
        if re.search(r'(?i)\\RunOnce|\\RenameFiles|\\Active Setup\\', key): continue   # setup's one-shot work, not the shell's state
        if flags and not re.match(r'(?i)^(0x)?[0-9a-f]+$', flags): flags, val = '', flags
        fl = int(flags, 0) if flags else 0
        at = r'Software\pcrecomp\%s\%s' % (box, {'HKCR': r'HKLM\SOFTWARE\Classes', 'HKLM': 'HKLM', 'HKCU': 'HKCU'}[hive.upper()])
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, at + '\\' + key) as k:
            if fl & 2:   # FLG_ADDREG_NOCLOBBER
                try: winreg.QueryValueEx(k, name or None); continue
                except OSError: pass
            if fl & 1 and fl & 0x10000:   # REG_DWORD written as 4 bytes
                v = [int(x, 16) for x in [val] + data if x]
                if len(v) == 1: v = list(v[0].to_bytes(4, 'little'))   # one number, not bytes (0x10001,"0x...")
                b = bytes(v)
                winreg.SetValueEx(k, name or None, 0, winreg.REG_DWORD, int.from_bytes(b[:4].ljust(4, b'\0'), 'little')) if len(b) <= 4 else \
                    winreg.SetValueEx(k, name or None, 0, winreg.REG_BINARY, b)
            elif fl & 0x10001 == 0x10000:
                winreg.SetValueEx(k, name or None, 0, winreg.REG_MULTI_SZ, [x for x in [val] + data if x])   # FLG_ADDREG_TYPE_MULTI_SZ
            elif fl & 1:
                winreg.SetValueEx(k, name or None, 0, winreg.REG_BINARY, bytes(int(x, 16) for x in [val] + data if x))
            elif val or name:
                winreg.SetValueEx(k, name or None, 0, winreg.REG_EXPAND_SZ if fl & 0x20000 else winreg.REG_SZ, val)
        n += 1
    # The time zone: timezone.inf leaves "!!!First Boot!!!", and Explorer then WinExecs
    # "rundll32 shell32.dll,Control_RunDLL timedate.cpl,,/m" for setup's zone pick. Pick here what
    # that page would: Win98's first zone with the host's bias, written as timedate.cpl does.
    # ponytail: first match by bias only (several zones share one); the host's zone name is not mapped.
    import time
    tzs = r'Software\pcrecomp\%s\HKLM\Software\Microsoft\Windows\CurrentVersion\Time Zones' % box
    bias = time.timezone // 60
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, tzs) as k:
        names = []
        try:
            for i in range(1000): names.append(winreg.EnumKey(k, i))
        except OSError: pass
    for zn in names:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, tzs + '\\' + zn) as z:
            tzi = winreg.QueryValueEx(z, 'TZI')[0]
            if int.from_bytes(tzi[:4], 'little', signed=True) != bias: continue
            std, dlt = winreg.QueryValueEx(z, 'Std')[0], winreg.QueryValueEx(z, 'Dlt')[0]
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r'Software\pcrecomp\%s\HKLM\System\CurrentControlSet\control\TimeZoneInformation' % box) as k:
            for nm, v in (('StandardName', std), ('DaylightName', dlt)):
                winreg.SetValueEx(k, nm, 0, winreg.REG_SZ, v)
            for nm, v in (('Bias', tzi[0:4]), ('StandardBias', tzi[4:8]), ('DaylightBias', tzi[8:12]),
                          ('StandardStart', tzi[12:28]), ('DaylightStart', tzi[28:44]), ('ActiveTimeBias', tzi[0:4])):
                winreg.SetValueEx(k, nm, 0, winreg.REG_BINARY, v)
        print('time zone:', zn)
        break
    # HKCC\Display\Settings, which setup's display class installer writes (no INF has it): USER's
    # boot (seg 21 3A67) opens it, and when it is missing queues RunOnce *DisplayFallback ("rundll32
    # shell32.dll,Control_RunDLL desk.cpl ,-1"). The mode is the display stand-in's (drv16, 800x600x16).
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r'Software\pcrecomp\%s\HKLM\SYSTEM\CurrentControlSet'
                          r'\Hardware Profiles\Current\Display\Settings' % box) as k:
        for nm, v in (('Resolution', '800,600'), ('BitsPerPixel', '16'), ('DPILogicalX', '96'), ('DPILogicalY', '96'),
                      ('DPIPhysicalX', '96'), ('DPIPhysicalY', '96'), ('fonts.fon', 'vgasys.fon'),
                      ('fixedfon.fon', 'vgafix.fon'), ('oemfonts.fon', 'vgaoem.fon')):
            winreg.SetValueEx(k, nm, 0, winreg.REG_SZ, v)
    # USER's appearance keys are the guest's whole (pcrecomp regbox32.c "pcrecomp:whole"): a value
    # Win98 has not written falls to USER's own default, not to Windows 11's (white faces, a black desktop)
    for k in (r'Control Panel\Colors', r'Control Panel\Desktop', r'Control Panel\Desktop\WindowMetrics'):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r'Software\pcrecomp\%s\HKCU\%s' % (box, k)) as h:
            winreg.SetValueEx(h, 'pcrecomp:whole', 0, winreg.REG_DWORD, 1)
    # The startup lists are the box's own, empty: else the open falls through to the host's
    # (read-only), and Explorer starts the host's Run entries (rundll32 ..., "Cannot find").
    for hive in ('HKLM', 'HKCU'):
        for k in ('Run', 'RunOnce', 'RunOnceEx', 'RunServices', 'RunServicesOnce'):
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r'Software\pcrecomp\%s\%s\Software\Microsoft\Windows\CurrentVersion\%s' % (box, hive, k)) as h:
                while True:   # and emptied: an old run's leftovers (USER's *DisplayFallback desk.cpl line)
                    try: winreg.DeleteValue(h, winreg.EnumValue(h, 0)[0])
                    except OSError: break
    print('%d lines into HKCU\\Software\\pcrecomp\\%s' % (n, box))


if __name__ == '__main__':
    main()
