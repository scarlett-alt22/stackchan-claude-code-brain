#!/bin/bash
# StackChan 设备侧 SD 写入稳定性 —— 换卡对照实验
#
# 背景: 2026-07-26 起 /voice_upload 约半数返回 500, 串口伴 ff_sd_status(),
#       恒定断在 4096 字节。已排除"卡在 Mac 上坏了"(60MB 写入 0 失败),
#       但从未换过**物理另一张卡**。本脚本就是补这个对照。
#
# 用法:
#   ./sd_write_trial.sh <设备IP> <标签> [轮次数]
#   例: ./sd_write_trial.sh YOUR_ROBOT_IP newcard 2
#
#   <标签> 用来区分两次运行(newcard / oldcard), 只影响输出文件名。
#   [轮次数] 每轮把 18 个语音文件全传一遍; 默认 1 轮 = 18 次请求。
#            想要 n>=20 就跑 2 轮 (36 次)。
#
# payload 刻意用**卡上已有的同名真实文件**: 内容逐字节相同, 传成功=原样覆盖,
# 不会污染语音池, 也跟 07-26 那批 "8成功/10失败" 的基线可直接比。

set -u

IP="${1:?用法: $0 <设备IP> <标签> [轮次数]}"
LABEL="${2:?用法: $0 <设备IP> <标签> [轮次数]}"
ROUNDS="${3:-1}"

# 改成你本机 SD-staging 里语音目录的实际路径
SRC="${SRC:-$PWD/SD-staging/app/AiStackChanEx/voice}"
OUT="$(dirname "$0")/sdtrial-${LABEL}-$(date +%Y%m%d-%H%M%S).csv"

if [ ! -d "$SRC" ]; then echo "❌ 找不到语音源目录: $SRC"; exit 1; fi

# 连通性先探一下, 别在半路才发现设备没上线
if ! curl -sS -m 5 -o /dev/null "http://${IP}/"; then
  echo "❌ 连不上 http://${IP}/ —— 设备没开机 / IP 不对 / 不在同一网段"
  exit 1
fi
echo "✅ 设备 http://${IP}/ 可达"

echo "idx,round,category,file,bytes,http_code,elapsed_s,result" > "$OUT"

idx=0; ok=0; fail=0
for r in $(seq 1 "$ROUNDS"); do
  for cat in pat pet pet_back shake; do
    for f in "$SRC/$cat"/*.wav; do
      [ -e "$f" ] || continue
      base="$(basename "$f")"
      sz=$(stat -f%z "$f")
      idx=$((idx+1))

      # 上传口用文件名编码目标: "<分类>-<文件名>"(不能用 query 参数, multipart
      # 回调里 server.arg() 时序不保证 —— 见 docs/JOURNEY.md「SD 卡」一节)
      read -r code elapsed <<<"$(curl -sS -m 30 -o /dev/null \
          -w '%{http_code} %{time_total}' \
          -F "file=@${f};filename=${cat}-${base}" \
          "http://${IP}/voice_upload" 2>/dev/null || echo "000 0")"

      if [ "$code" = "200" ]; then res=OK;   ok=$((ok+1))
      else                        res=FAIL; fail=$((fail+1)); fi

      printf '%d,%d,%s,%s,%d,%s,%s,%s\n' \
        "$idx" "$r" "$cat" "$base" "$sz" "$code" "$elapsed" "$res" >> "$OUT"
      printf '%3d  %-9s %-14s %6dB  %s  %ss  %s\n' \
        "$idx" "$cat" "$base" "$sz" "$code" "$elapsed" "$res"
      # 不加 sleep: 基线就是背靠背连传的(JOURNEY: "空闲一阵后的第一个总能写完,
      # 紧接着的开始失败"), 加间隔会把要复现的现象洗掉
    done
  done
done

total=$((ok+fail))
echo
echo "=========================================="
echo " 标签: $LABEL   共 $total 次   成功 $ok   失败 $fail"
if [ "$total" -gt 0 ]; then
  echo " 失败率: $(awk -v f="$fail" -v t="$total" 'BEGIN{printf "%.1f%%", 100*f/t}')"
fi
echo " 对照基线(旧卡 2026-07-26): 18 次中 10 失败 = 55.6%"
echo " 明细: $OUT"
echo "=========================================="
echo
echo "⚠️ 单次运行只是一个样本。判读前先看 docs/JOURNEY.md「SD 卡」一节那条教训:"
echo "   别拿小样本宣布元凶。新旧卡各跑一遍、条件尽量一致, 才算对照。"
