"""FHD safety guides with pixel rulers inside the top and left edges."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent
im = Image.new('RGBA', (1920, 1080), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
white = (255, 255, 255, 255)
font = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 16)
for box in [(96,54,1823,1025), (192,108,1727,971)]:
    d.rectangle(box, outline=white, width=2)
for x in range(0,1920,10):
    length = 18 if x % 100 == 0 else 12 if x % 50 == 0 else 6
    d.line((x,0,x,length), fill=white)
    if x % 100 == 0:
        d.text((max(2,min(x,1878)),21),str(x),font=font,fill=white)
for y in range(0,1080,10):
    length = 18 if y % 100 == 0 else 12 if y % 50 == 0 else 6
    d.line((0,y,length,y), fill=white)
    if y % 100 == 0 and y != 0:
        d.text((22,y),str(y),font=font,fill=white,anchor='lm')
# Label the canvas extent separately from its last pixel index.
d.text((1917,40),'1920 px',font=font,fill=white,anchor='ra')
d.text((22,1076),'1080 px',font=font,fill=white,anchor='ls')
im.save(root/'safety_area_source.png')
