#!/usr/bin/env python3
#
# kernel-size.py -- categorise every byte in build/ntoskrnl/xboxkrnl.exe.
#
# usage:
#   tools/kernel-size.py [PATH/TO/xboxkrnl.exe]
#
# Defaults to build/ntoskrnl/xboxkrnl.exe relative to repo root.  Reads the
# PE section table, runs nm with --print-size, and classifies every symbol
# (by name prefix) into a subsystem/library bucket.  Output is a table
# that adds up to the section sums.
#
# Why this exists.  --gc-sections + per-driver static libs make it hard to
# eyeball what's actually consuming bytes -- you can't just look at .obj
# files because the linker drops a lot.  This walks the linked binary
# instead.  Use it before/after a trim pass to see what moved.
#
# The classifier is name-prefix based, hand-tuned for this codebase.  Any
# symbol that doesn't match any rule lands in "UNCLASSIFIED <name>" so
# misses are visible (and easy to add a rule for).  Section-name pseudo-
# symbols nm emits (".text", "INIT", etc.) bucket as "anonymous /
# inter-function padding" since they represent linker fillers between
# real symbols.

import os
import re
import subprocess
import sys

# ----------- classification rules --------------------------------------
# Each entry is (regex, bucket-name).  First match wins.  Run with
# debug() if you need to dump what each symbol classified to.

