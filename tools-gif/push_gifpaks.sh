#!/bin/bash
# 把 SD-staging 的 GIF 场景打成 pak 推给设备 SPIFFS(白脸治本, 2026-07-28)。
# 用法: push_gifpaks.sh [device_ip] [scene ...]     # 不带 scene = 全部 8 个
# 200 = 设备端已通过写完读回 FNV 校验(不是"收到了"是"内容对了")。
# 🔴 SPIFFS 写偏慢: idle(~580KB) 单场景要 3~12s, 期间设备触摸无响应, 属正常。
set -euo pipefail
IP="${1:-YOUR_ROBOT_IP}"
[ $# -gt 0 ] && shift
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../SD-staging/app/AiStackChanEx/gif"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SCENES=("$@")
[ ${#SCENES[@]} -eq 0 ] && SCENES=(idle listening talking thinking error happy dizzy annoyed)

for s in "${SCENES[@]}"; do
  python3 "$HERE/make_gifpak.py" "$SRC/$s" "$TMP/$s.pak"
  # 文件名 "<scene>.pak" 就是设备端定目标的依据, 别改名
  curl -sS --fail -X POST -F "file=@$TMP/$s.pak" "http://$IP/gifpak_upload" \
    && echo "  $s ✅"
done
echo "全部完成。设备端下一次挑片即生效, 不用重启。"
