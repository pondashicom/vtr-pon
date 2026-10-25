"""Draw a 16:9 / centered 4:3 geometry test at square pixel resolution."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent
im = Image.new('RGB', (1920, 1080), 'black')
d = ImageDraw.Draw(im)
blue = (49, 87, 137)
cyan = (30, 168, 194)
font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 26)
d.rectangle((0, 0, 1919, 1079), outline=blue, width=6)
# A centered 1440 x 1080 region is exactly 4:3.
for x in (240, 1680):
    for y in range(0, 1080, 14):
        d.line((x, y, x, min(y+7, 1079)), fill=cyan, width=1)
# Equal width and height; no text inside the circle.
d.ellipse((690, 270, 1229, 809), outline=cyan, width=4)
def dimension(x1, x2, y, label):
    d.line((x1, y, x2, y), fill=blue, width=3)
    d.polygon([(x1, y), (x1+13, y-6), (x1+13, y+6)], fill=blue)
    d.polygon([(x2, y), (x2-13, y-6), (x2-13, y+6)], fill=blue)
    d.text((x1+65, y+14), label, font=font, fill=cyan)
dimension(240, 1680, 930, '4:3')
dimension(6, 1913, 1010, '16:9')
im.save(root / 'aspect_ratio_source.png')