RULES = [
    # --- drivers (storage stack + FSDs) ----------------------------
    (r'^_?Ata[A-Z]|^_?Atapi|^_?AtaReq|^_?AtaPort|^_?Pata|^_?Pdo[A-Z]|^_?Fdo[A-Z]'
     r'|^_?ServiceTransferRequest|^_?InitializeTransferPackets'
     r'|^_?Setup(Read|Eject|Sense|Write)|^_?InterpretTransferPacketError'
     r'|^_?ScsiPort|^_?gsBmdma|^_?gsAtaInquiry|^_?CmdSetTransferMode'
     r'|^_?ChannelExtension|^_?IdeAtapi|^_?ViaGetController', 'drv/atapi'),
    (r'^_?Class[A-Z]|^_?Classp|^_?Transfer[A-Z]|^_?Srb[A-Z]|^_?gsScsi'
     r'|^_?WmiSysFsControl|^_?HistoryLogReturnedPacket|^_?RetryTransferPacket'
     r'|^_?SetupDriveCapacityTransferPacket|^_?SetupModeSelectTransferPacket'
     r'|^_?SetupModeSenseTransferPacket|^_?SubmitTransferPacket'
     r'|^_?InterpretTransferPacket|^_?TransferPkt|^_?DispatchClassPnp'
     r'|^_?InitializePool@', 'drv/classpnp'),
    (r'^_?Disk[A-Z]|^_?Diskp|^_?QueryDeviceInformation'
     r'|^_?DeviceProcessDsmTrim|^_?_?Mbr[A-Z]', 'drv/disk'),
    (r'^_?PciIde', 'drv/pciidex'),
    (r'^_?MountMgr|^_?Mountmgr|^_?Mountp|^_?Reconcile|^_?MountedDevice'
     r'|^_?SaveLetter|^_?MountedRegister|^_?Master[A-Z]'
     r'|^_?OnlineMountedVolumes|^_?QueryPointsFrom|^_?QuerySuggestedLink'
     r'|^_?QuerySymbolicLinkNames|^_?ReadVolumeLabel|^_?QueryVolumeName'
     r'|^_?ChangeRemoteDatabase|^_?CreateRemoteDatabase|^_?CheckForValid'
     r'|^_?WriteRemoteDatabase|^_?DeleteFromMaster|^_?DeleteFromLocal'
     r'|^_?AddRemoteDatabase|^_?AttachAndStart|^_?IssueUniqueIdChange'
     r'|^_?CreateStringWithGlobal|^_?IsDriveLetterAvailable'
     r'|^_?WaitForOnline|^_?MountedTunnel', 'drv/mountmgr'),
    (r'^_?PartMgr|^_?Partp|^_?MBRConvert|^_?GUIDPartition|^_?PartIoctl'
     r'|^_?PartCheckCmd|^_?PartitionHandle', 'drv/partmgr'),
    (r'^_?Vfat|^_?FAT[0-9A-Z]|^_?vfat|^_?DoQuery$|^_?FindFile$|^_?GetEntry'
     r'|^_?ReadDir', 'drv/vfatfs'),
    (r'^_?Xdvdfs|^_?Xdvd[A-Z]|^_?XdvdpFcb|^_?XdvdpRead|^_?XdvdpClose',
     'drv/xdvdfs'),
    (r'^_?NxCdrom|^_?Cdrom[A-Z]|^_?CdRom[A-Z]', 'drv/nxcdrom'),
    (r'^_?Pcip|^_?Pci_|^_?PciCreate|^_?PciDriverEntry|^_?PciIrq|^_?PciPnp'
     r'|^_?ListNum|^_?Devfn', 'drv/pci-bus'),
    (r'^_?wmi|^_?WmiLib|^_?Wmi[A-Z]', 'drv/wmilib'),
    (r'^_?Boot(Vid|Vga|Pci)|^_?Vid[A-Z]', 'drv/bootvid'),

    # --- HAL ------------------------------------------------------
    (r'^_?Halp|^_?Hal[A-Z]|^@xHal|^@nHal|^@Hal|^_?xHal|^_?nHal|^_?conexant'
     r'|^_?HalEnvironmentRead|^_?HalEnvironmentWrite|^_?Avp|^_?Av[A-Z]'
     r'|^_?xboxbvid|^_?Smb[A-Z]|^_?HaliRegisterBusHandler|^_?KdSerial'
     r'|^_?HaliQuery|^_?HaliSetSystem|^_?HaliInit'
     r'|^_?SetGPURegister|^_?focus_calc_mode|^_?Composite_XCal'
     r'|^_?BootPci|^_?BootVga', 'hal/halxbox'),

    # --- ntoskrnl subsystems --------------------------------------
    (r'^_?(Mm|Mi|Mmp)[A-Z]|^_?PeFmtCreateSection|^_?Pf[A-Z]'
     r'|^_?_PoolAlloc|^_?ExeFmtp?|^__Mm|^@Mi|^_RemoveFromWsList'
     r'|^__ZL16RemoveFromWsList|^_RtlImageNtHeader', 'ntoskrnl/mm'),
    (r'^_?(Ki|Ke|Kxp|Kx|Kep)[A-Z]|^@?Ki[A-Z]|^@Ke[A-Z]|^@Kf[A-Z]'
     r'|^@?Kii[A-Z]', 'ntoskrnl/ke'),
    (r'^_?(Iop|Io|Iovp|Iov)[A-Z]|^@Iof|^_?Raw(Fs|Query|Set|Read|Write|Create'
     r'|Close|Cleanup|Dispatch)', 'ntoskrnl/io'),
    (r'^_?(Obp|Ob)[A-Z]|^@?Obp|^@?Obf', 'ntoskrnl/ob'),
    (r'^_?(Psp|Ps)[A-Z]', 'ntoskrnl/ps'),
    (r'^_?(Exp|Ex|Exi|ExpP)[A-Z]|^_?Phase1Init|^_?QSI', 'ntoskrnl/ex'),
    (r'^_?(Cc|Ccp|Csq)[A-Z]', 'ntoskrnl/cc'),
    (r'^_?(Pop|Po)[A-Z]', 'ntoskrnl/po'),
    (r'^_?(Inbv|Inbvp|DisplayBootBitmap|ShowProgressBar|FinalizeBootLogo'
     r'|BootAnimation|VidpFontData|InbvSetSysFontColor|RotBars'
     r'|InitBootStringTable)', 'ntoskrnl/inbv'),
    (r'^_?Kdp[A-Z]|^_?Kd[A-Z]|^_?KdComport|^_?Kdcom|^_?Kdb',
     'ntoskrnl/kd'),
    (r'^_?FsRtl|^_?Fspp|^_?Fsp', 'ntoskrnl/fsrtl'),
    (r'^_?Fstub|^@?xHalIoRead|^@?xHalIoAssign|^@?xHalQueryDisk'
     r'|^@?xHalQueryDrive|^@?xHalIoSet', 'ntoskrnl/fstub'),
    (r'^_?(Vf|Vfp|Vfi)[A-Z]', 'ntoskrnl/vf'),
    # All nxkrnl-Xbox code/data uses the Nx* namespace (Nxk/Nxp/Nxc/Nxv/
    # Nxb...), so match Nx with no case constraint on the next char; the
    # Xe* loader/chainload helpers come along too.
    (r'^_?Nx|^_?Fsc[A-Z]|^_?Xep?[A-Z]|^_?Xe[A-Z]|^_?chainloadBuf|^_?Xboxmm|^_?XboxLoader',
     'ntoskrnl/xb'),
    (r'^_?Sep|^_?Se[A-Z]', 'ntoskrnl/se-stubs'),
    (r'^_?(Nt|Zw)[A-Z]', 'ntoskrnl/nt-api'),  # ordinal-exported Nt* bodies + SEH/APC plumbing; Zw is extinct

    # --- support libraries ---------------------------------------
    (r'^_?Rtl[A-Z]|^_?Rtlp|^_?ipv6_string', 'lib/rtl'),
    (r'^_?A_SHA|^_?MD[245]|^_?RC4', 'lib/cryptlib'),
    (r'^_?streamout|^_?wstreamout|^_?__cd|^_?__chk|^_?__alldiv'
     r'|^_?__aulldiv|^_?__alldvrm|^_?__llmul|^_?_chkstk|^_?__divdi'
     r'|^_?__udivdi|^_?__moddi|^_?__umoddi|^_?_purecall|^_?__report'
     r'|^_?memcpy|^_?memset|^_?memcmp|^_?memmove|^_?strncpy|^_?strncmp'
     r'|^_?strlen|^_?wcsnicmp|^_?strchr|^_?wcschr|^_?wcslen|^_?wcsncmp'
     r'|^_?wcscat|^_?wcsicmp|^_?atol|^_?atoi|^_?wcstoul|^_?wcstol'
     r'|^_?_strnicmp|^_?_stricmp|^_?_strupr|^_?wcsstr|^_?strcat'
     r'|^_?strcpy|^_?strstr|^_?format_float|^_?format_floatw'
     r'|^_?strtoul|^_?_vsnprintf|^_?_snprintf|^_?qsort|^_?bsearch'
     r'|^_?abs|^_?atexit|^_?vDbgPrint|^_?DbgPrint|^_?__udivmoddi'
     r'|^_?__ctype|^_?__wctype|^_?_ctype|^_?towlower|^_?towupper'
     r'|^_?iswctype|^_?isdigit|^_?isspace|^_?tolower|^_?toupper',
     'lib/crt-cntpr'),
    (r'^_?Cmp|^_?Hv[A-Z]|^_?Cmlib|^_?CmHive', 'lib/cmlib'),
    (r'^_?SEH|^_?seh_|^_?_SEH|^_?_local_unwind|^___pseh', 'lib/pseh'),
    (r'^_?__guard|^_?__security|^___stack_chk', 'lib/security-cookie'),
    (r'^_?CPort', 'lib/cportlib'),

    # --- PE / linker artefacts ----------------------------------
    (r'^___imp_|^__imp_|^___RUNTIME_PSEUDO_RELOC', 'PE/imptab'),
    (r'^_KiSystemStartup|^_NxBoot|^_KiSystemStartupBootStack'
     r'|^__init_array_start|^__init_array_end|^__bss_start'
     r'|^__data_start|^__rdata_start|^__rdata_end|^__rsrc_start'
     r'|^__rsrc_end|^__edata_start|^___mingw|^___DTOR_|^___CTOR_'
     r'|^___RUNTIME_', 'PE/runtime'),

    # --- big static data lumps ---------------------------------
    (r'^_NxkrnlNls|^_?Nls[A-Z]|^_?__NLS', 'data/nls-tables'),
    (r'^_KdpBreakpointTable|^_KiTimerTableListHead|^_NonPagedPoolDescriptor'
     r'|^_RotLineBuffer|^_ObsSecurityDescriptorCache|^_KiFreeze'
     r'|^_KdpMessageBuffer|^_KdpPathBuffer|^_KdPrintDefaultCircularBuffer'
     r'|^_HalpSavedIoMapData|^_KiBootStackData|^_P0BootStackData'
     r'|^_KiDoubleFaultStackData|^_XbeInterruptShadows'
     r'|^_NxSysVaTable|^_table_c[0-9a-f]+', 'data/kernel-tables'),
]

