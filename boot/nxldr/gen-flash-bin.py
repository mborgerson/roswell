#!/usr/bin/env python3
#
# gen-flash-bin.py -- inject an XZ-compressed xboxkrnl.exe payload into
# the nxldr.bin produced by boot/nxldr (256 KB release, 512 KB DBG).
#
# The linker (boot/nxldr/nxldr.ld) lays out a complete flash image:
#   - .low_rom (MCPX magic + Xcodes + post-Xcode entry) at file offset 0
#   - hole filled with 0xff for the kernel payload
#   - .text/.rodata/.data source (gets ramcopied to RAM at boot)
#   - .reset_main + .reset_vector in the top 512 bytes (file offsets
#     FLASH-0x200 + FLASH-0x10; .reset_vector lands at the CPU reset vector
#     0xfffffff0 via the last flash mirror)
#
# The hole's offset + max size are exported as linker symbols
# (KernelPayloadOffset and KernelPayloadMaxSize).  We read them via
# `nm` so the slot stays a single source of truth in nxldr.ld.  The
# compressed-kernel layout we write at that offset is:
#
#   { u32 magic == NXKRNL_XZ_MAGIC; u32 csize; u32 dsize; u8 xz[csize]; }
#
# matching the structure nxldr's loader.c expects.
#
import argparse, os, struct, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NXKRNL_XZ_MAGIC = 0x5a4b584e  # 'NXKZ' (little-endian on disk)
NM = os.environ.get("NM", "nm")


def read_linker_symbols(elf_path, names):
    """Return {name: integer_value} for the given linker symbols."""
    out = subprocess.check_output([NM, "--no-sort", elf_path],
                                  stderr=subprocess.DEVNULL).decode()
    want = dict.fromkeys(names)  # type: dict[str, int | None]
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        val, _type, sym = parts
        if sym in want:
            try:
                want[sym] = int(val, 16)
            except ValueError:
                pass
    missing = [n for n, v in want.items() if v is None]
    if missing:
        sys.exit("gen-flash-bin.py: missing symbols in %s: %s" %
                 (elf_path, ", ".join(missing)))
    return {n: v for n, v in want.items() if v is not None}


def xz_compress(src_bytes: bytes) -> bytes:
    """Compress with the x86 BCJ filter + LZMA2 at max preset into a
    complete xz container; nxldr's vendored XZ Embedded decoder
    (boot/nxldr/xz/) parses the container in single-call mode.  crc32 is
    the only integrity check that decoder compiles in."""
    result = subprocess.run(
        ["xz", "--x86", "--lzma2=preset=9e", "--check=crc32", "-T", "1", "-c"],
        input=src_bytes,
        capture_output=True,
        check=True,
    )
    framed = result.stdout
    # xz stream magic
    if framed[:6] != b"\xfd7zXZ\x00":
        sys.exit("gen-flash-bin.py: unexpected xz magic %r" % framed[:6])
    return framed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "build/flash.bin"))
    ap.add_argument("--nxldr",
                    help="path to nxldr.bin (default: build/boot/nxldr/nxldr.bin "
                         "under whichever build tree was last touched)")
    ap.add_argument("--elf",
                    help="path to nxldr.elf (for reading payload symbols; "
                         "default: derived from --nxldr)")
    ap.add_argument("--kernel",
                    help="path to xboxkrnl.exe; omit for a loader-only image "
                         "with the payload slot left as 0xff fill")
    args = ap.parse_args()

    if args.nxldr is None:
        candidates = [
            os.path.join(REPO, "build-dbg/boot/nxldr/nxldr.bin"),
            os.path.join(REPO, "build/boot/nxldr/nxldr.bin"),
        ]
        existing = [p for p in candidates if os.path.exists(p)]
        if not existing:
            sys.exit("gen-flash-bin.py: no nxldr.bin found; build the "
                     "'startup' target first or pass --nxldr PATH "
                     "(checked: %s)" % ", ".join(candidates))
        args.nxldr = max(existing, key=os.path.getmtime)

    if args.elf is None:
        args.elf = args.nxldr.rsplit(".", 1)[0] + ".elf"

    for p in (args.nxldr, args.elf):
        if not os.path.exists(p):
            sys.exit("gen-flash-bin.py: missing input %s" % p)
    if args.kernel and not os.path.exists(args.kernel):
        sys.exit("gen-flash-bin.py: missing --kernel %s" % args.kernel)

    syms = read_linker_symbols(args.elf, [
        "KernelPayloadOffset",
        "KernelPayloadMaxSize",
    ])
    payload_offset = syms["KernelPayloadOffset"]
    payload_max_size = syms["KernelPayloadMaxSize"]

    # Erase a stale output first; we'd rather run-xemu fail loudly with
    # "missing required file" than silently boot an old image.
    try:
        os.remove(args.out)
    except FileNotFoundError:
        pass

    image = bytearray(open(args.nxldr, "rb").read())

    kernel_raw_size = 0
    blob = b""
    if args.kernel:
        kernel_raw = open(args.kernel, "rb").read()
        kernel_raw_size = len(kernel_raw)
        compressed = xz_compress(kernel_raw)
        blob = struct.pack("<III", NXKRNL_XZ_MAGIC,
                           len(compressed), kernel_raw_size) + compressed
        if len(blob) > payload_max_size:
            sys.exit("gen-flash-bin.py: xz(xboxkrnl) is %d bytes, "
                     "kernel payload slot is %d bytes "
                     "(raw=%d, xz=%d, header=12)" %
                     (len(blob), payload_max_size,
                      kernel_raw_size, len(compressed)))
        image[payload_offset:payload_offset + len(blob)] = blob

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)

    payload_free = payload_max_size - len(blob)
    out_rel = os.path.relpath(args.out)

    if blob:
        print("%s: kernel %d -> %d (xz %.1f%%), %d free in %d-byte slot (%.0f%%)"
              % (out_rel, kernel_raw_size, len(blob),
                 100.0 * len(blob) / kernel_raw_size,
                 payload_free, payload_max_size,
                 100.0 * payload_free / payload_max_size))
    else:
        print("%s: loader-only (%d-byte payload slot is 0xff fill)"
              % (out_rel, payload_max_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
