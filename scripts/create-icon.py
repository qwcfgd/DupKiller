"""Regenerate the original application icon. Development dependency: Pillow."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SCALE = 4
im = Image.new("RGBA", (256 * SCALE, 256 * SCALE), (0, 0, 0, 0))
d = ImageDraw.Draw(im)

def box(coords, radius, fill):
    d.rounded_rectangle(tuple(int(v * SCALE) for v in coords), radius=radius * SCALE, fill=fill)

def poly(coords, fill):
    d.polygon([(int(x * SCALE), int(y * SCALE)) for x, y in coords], fill=fill)

box((14, 17, 242, 247), 51, "#0b493e")
box((14, 9, 242, 237), 51, "#13755d")
box((43, 46, 146, 174), 13, "#8ec4b1")
box((64, 61, 170, 194), 13, "#084f40")
poly([(77, 57), (147, 57), (182, 92), (182, 185), (77, 185)], "#f5fcf9")
poly([(147, 57), (147, 92), (182, 92)], "#b3d8cb")
box((94, 109, 161, 119), 5, "#85b9a7")
box((94, 131, 152, 141), 5, "#85b9a7")
box((135, 149, 216, 216), 18, "#0b493e")
box((135, 143, 216, 210), 18, "#f3c45b")
# A shortcut arrow redirects the second document to the retained original.
poly([(151, 190), (151, 169), (179, 169), (179, 157), (201, 177),
      (179, 197), (179, 184), (166, 184), (166, 190)], "#174b3b")
im = im.resize((256, 256), Image.Resampling.LANCZOS)
im.save(ROOT / "QtDupKiller.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
(ROOT / "test-output").mkdir(exist_ok=True)
im.save(ROOT / "test-output" / "icon-preview.png")
print("Created QtDupKiller.ico (16–256 px)")