RULES = [(re.compile(rx), bucket) for rx, bucket in RULES]

SECTION_PREFIX = re.compile(
    r'^\.(text|data|rdata|bss|xdata|eh_frame)\$')

def classify(sym: str) -> str:
    # nm emits the section name itself (".text", "INIT", "PAGE", etc.) as
    # a symbol; these represent the padding/anonymous regions between real
    # symbols rather than content from a TU.
    if re.fullmatch(r'\.(text|rdata|data|xdata|edata|idata|eh_frame|bss)'
                    r'|INIT|PAGE|PAGECONS|PAGEDATA|INITDATA|\.rsrc',
                    sym):
        return 'anon/padding'
    # -ffunction-sections emits each function as `.text$FuncName` (and
    # -fdata-sections does `.data$Var`).  Try classifying both the cdecl/
    # stdcall form (`_Foo`) and the fastcall form (`@Foo`) -- rules use a
    # mix.  Original GCC/COFF symbol decorations:
    #   cdecl    -> `_Foo`
    #   stdcall  -> `_Foo@N`
    #   fastcall -> `@Foo@N`
    m = SECTION_PREFIX.match(sym)
    candidates = [sym]
    if m:
        bare = sym[m.end():]
        candidates = ['_' + bare, '@' + bare]
    for cand in candidates:
        for pat, bucket in RULES:
            if pat.search(cand):
                return bucket
    return f'UNCLASSIFIED {sym}'


