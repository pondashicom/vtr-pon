"""Create a 20-to-0 countdown, with each number visible for one second."""
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent
base = Image.new('RGB', (1920, 1080), 'black')
d = ImageDraw.Draw(base)
d.rectangle((0, 0, 1919, 1079), outline='white', width=3)
d.line((959, 0, 959, 1079), fill='white', width=5)
d.line((0, 539, 1919, 539), fill='white', width=5)
d.ellipse((419, 0, 1498, 1079), outline='white', width=6)
for x, y in [(0, 0), (1652, 0), (0, 812), (1652, 812)]:
    d.ellipse((x, y, x+267, y+267), outline='white', width=3)
font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 940)
cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-n',
       '-f', 'rawvideo', '-pixel_format', 'rgb24', '-video_size', '1920x1080',
       '-framerate', '1', '-i', '-', '-vf',
       'fps=30,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,setsar=1',
       '-an', '-c:v', 'libx264', '-preset', 'medium', '-crf', '10',
       '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709',
       '-color_trc', 'bt709', '-movflags', '+faststart',
       str(root.parent / 'media' / 'Countdown_20_to_0_1080p30_21s.mp4')]
with subprocess.Popen(cmd, stdin=subprocess.PIPE) as process:
    for number in range(20, -1, -1):
        frame = base.copy()
        draw = ImageDraw.Draw(frame)
        label = str(number)
        box = draw.textbbox((0, 0), label, font=font)
        x = (1920-(box[2]-box[0]))/2-box[0]
        y = (1080-(box[3]-box[1]))/2-box[1]
        draw.text((x, y), label, font=font, fill='white')
        process.stdin.write(frame.tobytes())
    process.stdin.close()
    if process.wait() != 0:
        raise RuntimeError('Countdown encoding failed')
