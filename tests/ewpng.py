"""PNG reading and writing for the tests (Python 3 standard library only; Pillow is used when installed).

Pillow is used for decoding and resizing when it is installed (much faster); without it a pure Python
PNG decoder is used (8/16-bit gray, gray+alpha, RGB, RGBA and palette images, not interlaced).
Images are handled as (width, height, bytearray RGBA).
"""
import os, struct, zlib

SIG = b"\x89PNG\r\n\x1a\n"


class PngError(Exception):
    pass


# ---------------------------------------------------------------- reading

def png_size(path):
    """(width, height) from the IHDR chunk, without decoding. Raises PngError for a non-PNG file."""
    with open(path, "rb") as f:
        head = f.read(33)
    if len(head) < 33 or head[:8] != SIG or head[12:16] != b"IHDR":
        raise PngError(f"not a PNG file: {path}")
    return struct.unpack(">II", head[16:24])


def _chunks(data):
    pos = 8
    while pos + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if len(body) != n:
            raise PngError("truncated PNG chunk")
        yield kind, body
        pos += 12 + n
        if kind == b"IEND":
            return


def _unfilter(raw, h, stride, bpp):
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if len(line) != stride:
            raise PngError("truncated PNG image data")
        if ft == 0:
            pass
        elif ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        else:
            raise PngError(f"bad PNG filter type {ft}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def _read_png_pure(path):
    data = open(path, "rb").read()
    if data[:8] != SIG:
        raise PngError(f"not a PNG file: {path}")
    ihdr, idat, plte, trns = None, bytearray(), None, None
    for kind, body in _chunks(data):
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
    if not ihdr:
        raise PngError(f"PNG without IHDR: {path}")
    w, h, depth, ctype, _, _, interlace = ihdr
    if interlace:
        raise PngError(f"interlaced PNG is not supported without Pillow: {path}")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None or depth not in (1, 2, 4, 8, 16) or (depth < 8 and ctype not in (0, 3)):
        raise PngError(f"unsupported PNG format (color type {ctype}, bit depth {depth}): {path}")
    bits = channels * depth
    stride = (w * bits + 7) // 8
    px = _unfilter(zlib.decompress(bytes(idat)), h, stride, max(1, bits // 8))
    if depth == 16:  # keep the high byte of each sample
        px = px[0::2]
        depth = 8
        stride = w * channels
    if depth < 8:  # unpack 1/2/4-bit samples to one byte each
        un = bytearray(w * h)
        per = 8 // depth
        mask = (1 << depth) - 1
        for y in range(h):
            row = px[y * stride:(y + 1) * stride]
            for x in range(w):
                un[y * w + x] = (row[x // per] >> (8 - depth * (x % per + 1))) & mask
        if ctype == 0:
            scale = 255 // mask
            un = bytearray(v * scale for v in un)
        px = un
    n = w * h
    rgba = bytearray(n * 4)
    if ctype == 6:
        rgba[:] = px[:n * 4]
    elif ctype == 2:
        rgba[0::4], rgba[1::4], rgba[2::4] = px[0::3], px[1::3], px[2::3]
        rgba[3::4] = b"\xff" * n
    elif ctype == 0:
        rgba[0::4] = rgba[1::4] = rgba[2::4] = px[:n]
        rgba[3::4] = b"\xff" * n
    elif ctype == 4:
        rgba[0::4] = rgba[1::4] = rgba[2::4] = px[0::2]
        rgba[3::4] = px[1::2]
    else:  # palette
        if not plte:
            raise PngError(f"palette PNG without PLTE: {path}")
        pal = []
        alpha = trns or b""
        for i in range(len(plte) // 3):
            pal.append(bytes(plte[i * 3:i * 3 + 3]) + bytes([alpha[i] if i < len(alpha) else 255]))
        table = b"".join(pal) + b"\0\0\0\xff" * (256 - len(pal))
        for i, v in enumerate(px[:n]):
            rgba[i * 4:i * 4 + 4] = table[v * 4:v * 4 + 4]
    return w, h, rgba


def _pillow():
    if os.environ.get("EW_NO_PILLOW"):
        return None
    try:
        from PIL import Image  # noqa: F401
        return Image
    except Exception:
        return None


def read_png(path):
    """(width, height, bytearray RGBA). Raises PngError for a missing or unreadable file."""
    if not os.path.isfile(path):
        raise PngError(f"file not found: {path}")
    png_size(path)  # also rejects non-PNG files on the Pillow path
    Image = _pillow()
    if Image:
        try:
            with Image.open(path) as im:
                im = im.convert("RGBA")
                return im.width, im.height, bytearray(im.tobytes())
        except Exception as e:
            raise PngError(f"cannot read {path}: {e}")
    try:
        return _read_png_pure(path)
    except zlib.error as e:
        raise PngError(f"corrupt PNG data in {path}: {e}")


# ---------------------------------------------------------------- writing

def write_png(path, w, h, rgba):
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw += b"\0"
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    data = SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


# ---------------------------------------------------------------- image operations

def resize(img, nw, nh):
    w, h, px = img
    if (w, h) == (nw, nh):
        return img
    Image = _pillow()
    if Image:
        lanczos = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
        im = Image.frombytes("RGBA", (w, h), bytes(px)).resize((nw, nh), lanczos)
        return nw, nh, bytearray(im.tobytes())
    # Nearest neighbour, one row at a time (map() keeps the per-pixel work in C).
    cols = []
    for x in range(nw):
        sx = min(w - 1, x * w // nw) * 4
        cols.extend((sx, sx + 1, sx + 2, sx + 3))
    out = bytearray(nw * nh * 4)
    cache = {}
    for y in range(nh):
        sy = min(h - 1, y * h // nh)
        row = cache.get(sy)
        if row is None:
            src = px[sy * w * 4:(sy + 1) * w * 4]
            row = cache[sy] = bytes(map(src.__getitem__, cols))
            if len(cache) > 2:
                cache.pop(next(iter(cache)))
        out[y * nw * 4:(y + 1) * nw * 4] = row
    return nw, nh, out


def canvas(w, h, color):
    return w, h, bytearray(bytes(color) * (w * h))


def blit(dst, src, x0, y0):
    dw, dh, dp = dst
    sw, sh, sp = src
    for y in range(sh):
        if 0 <= y0 + y < dh:
            o = ((y0 + y) * dw + x0) * 4
            dp[o:o + sw * 4] = sp[y * sw * 4:(y + 1) * sw * 4]


def fill(dst, x0, y0, w, h, color):
    dw, _, dp = dst
    row = bytes(color) * w
    for y in range(y0, y0 + h):
        o = (y * dw + x0) * 4
        dp[o:o + w * 4] = row