def read_pe_sections(path):
    """(image_base, [(rva_start, rva_end, name, discardable, exec), ...])."""
    d = open(path, 'rb').read()
    e = int.from_bytes(d[0x3c:0x40], 'little')
    nsec = int.from_bytes(d[e+6:e+8], 'little')
    optsz = int.from_bytes(d[e+20:e+22], 'little')
    image_base = int.from_bytes(d[e+24+28:e+24+32], 'little')
    secoff = e + 24 + optsz
    secs = []
    for i in range(nsec):
        off = secoff + i * 40
        name = d[off:off+8].rstrip(b'\0').decode('latin-1')
        vsize = int.from_bytes(d[off+8:off+12], 'little')
        vaddr = int.from_bytes(d[off+12:off+16], 'little')
        chars = int.from_bytes(d[off+36:off+40], 'little')
        secs.append((vaddr, vaddr + max(vsize, 1), name,
                     bool(chars & 0x02000000),     # IMAGE_SCN_MEM_DISCARDABLE
                     bool(chars & 0x20000000)))    # IMAGE_SCN_MEM_EXECUTE
    secs.sort()
    return image_base, secs


def section_of(va, image_base, secs):
    """Map an absolute symbol VA to (section_name, discardable, executable)."""
    rva = va - image_base
    for vaddr, vend, name, disc, ex in secs:
        if vaddr <= rva < vend:
            return name, disc, ex
    return '?', False, False


