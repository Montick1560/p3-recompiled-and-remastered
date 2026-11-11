"""Achievement images: user PNGs -> 512x512 JPEGs in games/patapon3/assets/achievements/.

Usage: python -I achievement_images.py <png_dir>
Each PNG is named <achievement id>.png, square, subject inside the central 80 % circle.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
CATALOGUE = HERE.parent / "achievements.json"
OUT_DIR = HERE.parent / "assets" / "achievements"
BACKGROUND = (0x24, 0x18, 0x12)
CREAM = (0xFC, 0xEB, 0xD4)
SIZE = 512


def _save(im: Image.Image, dst: Path) -> Path:
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        flat = Image.new("RGB", im.size, BACKGROUND)
        flat.paste(im, mask=im.getchannel("A"))
        im = flat
    im = im.convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
    dst.parent.mkdir(parents=True, exist_ok=True)
    im.save(dst, "JPEG", quality=85, optimize=True, progressive=False)
    return dst


def convert(src: Path, out_dir: Path, ids: set) -> Path:
    if src.stem not in ids:
        raise ValueError("unknown id")
    with Image.open(src) as im:
        if im.width != im.height:
            raise ValueError("not square")
        return _save(im, out_dir / f"{src.stem}.jpg")


def missing(ids: set, img_dir: Path) -> list:
    return sorted(i for i in ids if not (img_dir / f"{i}.jpg").exists())


def default_image() -> Image.Image:
    """A cream drum outline on the dark background: the stand-in for ids without an image."""
    im = Image.new("RGB", (SIZE, SIZE), BACKGROUND)
    d = ImageDraw.Draw(im)
    w = 10
    top, bottom, left, right = 150, 362, 136, 376
    d.ellipse((left, top - 40, right, top + 40), outline=CREAM, width=w)            # drum head
    d.line((left, top, left + 20, bottom), fill=CREAM, width=w)                      # sides
    d.line((right, top, right - 20, bottom), fill=CREAM, width=w)
    d.arc((left + 20, bottom - 34, right - 20, bottom + 34), 0, 180, fill=CREAM, width=w)
    for x in range(left + 40, right - 30, 50):                                      # lacing
        d.line((x, top + 38, x + 25, bottom + 30), fill=CREAM, width=w // 2)
    return im


def catalogue_ids() -> set:
    return {e["id"] for e in json.loads(CATALOGUE.read_text(encoding="utf-8"))["achievements"]}


def main(argv: list) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    ids = catalogue_ids()
    done = 0
    for src in sorted(Path(argv[1]).glob("*.png")):
        try:
            convert(src, OUT_DIR, ids)
            done += 1
        except (ValueError, OSError) as e:
            print(f"skip {src.name}: {e}")
    if not (OUT_DIR / "_default.jpg").exists():
        _save(default_image(), OUT_DIR / "_default.jpg")
    left = missing(ids, OUT_DIR)
    print(f"{done} converted, {len(left)} missing: {' '.join(left)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
