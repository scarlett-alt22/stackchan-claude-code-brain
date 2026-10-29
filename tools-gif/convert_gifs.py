"""clawd GIF → StackChan CoreS3 320x240 全屏 GIF
- 全局联合bbox裁剪 + 统一NEAREST缩放（跨场景体型一致）
- 铺奶白底 (250,243,224)，与 StackChan 气泡同色
- 底部锚定（地平线一致），水平按内容中心
"""
from PIL import Image, ImageSequence
import os, sys

CREAM = (250, 243, 224)
W, H = 320, 240
BOTTOM_MARGIN = 12
MARGIN = 4

SRC = 'raw_gifs'
OUT = 'converted'

def frames_rgba(path):
    im = Image.open(path)
    dur = im.info.get('duration', 70)
    return [f.convert('RGBA') for f in ImageSequence.Iterator(im)], dur

# pass 1: 全局联合bbox
union = None
gifs = {}
for f in sorted(os.listdir(SRC)):
    frames, dur = frames_rgba(os.path.join(SRC, f))
    gifs[f] = (frames, dur)
    for fr in frames:
        b = fr.getchannel('A').getbbox()
        if b:
            union = b if union is None else (
                min(union[0], b[0]), min(union[1], b[1]),
                max(union[2], b[2]), max(union[3], b[3]))

x0, y0, x1, y1 = (union[0]-MARGIN, union[1]-MARGIN, union[2]+MARGIN, union[3]+MARGIN)
cw, ch = x1-x0, y1-y0
scale = min(W / cw, (H - BOTTOM_MARGIN) / ch)
sw, sh = int(cw*scale), int(ch*scale)
print(f"全局bbox: {union} 裁剪区: {cw}x{ch} 缩放: {scale:.3f} → {sw}x{sh}")

os.makedirs(OUT, exist_ok=True)
for name, (frames, dur) in gifs.items():
    out_frames = []
    for fr in frames:
        canvas = Image.new('RGB', (W, H), CREAM)
        flat = Image.new('RGBA', fr.size, CREAM + (255,))
        flat.alpha_composite(fr)
        content = flat.crop((x0, y0, x1, y1)).resize((sw, sh), Image.NEAREST)
        canvas.paste(content.convert('RGB'), ((W - sw)//2, H - BOTTOM_MARGIN - sh))
        out_frames.append(canvas.quantize(colors=64, dither=Image.Dither.NONE))
    outp = os.path.join(OUT, name)
    out_frames[0].save(outp, save_all=True, append_images=out_frames[1:],
                       duration=dur, loop=0, optimize=True)
    print(f"{name}: {len(out_frames)}帧 {os.path.getsize(outp)//1024}KB")
