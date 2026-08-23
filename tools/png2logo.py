#!/usr/bin/env python3
"""Convert an RGBA PNG into a 4bpp uncompressed BMP for the boot splash.

bootvid only blits 4bpp DIBs (16-colour palette, biSize==40, biClrUsed==16,
BI_RGB).  The boot logo is white artwork on a transparent background, so we
composite over black and map the result onto a 16-level grayscale ramp.  Index
0 is black, which matches the cleared screen behind the centred logo and lets
inbv's palette fade-in animate the logo up from black for free.

Stdlib only (no PIL): decodes 8-bit RGBA, non-interlaced PNG via zlib.

Usage: png2logo.py <input.png> <output.bmp>
"""

import struct
import sys
import zlib


def read_rgba_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")

    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + data + CRC
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break

    w, h, depth, color, _comp, _filt, interlace = ihdr
    if (depth, color, interlace) != (8, 6, 0):
        raise SystemExit(f"{path}: need 8-bit RGBA, non-interlaced (got "
                         f"depth={depth} color={color} interlace={interlace})")

    raw = zlib.decompress(idat)
    bpp, stride = 4, w * 4
    out, prev, p = bytearray(), bytearray(stride), 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            x = line[i]
            if f == 1:
                line[i] = (x + a) & 0xFF
            elif f == 2:
                line[i] = (x + b) & 0xFF
            elif f == 3:
                line[i] = (x + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (x + pr) & 0xFF
        out += line
        prev = line
    return w, h, bytes(out)


def to_4bpp_grayscale_bmp(w, h, rgba):
    # 16-entry grayscale ramp: index 0 = black ... 15 = white (steps of 17).
    palette = bytes()
    for i in range(16):
        v = i * 17
        palette += struct.pack("<BBBB", v, v, v, 0)  # BGRX

    # Composite white-on-transparent over black, then map luminance to the
    # ramp.  Pixels are effectively (255,255,255,alpha); intensity == alpha.
    def index_at(x, y):
        o = (y * w + x) * 4
        r, g, b, a = rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]
        lum = (r * 77 + g * 150 + b * 29) >> 8      # premultiply-free approx
        val = lum * a // 255                         # composite over black
        return (val * 15 + 127) // 255               # nearest ramp index

    stride = ((w * 4 + 31) // 32) * 4                # DWORD-aligned, 4bpp
    pixels = bytearray()
    for y in range(h - 1, -1, -1):                   # BMP rows are bottom-up
        row = bytearray(stride)
        for x in range(w):
            idx = index_at(x, y) & 0xF
            if x & 1:
                row[x >> 1] |= idx                   # low nibble = odd pixel
            else:
                row[x >> 1] |= idx << 4              # high nibble = even pixel
        pixels += row

    bih = struct.pack("<IiiHHIIiiII",
                      40,            # biSize
                      w, h,          # biWidth, biHeight
                      1, 4,          # biPlanes, biBitCount
                      0,             # biCompression = BI_RGB
                      len(pixels),   # biSizeImage
                      0, 0,          # pels-per-meter
                      16, 16)        # biClrUsed, biClrImportant
    offbits = 14 + len(bih) + len(palette)
    bfh = struct.pack("<2sIHHI", b"BM", offbits + len(pixels), 0, 0, offbits)
    return bfh + bih + palette + pixels


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <input.png> <output.bmp>")
    w, h, rgba = read_rgba_png(sys.argv[1])
    open(sys.argv[2], "wb").write(to_4bpp_grayscale_bmp(w, h, rgba))
    print(f"{sys.argv[2]}: {w}x{h} 4bpp ({16} colours)")


if __name__ == "__main__":
    main()