def load_cov_hits(cov_logs, sym_ranges):
    """Mark symbols reached by any basic block in the trace(s).

    sym_ranges is a sorted list of (va_start, va_end, key); returns the
    set of keys whose [start,end) contains at least one traced TB start.
    """
    from bisect import bisect_right
    starts = [r[0] for r in sym_ranges]
    hits = set()
    addr_re = re.compile(rb"^0x([0-9a-fA-F]+):")
    for log in cov_logs:
        with open(log, 'rb') as f:
            for line in f:
                m = addr_re.match(line)
                if not m:
                    continue
                va = int(m.group(1), 16)
                i = bisect_right(starts, va) - 1
                if i >= 0:
                    vs, ve, key = sym_ranges[i]
                    if vs <= va < ve:
                        hits.add(key)
    return hits


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # ---- args: [exe] [--bucket NAME] [--cov LOG[,LOG...]] [--resident] ----
    argv = sys.argv[1:]
    want_bucket = None
    cov_logs = []
    resident_only = False
    positional = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--bucket':
            want_bucket = argv[i + 1]; i += 2; continue
        if a == '--cov':
            cov_logs = [x for x in argv[i + 1].split(',') if x]; i += 2; continue
        if a == '--resident':
            resident_only = True; i += 1; continue
        if a in ('-h', '--help'):
            print('usage: kernel-size.py [EXE] [--resident] [--bucket NAME] '
                  '[--cov LOG[,LOG2]]')
            print('  --resident     count only lifetime-resident sections '
                  '(the .text-diet target)')
            print('  --bucket NAME  list every symbol in one bucket, '
                  'largest first')
            print('  --cov LOG      cross-reference an xemu in_asm trace '
                  '(run-xemu --trace);')
            print('                 flags resident bytes never executed = '
                  'prime cut candidates')
            return
        positional.append(a); i += 1

    candidates = [
        'build/ntoskrnl/xboxkrnl.unstripped.exe',
        'build-dbg/ntoskrnl/xboxkrnl.unstripped.exe',
        'build/ntoskrnl/xboxkrnl.exe',
        'build-dbg/ntoskrnl/xboxkrnl.exe',
    ]
    exe = positional[0] if positional else None
    if exe is None:
        for c in candidates:
            p = os.path.join(repo_root, c)
            if os.path.exists(p):
                exe = p; break
    if exe is None:
        sys.exit('no kernel binary found; build first or pass a path explicitly')
    if not os.path.exists(exe):
        sys.exit(f'no such file: {exe}')

    # Section flags come from the shipped (stripped) sibling -- pefixup
    # re-applies IMAGE_SCN_MEM_DISCARDABLE to INIT there post-strip, and
    # it carries no DWARF sections to confuse the VA->section map.
    shipped = os.path.join(os.path.dirname(exe), 'xboxkrnl.exe')
    flags_exe = shipped if os.path.exists(shipped) else exe
    image_base, pe_secs = read_pe_sections(flags_exe)

    nm = os.environ.get('NM', 'i686-w64-mingw32-nm')
    out = subprocess.check_output(
        [nm, '--print-size', '--size-sort', exe],
        stderr=subprocess.DEVNULL,
    ).decode('latin-1', errors='replace').splitlines()

    # Per-address dedup of nm output (see loop body for the rationale).
    entries = {}
    kinds = {}
    for line in out:
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
        except ValueError:
            continue
        kind = parts[2]
        name = parts[3]
        if kind == 'C':            # GC'd-function phantoms (section 0)
            continue
        if name.startswith('.debug_'):   # sidecar-only DWARF
            continue
        entries.setdefault((addr, size), name)
        if not name.startswith(('.text$', '.data$', '.rdata$', '.bss$',
                                '.xdata$', '.eh_frame$', 'INIT$', 'PAGE$')):
            entries[(addr, size)] = name
        kinds[(addr, size)] = kind

    # Optional coverage cross-reference.
    hits = set()
    if cov_logs:
        sym_ranges = sorted((addr, addr + size, (addr, size))
                            for (addr, size) in entries)
        hits = load_cov_hits(cov_logs, sym_ranges)

    # Bucket each deduped (addr, size): total / resident / dead-resident.
    # "resident" = lands in a non-discardable section (lifetime-mapped at
    # the retail base).  "dead-resident" = resident *executable* code never
    # executed in the trace -- the prime .text-diet candidates.  Data
    # (.rdata/.data/.bss) is excluded from dead-resident: a coverage trace
    # can't "execute" data, so counting it would falsely flag every table.
    totals = {}        # bucket -> [total, resident, dead_resident]
    unclass = []       # (size, name, resident, hit)
    bucket_syms = {}   # bucket -> [(size, name, section, resident, hit, exe)]
    for (addr, size), name in entries.items():
        sec_name, disc, is_exe = section_of(addr, image_base, pe_secs)
        resident = not disc
        hit = (addr, size) in hits if cov_logs else None
        bucket = classify(name)
        if bucket.startswith('UNCLASSIFIED '):
            unclass.append((size, name, resident, hit))
            bucket = 'UNCLASSIFIED (one-off symbols)'
        rec = totals.setdefault(bucket, [0, 0, 0])
        rec[0] += size
        if resident:
            rec[1] += size
            if cov_logs and is_exe and not hit:
                rec[2] += size
        bucket_syms.setdefault(bucket, []).append(
            (size, name, sec_name, resident, hit, is_exe))

    # ---- drill-down: one bucket's symbols, largest first ----------------
    if want_bucket is not None:
        match = [b for b in bucket_syms if want_bucket in b]
        if not match:
            sys.exit(f'no bucket matching "{want_bucket}"; try one of:\n  '
                     + '\n  '.join(sorted(bucket_syms)))
        for b in sorted(match):
            syms = sorted(bucket_syms[b], reverse=True)
            tot = sum(x[0] for x in syms)
            res = sum(x[0] for x in syms if x[3])
            dead = sum(x[0] for x in syms if x[3] and x[5] and x[4] is False)
            print(f'{b}: {tot:,} bytes total, {res:,} resident'
                  + (f', {dead:,} dead-resident code' if cov_logs else ''))
            hdr = f'  {"size":>9}  {"section":<8} {"res":>3}'
            if cov_logs:
                hdr += f' {"hit":>3}'
            print(hdr + '  symbol')
            for size, name, sec, resident, hit, is_exe in syms:
                row = (f'  {size:>9,}  {sec:<8} '
                       f'{"yes" if resident else " - ":>3}')
                if cov_logs:
                    row += f' {("yes" if hit else ("NO" if is_exe else " - ")):>3}'
                print(row + f'  {name}')
        return

    # ---- PE section table ----------------------------------------------
    objdump = os.environ.get('OBJDUMP', 'i686-w64-mingw32-objdump')
    sec_out = subprocess.check_output([objdump, '-h', exe],
                                      stderr=subprocess.DEVNULL).decode().splitlines()
    section_sizes = []
    for line in sec_out:
        m = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)', line)
        if m and m.group(1) not in {'Idx', 'CONTENTS'}:
            section_sizes.append((m.group(1), int(m.group(2), 16)))
    file_size = os.path.getsize(exe)

    # ---- runtime footprint vs retail budget ----------------------------
    RETAIL_BUDGET = 0x51000
    resident_pages = 0x1000  # PE header page
    discarded = 0
    resident_rows = []
    for vaddr, vend, sname, disc, ex in pe_secs:
        vsize = vend - vaddr
        pages = (vsize + 0xfff) & ~0xfff
        if disc:
            discarded += pages
        else:
            resident_pages += pages
            resident_rows.append((sname, pages))

    print(f'kernel-size.py: {exe}')
    print(f'file on disk (unstripped): {file_size:>12,} bytes')
    if shipped != exe and os.path.exists(shipped):
        shipped_size = os.path.getsize(shipped)
        print(f'  (shipped post-strip:     {shipped_size:>10,} bytes, '
              f'-{file_size - shipped_size:,} from COFF symtab + leftover DWARF)')
    if cov_logs:
        print(f'coverage: {len(cov_logs)} trace(s), '
              f'{len(hits):,} symbols reached')
    print()
    print('PE section table (VirtualSize column)')
    print('-------------------------------------')
    section_total = 0
    bss_total = 0
    for name, size in section_sizes:
        marker = ''
        if name == '.bss':
            marker = ' (zero-fill at load, NOT stored in file)'
            bss_total += size
        print(f'  {name:<14} {size:>10,}{marker}')
        section_total += size
    print(f'  {"sum":<14} {section_total:>10,}')
    on_disk_content = section_total - bss_total
    print(f'  {"on-disk content (sum - bss)":<29} {on_disk_content:>10,}')
    print(f'  {"PE hdr + raw-data padding":<29} {file_size - on_disk_content:>10,}'
          ' (SizeOfRawData rounded up to FileAlignment=0x1000)')
    print()
    print('runtime footprint vs retail budget')
    print('----------------------------------')
    print('  resident after MiFreeInitializationCode, page-rounded, '
          'incl. 1 header page:')
    for sname, pages in resident_rows:
        print(f'    {sname:<10} {pages:>10,}')
    print(f'  {"resident total":<25} {resident_pages:>10,}')
    print(f'  {"discarded (INIT et al)":<25} {discarded:>10,}')
    print(f'  {"retail budget (0x51000)":<25} {RETAIL_BUDGET:>10,}')
    over = resident_pages - RETAIL_BUDGET
    if over > 0:
        print(f'  {"OVER BUDGET by":<25} {over:>10,}  '
              f'({over/1024.0:.0f} KB to eliminate or move to INIT)')
    else:
        print(f'  {"headroom":<25} {-over:>10,}')
    print()

    # ---- content by source ---------------------------------------------
    # Sort by the column that matters: dead-resident if a trace was given
    # (what to cut first), else resident (the diet target), else total.
    sortkey = 2 if cov_logs else 1
    title = ('content by source -- sorted by '
             + ('DEAD-RESIDENT (never executed; cut these first)'
                if cov_logs else 'RESIDENT bytes'))
    print(title)
    print('-' * len(title))
    col = f'  {"total":>10} {"resident":>10}'
    if cov_logs:
        col += f' {"dead-res":>10}'
    print(col + '  bucket')
    grand = [0, 0, 0]
    rows = sorted(totals.items(), key=lambda x: -x[1][sortkey])
    for bucket, (t, r, d) in rows:
        if resident_only and r == 0:
            continue
        grand[0] += t; grand[1] += r; grand[2] += d
        line = f'  {t:>10,} {r:>10,}'
        if cov_logs:
            line += f' {d:>10,}'
        print(line + f'  {bucket}')
    line = f'  {grand[0]:>10,} {grand[1]:>10,}'
    if cov_logs:
        line += f' {grand[2]:>10,}'
    print(line + '  TOTAL')

    if cov_logs:
        # The global cut list: biggest resident-but-never-run functions.
        print()
        print('top dead-resident functions (resident code, never executed in trace)')
        print('--------------------------------------------------------------------')
        def _dead(addr, size):
            name_sec, disc, ex = section_of(addr, image_base, pe_secs)
            return (not disc) and ex and (addr, size) not in hits
        dead = sorted(((size, name) for (addr, size), name in entries.items()
                       if _dead(addr, size)), reverse=True)
        for size, name in dead[:30]:
            print(f'  {size:>9,}  {name}')
        print(f'  ({len(dead):,} dead-resident symbols, '
              f'{sum(s for s, _ in dead):,} bytes total)')

    if unclass:
        print()
        print('top UNCLASSIFIED symbols (add a rule to put them in a bucket)')
        print('-------------------------------------------------------------')
        for sz, name, _r, _h in sorted(unclass, reverse=True)[:20]:
            print(f'  {sz:>10,}  {name}')


if __name__ == '__main__':
    main()
