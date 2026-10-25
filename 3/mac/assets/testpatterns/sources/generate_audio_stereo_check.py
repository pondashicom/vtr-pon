"""Create a three-minute 1 kHz L / R / L+R channel check (10 seconds each)."""

from pathlib import Path
import subprocess

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "media" / "Audio_Stereo_Check_1kHz_1080p30_180s.mp4"
LABELS = ("L", "R", "L+R")


def make_frame(label: str) -> Image.Image:
    frame = Image.new("RGB", (1920, 1080), "black")
    draw = ImageDraw.Draw(frame)
    for text, size, center_y, color in (
        ("AUDIO STEREO CHECK", 64, 150, "#b8b8b8"),
        (label, 440, 540, "white"),
        ("1 kHz  /  -18 dBFS peak", 54, 870, "#b8b8b8"),
        ("L  >  R  >  L+R     |     10 s each", 42, 958, "#b8b8b8"),
    ):
        font = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", size)
        left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
        draw.text(((1920 - (right - left)) / 2 - left,
                   center_y - (bottom - top) / 2 - top),
                  text, font=font, fill=color)
    return frame


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    # Sample numbers give exact 10-second switches and a repeating 48-sample
    # sine period. The inactive channel is zero; L+R carries identical signals.
    tone = "0.12589254117941673*sin(2*PI*mod(n,48)/48)"
    phase = "floor(mod(n,1440000)/480000)"
    audio = (f"aevalsrc=exprs='if(eq({phase},1),0,{tone})|"
             f"if(eq({phase},0),0,{tone})':s=48000:d=180:c=stereo")
    command = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-n",
        "-f", "rawvideo", "-pixel_format", "rgb24",
        "-video_size", "1920x1080", "-framerate", "1/10", "-i", "-",
        "-f", "lavfi", "-i", audio,
        "-map", "0:v:0", "-map", "1:a:0", "-t", "180",
        "-vf", "fps=30,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,setsar=1",
        "-c:v", "libx264", "-preset", "medium", "-crf", "16", "-threads", "4",
        "-color_range", "tv", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709",
        "-c:a", "aac", "-b:a", "320k", "-aac_ms", "0", "-aac_is", "0",
        "-movflags", "+faststart", str(OUTPUT),
    ]
    frames = [make_frame(label).tobytes() for label in LABELS]
    with subprocess.Popen(command, stdin=subprocess.PIPE) as process:
        try:
            for _ in range(6):
                for frame in frames:
                    process.stdin.write(frame)
        finally:
            process.stdin.close()
        if process.wait() != 0:
            raise RuntimeError("Audio stereo check encoding failed")

    # Preview the encoded video rather than the uncompressed drawing.
    previews = ROOT / "previews"
    previews.mkdir(parents=True, exist_ok=True)
    for index, suffix in enumerate(("l", "r", "lr")):
        subprocess.run([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-n",
            "-ss", str(index * 10 + 5), "-i", str(OUTPUT),
            "-frames:v", "1", "-update", "1",
            str(previews / f"audio_stereo_check_{suffix}_preview.png"),
        ], check=True)
    print(OUTPUT)


if __name__ == "__main__":
    main()
