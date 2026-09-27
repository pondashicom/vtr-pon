"""Generate a geometric display-check chart; no VTR-PON2 assets used."""
from pathlib import Path
import colorsys
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent
im = Image.new('RGB', (1920, 1080), 'black')
d = ImageDraw.Draw(im)
font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 16)
for x in range(0, 1920, 120):
    d.line((x, 0, x, 1079), fill=(210, 210, 210))
for y in range(0, 1080, 120):
    d.line((0, y, 1919, y), fill=(210, 210, 210))
d.rectangle((0, 0, 1919, 1079), outline='white')
d.ellipse((420, 0, 1499, 1079), outline=(240, 155, 55))
for cx, cy in [(120, 120), (1800, 120), (120, 960), (1800, 960)]:
    d.ellipse((cx-120, cy-120, cx+119, cy+119), outline=(240, 155, 55))
    d.ellipse((cx-40, cy-40, cx+39, cy+39), fill='white')
# Clear the central panel; preserve the surrounding alignment grid.
d.rectangle((240, 240, 1679, 839), fill='black')
for i in range(12):
    rgb = tuple(round(v*255) for v in colorsys.hsv_to_rgb(i/12, 1, 1))
    d.rectangle((242+i*120, 242, 357+i*120, 357), fill=rgb)
for channel in range(3):
    for x in range(1440):
        t = x/1439
        rgb = [round(max(0, 2*t-1)*255)]*3
        rgb[channel] = round(min(1, 2*t)*255)
        d.line((240+x, 360+channel*80, 240+x, 439+channel*80), fill=tuple(rgb))
for row, levels in enumerate([list(range(1, 13)), [2,5,10,20,30,40,50,60,70,80,90,100]]):
    for i, level in enumerate(levels):
        value = round(255*level/100)
        x, y = 240+i*120, 600+row*120
        d.rectangle((x+2, y+2, x+117, y+117), fill=(value,)*3)
        d.text((x+60, y+60), f'{level}%', font=font,
               fill='white' if level < 50 else 'black', anchor='mm')
d.text((960, 900), '1920x1080   16:9', font=font, fill='white', anchor='mm')
im.save(root / 'colorchecker_source.png')
