#!/usr/bin/env python3
"""把一个场景目录的 *.gif 打成设备端 GifPlayer 认的 GPK1 pak(白脸治本, 2026-07-28)。

用法: make_gifpak.py <scene_dir> <out.pak>

格式(小端, 与 firmware/src/driver/GifPlayer.cpp 的 pick_random_gif_pak 对齐):
  "GPK1" + u16 count
  + count 条 { u8 nameLen, name(不含\0), u32 off, u32 len }   # off 相对文件头
  + 素材体逐个拼接

约束: 单场景 ≤255 个文件(count 是设备端随机用的, 索引 u16 但白名单场景没这么多);
      文件名 <48 字节(设备端解析缓冲上限)。
"""
import struct
import sys
from pathlib import Path


def build(scene_dir: Path, out: Path) -> None:
    gifs = sorted(p for p in scene_dir.iterdir()
                  if p.is_file() and p.suffix.lower() == ".gif")
    if not gifs:
        sys.exit(f"{scene_dir}: 没有 .gif")
    if len(gifs) > 255:
        sys.exit(f"{scene_dir}: {len(gifs)} 个文件, 超过 255")
    for p in gifs:
        if len(p.name.encode()) >= 48:
            sys.exit(f"{p.name}: 文件名 ≥48 字节, 设备端解析不了")

    index_size = 6 + sum(1 + len(p.name.encode()) + 8 for p in gifs)
    entries, blobs = [], []
    off = index_size
    for p in gifs:
        data = p.read_bytes()
        name = p.name.encode()
        entries.append(struct.pack(f"<B{len(name)}sII", len(name), name, off, len(data)))
        blobs.append(data)
        off += len(data)

    out.write_bytes(b"GPK1" + struct.pack("<H", len(gifs))
                    + b"".join(entries) + b"".join(blobs))
    print(f"{out.name}: {len(gifs)} clips, {off} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(Path(sys.argv[1]), Path(sys.argv[2]))
