"""Generates installer/retroflatbed.ico and assets/retroflatbed.png (a flatbed scanner, lid open).

    python tools/make_icon.py

Needs Pillow. Every size is rendered from the vector-style drawing below (small sizes use a
simplified variant) and packed into a multi-resolution .ico by hand.
"""
import io
import os
import struct

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
S = 1024


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def gradient(w, h, top, bottom):
    col = Image.new("RGBA", (1, h))
    for y in range(h):
        col.putpixel((0, y), lerp(top, bottom, y / (h - 1)) + (255,))
    return col.resize((w, h))


def rounded_mask(box, radius):
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius, fill=255)
    return m


def paste_shape(img, mask, fill):
    layer = fill if isinstance(fill, Image.Image) else Image.new("RGBA", (S, S), fill)
    img.paste(layer, (0, 0), mask)


def poly_mask(points):
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).polygon(points, fill=255)
    return m


def glow(img, mask, color, radius, alpha):
    g = Image.new("RGBA", (S, S), color)
    a = mask.filter(ImageFilter.GaussianBlur(radius)).point(lambda v: int(v * alpha))
    g.putalpha(a)
    img.alpha_composite(g)


def edge_x(y, y0, x0, y1, x1):
    return x0 + (x1 - x0) * (y - y0) / (y1 - y0)


def master(detail):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    tile = rounded_mask((24, 24, S - 24, S - 24), 230)
    paste_shape(img, tile, gradient(S, S, (0x2B, 0xA3, 0x94), (0x1B, 0x2F, 0x57)))
    shine = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    ImageDraw.Draw(shine).ellipse((-300, -700, S + 300, 330), fill=(255, 255, 255, 38))
    shine.putalpha(ImageChops.multiply(shine.getchannel("A"), tile))
    img.alpha_composite(shine)

    dy = 40  # vertical centring
    shadow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow).ellipse((150, 770 + dy, 874, 880 + dy), fill=255)
    glow(img, shadow, (0, 0, 0), 22, 0.55)

    # open lid (underside visible)
    lid = [(262, 335 + dy), (762, 335 + dy), (714, 150 + dy), (310, 150 + dy)]
    paste_shape(img, poly_mask(lid), gradient(S, S, (0xFF, 0xF6, 0xE4), (0xE8, 0xDA, 0xBE)))
    d = ImageDraw.Draw(img)
    d.line([lid[3], lid[2]], fill=(0xC9, 0xB8, 0x95, 255), width=10)

    # body top face
    top = [(236, 335 + dy), (788, 335 + dy), (900, 640 + dy), (124, 640 + dy)]
    paste_shape(img, poly_mask(top), gradient(S, S, (0xF7, 0xEE, 0xDA), (0xEA, 0xDD, 0xC0)))
    # front face
    front = rounded_mask((124, 640 + dy, 900, 770 + dy), 38)
    front.paste(0, (0, 0, S, 640 + dy - 20))
    front = ImageChops.lighter(front, poly_mask([(124, 640 + dy), (900, 640 + dy), (900, 700 + dy), (124, 700 + dy)]))
    paste_shape(img, front, gradient(S, S, (0xD9, 0xC8, 0xA2), (0xB9, 0xA6, 0x7D)))

    # glass
    gy0, gy1 = 380 + dy, 592 + dy
    gx = lambda y, a, b: (edge_x(y, 335 + dy, 236, 640 + dy, 124) + a, edge_x(y, 335 + dy, 788, 640 + dy, 900) - a)
    gl = [(gx(gy0, 52, 0)[0], gy0), (gx(gy0, 52, 0)[1], gy0), (gx(gy1, 52, 0)[1], gy1), (gx(gy1, 52, 0)[0], gy1)]
    paste_shape(img, poly_mask(gl), gradient(S, S, (0x16, 0x23, 0x3F), (0x2B, 0x4A, 0x7A)))

    if detail:
        # sheet of paper lying on the glass
        paper = [(398, 405 + dy), (640, 405 + dy), (708, 585 + dy), (336, 585 + dy)]
        paste_shape(img, poly_mask(paper), (0xFF, 0xFF, 0xFF, 255))
        for i, y in enumerate((440, 468, 496, 524, 552)):
            y += dy
            x0 = edge_x(y, 405 + dy, 398, 585 + dy, 336) + 26
            x1 = edge_x(y, 405 + dy, 640, 585 + dy, 708) - 26 - (40 if i == 4 else 0)
            d.line([(x0, y), (x1, y)], fill=(0xA9, 0xB4, 0xC6, 255), width=9)
    else:
        paper = [(398, 405 + dy), (640, 405 + dy), (708, 585 + dy), (336, 585 + dy)]
        paste_shape(img, poly_mask(paper), (0xFF, 0xFF, 0xFF, 255))

    # scan bar with glow
    by = 498 + dy
    bx0 = edge_x(by, 335 + dy, 236, 640 + dy, 124) + 40
    bx1 = edge_x(by, 335 + dy, 788, 640 + dy, 900) - 40
    bar = Image.new("L", (S, S), 0)
    ImageDraw.Draw(bar).rounded_rectangle((bx0, by - 15, bx1, by + 15), 15, fill=255)
    glow(img, bar, (0xFF, 0x8A, 0x1F), 26, 0.95)
    paste_shape(img, bar, (0xFF, 0x9E, 0x2C, 255))
    core = Image.new("L", (S, S), 0)
    ImageDraw.Draw(core).rounded_rectangle((bx0 + 10, by - 5, bx1 - 10, by + 5), 5, fill=255)
    paste_shape(img, core, (0xFF, 0xE9, 0xB8, 255))

    # retro stripes + LED on the front panel
    d = ImageDraw.Draw(img)
    fy = 700 + dy
    for i, c in enumerate(((0xFF, 0x6B, 0x35), (0xFF, 0xB0, 0x2E), (0x2B, 0xA3, 0x94))):
        d.rounded_rectangle((176 + i * 62, fy + 4, 222 + i * 62, fy + 24), 8, fill=c + (255,))
    d.ellipse((822, fy - 2, 852, fy + 28), fill=(0x4C, 0xE0, 0x7A, 255))

    return img


def render(size):
    m = master(detail=size >= 48)
    return m.resize((size, size), Image.LANCZOS)


def dib_entry(im):
    w, h = im.size
    px = im.convert("RGBA").tobytes("raw", "BGRA")
    rows = [px[y * w * 4:(y + 1) * w * 4] for y in range(h)][::-1]
    mask_stride = ((w + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    return header + b"".join(rows) + b"\x00" * (mask_stride * h)


def png_entry(im):
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def write_ico(path, images):
    blobs = [png_entry(im) if im.width >= 64 else dib_entry(im) for im in images]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        w = 0 if im.width >= 256 else im.width
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


if __name__ == "__main__":
    images = [render(s) for s in SIZES]
    write_ico(os.path.join(ROOT, "installer", "retroflatbed.ico"), images)
    os.makedirs(os.path.join(ROOT, "assets"), exist_ok=True)
    images[-1].save(os.path.join(ROOT, "assets", "retroflatbed.png"))
    sheet = Image.new("RGBA", (256 + 40 + 96 + 20 + 48 + 20 + 32 + 20 + 16 + 40, 300), (240, 240, 240, 255))
    x = 10
    for im in reversed(images):
        if im.width in (256, 96, 48, 32, 16):
            sheet.alpha_composite(im, (x, 10))
            x += im.width + 20
    sheet.save(os.path.join(ROOT, "assets", "_preview.png"))
    print("wrote icon with sizes", SIZES)
