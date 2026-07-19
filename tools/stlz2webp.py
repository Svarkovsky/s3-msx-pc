#!/usr/bin/env python3
import struct, sys, os
from PIL import Image, ImageOps

# for f in *.png; do python3 stlz2webp.py "$f"; done          // .png

if len(sys.argv) < 2:
    print("Usage: stlz2webp.py <file.stlz> [output_file.webp]")
    sys.exit(1)

input_file = sys.argv[1]
if len(sys.argv) >= 3:
    output_file = sys.argv[2]
else:
    output_file = os.path.splitext(input_file)[0] + ".webp"

with open(input_file, "rb") as f:
    data = f.read()

magic, w, h, stripe_h, num_stripes, fmt = struct.unpack_from("<4sHHBBB", data)
if magic != b"STLZ":
    print("Error: not an STLZ file")
    sys.exit(1)

swap_bytes = (fmt == 2)  # RG_PIXEL_565_BE
offsets = struct.unpack_from(f"<{num_stripes}I", data, 18)
pixels = bytearray(w * h * 3)

for s in range(num_stripes):
    off = offsets[s]
    end = offsets[s+1] if s+1 < num_stripes else len(data)
    comp = data[off:end]
    
    dst = bytearray(w * stripe_h * 2)
    i, last_off = 0, 0
    rpos = 0
    
    while i < len(dst) and rpos < len(comp):
        cmd = comp[rpos]; rpos += 1
        
        if cmd <= 127:
            ln = cmd + 1
            dst[i:i+ln] = comp[rpos:rpos+ln]
            rpos += ln; i += ln
        elif cmd <= 191:
            mlen = (cmd & 0x3F) + 3
            off_b = comp[rpos]; rpos += 1
            src = i - off_b
            for j in range(mlen): dst[i+j] = dst[src+j]
            i += mlen; last_off = off_b
        elif cmd <= 223:
            mlen = (cmd & 0x1F) + 4
            off_b = comp[rpos] | (comp[rpos+1] << 8); rpos += 2
            src = i - off_b
            for j in range(mlen): dst[i+j] = dst[src+j]
            i += mlen; last_off = off_b
        elif cmd <= 239:
            mlen = (cmd & 0x0F) + 3
            src = i - last_off
            for j in range(mlen): dst[i+j] = dst[src+j]
            i += mlen
        elif cmd <= 247:
            ln = (cmd & 0x07) + 129
            dst[i:i+ln] = comp[rpos:rpos+ln]
            rpos += ln; i += ln
        elif cmd == 248:
            sub = comp[rpos]; rpos += 1
            if sub == 0:
                ln = (comp[rpos] | (comp[rpos+1] << 8)) + 137; rpos += 2
                dst[i:i+ln] = comp[rpos:rpos+ln]
                rpos += ln; i += ln
            elif sub == 1:
                mlen = comp[rpos] | (comp[rpos+1] << 8); rpos += 2
                off_b = comp[rpos] | (comp[rpos+1] << 8); rpos += 2
                src = i - off_b
                for j in range(mlen): dst[i+j] = dst[src+j]
                i += mlen; last_off = off_b
            elif sub == 2:
                mlen = comp[rpos] | (comp[rpos+1] << 8); rpos += 2
                src = i - last_off
                for j in range(mlen): dst[i+j] = dst[src+j]
                i += mlen
            elif sub == 3:
                mlen = comp[rpos] | (comp[rpos+1] << 8); rpos += 2
                off_b = comp[rpos]; rpos += 1
                src = i - off_b
                for j in range(mlen): dst[i+j] = dst[src+j]
                i += mlen; last_off = off_b

    # Delta decoding
    stride = w * 2
    for j in range(1, len(dst)):
        if j % stride != 0:
            dst[j] ^= dst[j-1]
    for j in range(stride, len(dst)):
        dst[j] ^= dst[j-stride]

    # BRG565 -> RGB888
    y_start = s * stripe_h
    y_end = min(y_start + stripe_h, h)
    for y in range(y_start, y_end):
        for x in range(w):
            idx = ((y - y_start) * w + x) * 2
            lo, hi = dst[idx], dst[idx+1]
            if swap_bytes:
                lo, hi = hi, lo
            val = lo | (hi << 8)
            
            b = int(((val >> 11) & 0x1F) * 255 / 31)
            r = int(((val >> 5) & 0x3F) * 255 / 63)
            g = int((val & 0x1F) * 255 / 31)
            
            p = (y * w + x) * 3
            pixels[p] = r
            pixels[p+1] = g
            pixels[p+2] = b

img = Image.frombytes("RGB", (w, h), bytes(pixels))

img = ImageOps.autocontrast(img, cutoff=0.1)

img.save(output_file, format="WEBP", lossless=True)

print(f" > {output_file}")
