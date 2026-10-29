# GIF 小剧场素材工具链 (OWNER/CC 2026-07-11)

把 clawd-on-desk 现成动画 / cc-gifs 生成动画转成 StackChan CoreS3 用的
320x240 奶白底 GIF。产物放 `SD-staging/app/AiStackChanEx/gif/<场景>/`，
拷到 SD 卡同路径生效。**素材不入 git**（artwork 版权归 Anthropic/原作者，
.gitignore 已挡），丢了按下面步骤重做。

## 重建/加新场景

```bash
python3 -m venv venv && ./venv/bin/pip install Pillow

# 路线A: clawd-on-desk 现成 GIF（24个可选: idle/thinking/error/happy/sweeping/…）
mkdir -p raw_gifs && cd raw_gifs
curl -sLO https://raw.githubusercontent.com/rullerzhou-afk/clawd-on-desk/main/assets/gif/clawd-<名字>.gif
cd .. && ./venv/bin/python3 convert_gifs.py   # 全局bbox+统一缩放,输出 converted/

# 路线B: cc-gifs 程序化生成（165个spinner场景，Baking/Brewing/…）
git clone --depth 1 https://github.com/crashchen/cc-gifs.git
./venv/bin/python3 cc-gifs/generate_clawd_gifs.py   # 全量生成到 cc-gifs/generated/
# 编辑 convert_cc.py 里的词表后:
./venv/bin/python3 convert_cc.py   # 所有选中场景共用全批bbox/缩放/底边锚点

# 改完后验证 320x240 / GIF89a / ≤64色 / ≤200KB / 帧时序未变
./venv/bin/python3 verify_gifs.py \
  --baseline ../rollback/gif-uniform-20260728-134155/gif

# 打包后还可逐场景核对 pak 索引、边界和内嵌 GIF 内容
./venv/bin/python3 verify_gifpak.py \
  ../SD-staging/app/AiStackChanEx/gif/idle /path/to/idle.pak
```

## 设备端约定（见 firmware/src/driver/GifPlayer.h）
- 场景目录: idle / listening / talking / thinking / error / happy，播放时目录内随机抽片
  (剧场模式 2026-07-11 v2: 屏幕常态=小剧场,橙脸只是无SD回退;
   idle=闲逛 listening=听主人说话 talking=念回复 thinking=等LLM)
- 加新片/挪场景=直接在场景目录间移动文件再拷SD，固件不用改不用重烧
- 帧规格: 320x240、RGB(奶白底250,243,224)、≤64色；文件≲200KB(整读进PSRAM)
- ⚠️ 两条路线都采用全批次统一缩放：加新片要把同一路线的**已有素材一起重转**
  (union bbox 变了会导致新旧体型不一致；bbox 没变时产物可直接比对确认)
- 两条路线都使用底部 12px 落脚线；cc-gifs 不再逐片各自撑满屏，避免与
  clawd-on-desk 素材切换时角色忽大忽小。

## 原创 StackChan 头晕场景

`generate_stackchan_dizzy.py` 画的是原创的橙色 StackChan 像素角色与旋转眼、星星，
仅参考 cc-gifs 的短循环和像素动效技法，不复用 Clawd 角色图。执行：

```bash
./venv/bin/python3 generate_stackchan_dizzy.py
```

它会生成 `../SD-staging/app/AiStackChanEx/gif/dizzy/stackchan-dizzy.gif`。将该目录同步到
设备 SD 卡的 `/app/AiStackChanEx/gif/dizzy/` 后，CoreS3 摇晃事件会播放一次该场景。
