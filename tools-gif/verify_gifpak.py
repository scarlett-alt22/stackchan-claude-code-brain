#!/usr/bin/env python3
"""核对 GPK1 pak 的索引、边界和每个内嵌 GIF 是否与源目录逐字节一致。"""
import struct
import sys
from pathlib import Path


def verify(scene_dir: Path, pak_path: Path) -> None:
    source = {
        path.name: path.read_bytes()
        for path in sorted(scene_dir.glob("*.gif"))
    }
    data = pak_path.read_bytes()
    if len(data) < 6 or data[:4] != b"GPK1":
        raise ValueError("不是 GPK1 pak")
    count = struct.unpack_from("<H", data, 4)[0]
    position = 6
    packed = {}
    for _ in range(count):
        if position >= len(data):
            raise ValueError("索引提前结束")
        name_length = data[position]
        position += 1
        if name_length == 0 or position + name_length + 8 > len(data):
            raise ValueError("文件名或索引长度越界")
        name = data[position:position + name_length].decode()
        position += name_length
        offset, length = struct.unpack_from("<II", data, position)
        position += 8
        if length == 0 or offset > len(data) or length > len(data) - offset:
            raise ValueError(f"{name}: 素材范围越界")
        if name in packed:
            raise ValueError(f"{name}: 索引重名")
        packed[name] = data[offset:offset + length]
    if source.keys() != packed.keys():
        raise ValueError(
            f"文件清单不一致：source={sorted(source)} pak={sorted(packed)}"
        )
    for name, source_data in source.items():
        if packed[name] != source_data:
            raise ValueError(f"{name}: pak 内容与源 GIF 不一致")
    print(f"{pak_path.name}: {count} 个 GIF，校验通过")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(f"用法: {Path(sys.argv[0]).name} <scene_dir> <pak>")
    verify(Path(sys.argv[1]), Path(sys.argv[2]))
