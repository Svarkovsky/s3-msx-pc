import os
import sys
import textwrap
import struct

try:
    from PIL import Image
except ImportError:
    print("\nОШИБКА: Для работы скрипта требуется библиотека Pillow!")
    print("Установите ее командой в терминале: python -m pip install Pillow\n")
    sys.exit(1)

def delta_encode(data, stride):
    """Дельта-XOR кодирование (аналог C-версии)"""
    size = len(data)
    if size < stride or stride <= 0: return
    for i in range(size - 1, stride - 1, -1):
        data[i] ^= data[i - stride]
    for i in range(size - 1, 0, -1):
        if i % stride != 0:
            data[i] ^= data[i - 1]

def match_cost(l, off, is_r):
    """Оценка стоимости токена LZ77"""
    if is_r:
        if l < 3: return 999
        return 1 if l <= 18 else 4
    if off <= 255:
        if l < 3: return 999
        return 2 if l <= 66 else 5
    if l < 4: return 999
    return 3 if l <= 35 else 6

def flush_lits(w, lit_start, i, data):
    """Запись накопленных литералов"""
    ll = i - lit_start
    while ll > 0:
        chunk = min(ll, 65535 + 137)
        if chunk <= 128:
            w.append(chunk - 1)
        elif chunk <= 136:
            w.append(240 | (chunk - 129))
        else:
            w.append(248); w.append(0)
            ext = chunk - 137
            w.append(ext & 0xFF); w.append((ext >> 8) & 0xFF)
        w.extend(data[lit_start : lit_start + chunk])
        lit_start += chunk
        ll -= chunk
    return lit_start

def stlz_compress(data):
    """Алгоритм сжатия STLZ-LZ77"""
    out = bytearray()
    i = 0
    lit_start = 0
    last_offset = 0
    length = len(data)
    hash_table = {}

    while i < length:
        best_len, best_off, is_rep = 0, 0, False

        if i + 3 <= length:
            h = tuple(data[i:i+3])
            if h in hash_table:
                for prev in hash_table[h]:
                    off = i - prev
                    if off > 65535: continue
                    mlen = 0
                    max_match = min(length - i, 65535)
                    while mlen < max_match and data[i+mlen] == data[prev+mlen]:
                        mlen += 1
                    if mlen > best_len and mlen >= (3 if off <= 255 else 4):
                        best_len = mlen
                        best_off = off

        if last_offset > 0 and i >= last_offset:
            mlen = 0
            max_match = min(length - i, 65535)
            while mlen < max_match and data[i+mlen] == data[i-last_offset+mlen]:
                mlen += 1
            if mlen >= 3:
                c_rep = match_cost(mlen, last_offset, True)
                c_best = match_cost(best_len, best_off, False) if best_len >= 3 else 999
                if (mlen - c_rep) > (best_len - c_best):
                    best_len = mlen
                    best_off = last_offset
                    is_rep = True

        if best_len >= 3:
            lit_start = flush_lits(out, lit_start, i, data)
            if is_rep:
                if best_len <= 18:
                    out.append(0xE0 | (best_len - 3))
                else:
                    out.append(248); out.append(2)
                    out.append(best_len & 0xFF); out.append((best_len >> 8) & 0xFF)
            elif best_off <= 255 and best_len <= 66:
                out.append(0x80 | (best_len - 3))
                out.append(best_off & 0xFF)
            elif best_len <= 35:
                out.append(0xC0 | (best_len - 4))
                out.append(best_off & 0xFF); out.append((best_off >> 8) & 0xFF)
            else:
                out.append(248); out.append(1)
                out.append(best_len & 0xFF); out.append((best_len >> 8) & 0xFF)
                out.append(best_off & 0xFF); out.append((best_off >> 8) & 0xFF)

            last_offset = best_off
            
            for j in range(min(best_len, 4)):
                if i + j + 3 <= length:
                    h = tuple(data[i+j : i+j+3])
                    if h not in hash_table: hash_table[h] = []
                    hash_table[h].insert(0, i+j)
                    if len(hash_table[h]) > 2: hash_table[h].pop()
            i += best_len
            lit_start = i
        else:
            if i + 3 <= length:
                h = tuple(data[i:i+3])
                if h not in hash_table: hash_table[h] = []
                hash_table[h].insert(0, i)
                if len(hash_table[h]) > 2: hash_table[h].pop()
            i += 1

    flush_lits(out, lit_start, i, data)
    return out

def convert_to_stlz(png_path):
    """Преобразование PNG в STLZ RGB565_LE"""
    img = Image.open(png_path).convert('RGB')
    width, height = img.size
    
    rgb565 = bytearray(width * height * 2)
    idx = 0
    for r, g, b in img.getdata():
        val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb565[idx] = val & 0xFF
        rgb565[idx+1] = (val >> 8) & 0xFF
        idx += 2
        
    stripe_h = 8
    num_stripes = (height + stripe_h - 1) // stripe_h
    fmt = 2  # RG_PIXEL_565_LE
    
    header = struct.pack("<4sHHBBB7s", b"STLZ", width, height, stripe_h, num_stripes, fmt, b'\0'*7)
    
    stripes_compressed = []
    print(f"> {png_path} ({width}x{height}) в STLZ...")
    for s in range(num_stripes):
        y_start = s * stripe_h
        y_end = min(y_start + stripe_h, height)
        actual_h = y_end - y_start
        
        start_idx = y_start * width * 2
        end_idx = y_end * width * 2
        stripe_data = bytearray(rgb565[start_idx:end_idx])
        
        if actual_h < stripe_h:
            stripe_data.extend(b'\0' * (width * 2 * (stripe_h - actual_h)))
            
        delta_encode(stripe_data, width * 2)
        stripes_compressed.append(stlz_compress(stripe_data))
        
    offsets = bytearray()
    current_offset = len(header) + num_stripes * 4
    for comp in stripes_compressed:
        offsets.extend(struct.pack("<I", current_offset))
        current_offset += len(comp)
        
    final_data = bytearray(header)
    final_data.extend(offsets)
    for comp in stripes_compressed:
        final_data.extend(comp)
        
    print(f"  -> {len(rgb565)} -> {len(final_data)} байт ({(len(final_data)/len(rgb565)*100):.1f}%)")
    return final_data

# ================== MAIN ==================
files = ["background_msx.png", "logo_msx.png"]
output = '#include "gui.h"\n\n'
refs = ""

script_dir = os.path.dirname(os.path.abspath(__file__))

for file in files:
    file_path = os.path.join(script_dir, file)
    if not os.path.exists(file_path):
        print(f"Предупреждение: {file} не найден, пропускаю")
        continue
    
    data = convert_to_stlz(file_path)
    
    struct_name = os.path.basename(file)[0:-4]
    
    hexdata = "".join(f"\\x{c:02X}" for c in data)

    output += 'static const binfile_t %s = {"%s", %d, {\n"%s"\n}\n};\n\n' % (
        struct_name,
        os.path.basename(file),
        len(data),
        '"\n"'.join(textwrap.wrap(hexdata, 100)),
    )
    refs += "&%s,\n" % struct_name

output += "\nconst binfile_t *builtin_images[] = {%s\n0\n};\n" % refs

output_path = os.path.join(script_dir, "images.c")
with open(output_path, "w", newline="") as f:
    f.write(output)

print(f"\n + {output_path}")
