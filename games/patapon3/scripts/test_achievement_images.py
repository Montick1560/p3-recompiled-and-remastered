"""Self-check for the rarity tiers and achievement_images.py. Run: python -I test_achievement_images.py"""
import json
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from PIL import Image  # noqa: E402

import achievement_images as ai  # noqa: E402

CATALOGUE = HERE.parent / "achievements.json"


def test_catalogue_rarities():
    d = json.loads(CATALOGUE.read_text(encoding="utf-8"))
    r = d["rarities"]
    assert list(r) == ["comun", "raro", "especial", "epico", "legendario"]
    assert [r[k]["color"] for k in r] == ["#231815", "#7E3FC7", "#E5601E", "#B0177E", "#4E9A2B"]
    assert [r[k]["ms"] for k in r] == [4000, 4000, 4500, 5000, 6000]
    assert [r[k]["name"]["es"] for k in r] == ["Común", "Raro", "Especial", "Único", "Legendario"]
    assert [r[k]["name"]["en"] for k in r] == ["Common", "Rare", "Special", "Unique", "Legendary"]
    assert all(e["rarity"] in r for e in d["achievements"])
    assert Counter(e["rarity"] for e in d["achievements"]) == {
        "comun": 19, "raro": 34, "especial": 60, "epico": 18, "legendario": 12}


def test_convert():
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        src = t / "x.png"
        Image.new("RGBA", (1024, 1024), (200, 50, 50, 128)).save(src)
        out = ai.convert(src, t / "out", ids={"x"})
        assert out == t / "out" / "x.jpg"
        with Image.open(out) as im:
            assert im.size == (512, 512) and im.mode == "RGB" and im.format == "JPEG"
        assert out.stat().st_size < 120_000
        Image.new("RGB", (600, 400)).save(t / "x.png")
        try:
            ai.convert(t / "x.png", t / "out", ids={"x"})
            raise AssertionError("non-square accepted")
        except ValueError as e:
            assert str(e) == "not square"
        Image.new("RGB", (64, 64)).save(t / "nope.png")
        try:
            ai.convert(t / "nope.png", t / "out", ids={"x"})
            raise AssertionError("unknown id accepted")
        except ValueError as e:
            assert str(e) == "unknown id"


def test_report():
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        (t / "b.jpg").write_bytes(b"")
        assert ai.missing({"c", "a", "b"}, t) == ["a", "c"]


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
