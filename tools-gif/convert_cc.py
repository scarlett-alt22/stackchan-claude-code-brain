"""cc-gifs 场景 GIF → StackChan CoreS3 320x240 全屏 GIF。

- 所有选中场景先求全批联合 bbox，再共用一个缩放比例，避免逐片撑满屏造成体型跳变
- 与 clawd-on-desk 转换保持相同的 12px 底边锚定和奶白背景
- 按场景语义直接落到 SD-staging 的场景目录

加词后必须把本脚本中的全部选中场景一起重转；如果联合 bbox 改变，新旧素材不能混用。

★ OWNER 2026-07-26: 原来只有一个平铺词表、产物全丢 converted/ 再手动分发。
  改成 词 → 场景 的映射，跑完直接就位，避免"转好了忘了分到哪个目录"。
"""
from PIL import Image, ImageSequence
import os

CREAM = (250, 243, 224)
W, H = 320, 240
BOTTOM_MARGIN = 12
MARGIN = 6
SD = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                  '..', 'SD-staging', 'app', 'AiStackChanEx', 'gif')

# 词 -> 场景。挑选依据是**语义对得上那个场景在干什么**，不是好看:
#   thinking = 等 LLM 回话时的沉思/推演   idle = 没人理它时的磨蹭游荡
#   talking  = 正在念回复时的说话姿态     happy = 高兴
SCENES = {
    'thinking': [
        'Baking', 'Brewing', 'Canoodling',      # 07-11 就在用的三个，保持不变
        'Clauding',                              # 进入 Claude 模式中 —— 它本人
        'Cogitating', 'Pondering', 'Mulling', 'Ruminating',
        'Puzzling', 'Deciphering',
        'Kneading',                              # 揉面中 —— 对应 pet/04 那句台词
    ],
    'idle': [
        'Lollygagging', 'Moseying', 'Puttering', 'Dilly-dallying',
        'Wandering', 'Frolicking', 'Vibing',
    ],
    'talking': [
        'Bloviating', 'Flibbertigibbeting', 'Gesticulating', 'Pontificating',
    ],
    'happy': [
        'Razzle-dazzling', 'Manifesting',
    ],
    # 逆毛(被倒着撸)的反应。前两个是跟台词配对挑的:
    #   Discombobulating 搞乱中      <-> "毛都乱了。"
    #   Recombobulating  重新整顿中  <-> "摸完要把我发型变回去。"
    # 后三个是情绪档:生闷气 / 被搞懵 / 抖一激灵(跟 headNuzzle 逆毛那个抖动呼应)。
    'annoyed': [
        'Discombobulating', 'Recombobulating', 'Stewing', 'Flummoxing', 'Wibbling',
    ],
    # 摇晃。一条「散架 -> 重组」的弧,对应台词 "我要散架了。"
    #   Unravelling 解开中 / Combobulating 整理中 / Coalescing 聚合中
    #   Topsy-turvying 翻天覆地中 / Whirlpooling 漩涡旋转中
    # ⚠️ 这个目录里还有 stackchan-dizzy.gif —— 那是 OWNER 自己画的原创**橙色
    #    StackChan**,不是 Clawd。两者混在一个场景里会在角色之间随机跳,
    #    这跟"不加 calico/cloudling"是同一条理由。主人知情并保留了原创那个。
    'dizzy': [
        'Unravelling', 'Combobulating', 'Coalescing', 'Topsy-turvying', 'Whirlpooling',
    ],
}

# pass 1: 载入整批并计算联合 bbox。不能逐 GIF 适配，否则每次切片都会改变角色大小。
jobs = []
union = None
source_size = None
for scene, words in SCENES.items():
    for word in words:
        src = f'cc-gifs/generated/Clawd-{word}.gif'
        if not os.path.exists(src):
            print(f'  !! 缺素材 {src}')
            continue
        im = Image.open(src)
        dur = im.info.get('duration', 170)
        frames = [f.convert('RGBA') for f in ImageSequence.Iterator(im)]
        im.close()
        if not frames:
            print(f'  !! 空素材 {src}')
            continue
        if source_size is None:
            source_size = frames[0].size
        if any(fr.size != source_size for fr in frames):
            raise ValueError(f'{src}: 画布不是统一的 {source_size}')
        jobs.append((scene, word, frames, dur))
        for fr in frames:
            b = fr.getchannel('A').getbbox()
            if b:
                union = b if union is None else (
                    min(union[0], b[0]), min(union[1], b[1]),
                    max(union[2], b[2]), max(union[3], b[3]))

if not jobs or union is None:
    raise RuntimeError('没有可转换的 cc-gifs 素材')

x0, y0, x1, y1 = (
    max(0, union[0] - MARGIN), max(0, union[1] - MARGIN),
    min(source_size[0], union[2] + MARGIN),
    min(source_size[1], union[3] + MARGIN),
)
cw, ch = x1 - x0, y1 - y0
scale = min(W / cw, (H - BOTTOM_MARGIN) / ch)
sw, sh = round(cw * scale), round(ch * scale)
print(f'全批bbox: {union} 裁剪区: {cw}x{ch} 缩放: {scale:.3f} → {sw}x{sh}')

# pass 2: 所有场景共用同一个裁剪框、缩放比例和落脚线。
for scene, word, frames, dur in jobs:
    out_dir = os.path.join(SD, scene)
    os.makedirs(out_dir, exist_ok=True)
    out = []
    for fr in frames:
        canvas = Image.new('RGB', (W, H), CREAM)
        flat = Image.new('RGBA', fr.size, CREAM + (255,))
        flat.alpha_composite(fr)
        content = flat.crop((x0, y0, x1, y1)).resize((sw, sh), Image.NEAREST)
        canvas.paste(content.convert('RGB'), ((W - sw)//2, H - BOTTOM_MARGIN - sh))
        out.append(canvas.quantize(colors=64, dither=Image.Dither.NONE))
    outp = os.path.join(out_dir, f'cc-{word.lower()}.gif')
    out[0].save(outp, save_all=True, append_images=out[1:],
                duration=dur, loop=0, optimize=True)
    print(f'{scene:9s} {word:20s} {len(out)}帧 {os.path.getsize(outp)//1024}KB')
