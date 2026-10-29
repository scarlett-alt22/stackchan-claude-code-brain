"""Generate a Clawd-style dizzy scene for the CoreS3 GIF theatre.

The animation uses the compact pixel-art timing and full-screen layout of the
local cc-gifs workflow, but draws an original crab with Clawd's blocky silhouette.
Output: ../SD-staging/app/AiStackChanEx/gif/dizzy/stackchan-dizzy.gif
"""
from pathlib import Path
import math

from PIL import Image, ImageDraw

W, H = 320, 240
CREAM = (250, 243, 224)
CLAWD = (221, 135, 114)
CLAWD_SHADOW = (190, 105, 89)
BLACK = (0, 0, 0)
WHITE = (250, 243, 224)
YELLOW = (255, 205, 48)
PURPLE = (123, 83, 178)

OUT = Path(__file__).parent.parent / "SD-staging/app/AiStackChanEx/gif/dizzy/stackchan-dizzy.gif"


def pixel_rect(draw, x, y, w, h, color):
    draw.rectangle((x, y, x + w - 1, y + h - 1), fill=color)


def spiral(draw, cx, cy, phase):
    points = [(0, -9), (7, -7), (9, 0), (6, 6), (0, 8), (-6, 6), (-7, 0), (-3, -3), (2, -3), (3, 1), (0, 3)]
    turn = phase % 4
    for i in range(len(points) - 1):
        x0, y0 = points[(i + turn) % len(points)]
        x1, y1 = points[(i + 1 + turn) % len(points)]
        draw.line((cx + x0, cy + y0, cx + x1, cy + y1), fill=BLACK, width=3)


def frame(index):
    wobble = (-8, -4, 1, 6, 8, 4, -1, -6)[index % 8]
    bob = (3, 1, -1, -3, -2, 0, 2, 3)[index % 8]
    im = Image.new("RGB", (W, H), CREAM)
    d = ImageDraw.Draw(im)

    # Big orbiting stars sell the dizzy turn rather than a static face swap.
    stars = [(79, 78), (228, 47), (258, 125), (207, 198), (63, 153)]
    for n, (x, y) in enumerate(stars):
        phase = (n * 2 + index) % 10
        angle = phase * 0.628
        sx = int(x + 10 * math.cos(angle))
        sy = int(y + 7 * math.sin(angle))
        s = 4 if n % 2 else 6
        pixel_rect(d, sx, sy, s, s, YELLOW)
        pixel_rect(d, sx - 3, sy + 3, s + 6, 2, YELLOW)
        pixel_rect(d, sx + 2, sy - 3, 2, s + 6, YELLOW)

    x, y = 110 + wobble, 86 + bob
    d.ellipse((109 + wobble, 195, 211 + wobble, 211), fill=(220, 204, 185))

    # Clawd's square crab body and its characteristic side claws.
    pixel_rect(d, x, y, 100, 69, CLAWD)
    pixel_rect(d, x + 10, y - 10, 80, 10, CLAWD)
    arm_y = y + (8 if index % 4 < 2 else 14)
    pixel_rect(d, x - 15, arm_y, 15, 22, CLAWD)
    pixel_rect(d, x - 23, arm_y + 5, 9, 12, CLAWD)
    pixel_rect(d, x + 100, arm_y, 15, 22, CLAWD)
    pixel_rect(d, x + 114, arm_y + 5, 9, 12, CLAWD)
    # Pincers open and close as the crab tries to regain balance.
    claw_shift = 5 if index % 4 < 2 else 0
    pixel_rect(d, x - 31, arm_y + 1 - claw_shift, 9, 9, CLAWD)
    pixel_rect(d, x - 31, arm_y + 15 + claw_shift, 9, 9, CLAWD)
    pixel_rect(d, x + 122, arm_y + 1 - claw_shift, 9, 9, CLAWD)
    pixel_rect(d, x + 122, arm_y + 15 + claw_shift, 9, 9, CLAWD)

    # Four visible legs alternate like the source Clawd idle, but staggered.
    leg_lift = 7 if index % 4 < 2 else 0
    for n, lx in enumerate((x + 12, x + 35, x + 62, x + 84)):
        lift = leg_lift if n % 2 == 0 else 7 - leg_lift
        pixel_rect(d, lx, y + 69, 10, 17 - lift, CLAWD)
        pixel_rect(d, lx - 4, y + 84 - lift, 18, 7, CLAWD_SHADOW)

    # Spiral eyes and a crooked mouth are the readable dizzy cue.
    spiral(d, x + 32, y + 31, index)
    spiral(d, x + 69, y + 31, index + 3)
    d.line((x + 43, y + 53, x + 61, y + 57), fill=BLACK, width=3)
    return im.quantize(colors=32, dither=Image.Dither.NONE)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    frames = [frame(i) for i in range(12)]
    frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=110,
                   loop=0, optimize=True, disposal=2)
    print(f"{OUT}: {len(frames)} frames, {OUT.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
