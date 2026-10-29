#!/usr/bin/env python3
"""验证 StackChan GIF 小剧场素材，并可与变更前快照比较动画语义。

用法:
  ./venv/bin/python3 verify_gifs.py
  ./venv/bin/python3 verify_gifs.py --baseline ../rollback/<快照>/gif
"""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageSequence


CREAM = (250, 243, 224)
CANVAS = (320, 240)
MAX_COLORS = 64
MAX_FILE_BYTES = 200 * 1024
MAX_FILES_PER_SCENE = 24
DEFAULT_ROOT = (
    Path(__file__).resolve().parent
    / "../SD-staging/app/AiStackChanEx/gif"
).resolve()


def union_bbox(left, right):
    if left is None:
        return right
    if right is None:
        return left
    return (
        min(left[0], right[0]),
        min(left[1], right[1]),
        max(left[2], right[2]),
        max(left[3], right[3]),
    )


def inspect(path):
    background = Image.new("RGB", CANVAS, CREAM)
    durations = []
    visible = None
    with Image.open(path) as image:
        version = image.info.get("version")
        loop = image.info.get("loop")
        transparency = image.info.get("transparency")
        canvas = image.size
        for frame in ImageSequence.Iterator(image):
            duration = frame.info.get("duration", image.info.get("duration", 0))
            durations.append(duration)
            rgb = frame.convert("RGB")
            colors = rgb.getcolors(maxcolors=MAX_COLORS + 1)
            if colors is None:
                raise ValueError(f"某帧超过 {MAX_COLORS} 色")
            visible = union_bbox(
                visible, ImageChops.difference(rgb, background).getbbox()
            )
    if canvas != CANVAS:
        raise ValueError(f"画布 {canvas[0]}x{canvas[1]}，应为 320x240")
    if version != b"GIF89a":
        raise ValueError(f"格式 {version!r}，应为 GIF89a")
    if loop != 0:
        raise ValueError(f"loop={loop!r}，应为无限循环 0")
    if transparency is not None:
        raise ValueError("不应包含透明索引")
    if not durations or any(duration <= 0 for duration in durations):
        raise ValueError("帧数为空或帧延时无效")
    if path.stat().st_size > MAX_FILE_BYTES:
        raise ValueError(
            f"文件 {path.stat().st_size / 1024:.1f}KiB，超过 200KiB"
        )
    return {
        "canvas": canvas,
        "durations": durations,
        "visible": visible,
        "bytes": path.stat().st_size,
    }


def relative_files(root):
    return {
        path.relative_to(root): path
        for path in sorted(root.glob("*/*.gif"))
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    baseline = args.baseline.resolve() if args.baseline else None

    failures = []
    files = relative_files(root)
    for scene in sorted({relative.parts[0] for relative in files}):
        count = sum(relative.parts[0] == scene for relative in files)
        if count > MAX_FILES_PER_SCENE:
            failures.append(
                f"{scene}: {count} 个 GIF，播放器上限 {MAX_FILES_PER_SCENE}"
            )

    baseline_files = relative_files(baseline) if baseline else {}
    if baseline and files.keys() != baseline_files.keys():
        missing = sorted(baseline_files.keys() - files.keys())
        extra = sorted(files.keys() - baseline_files.keys())
        failures.append(f"文件清单改变：缺少={missing} 新增={extra}")

    reports = {}
    for relative, path in files.items():
        try:
            reports[relative] = inspect(path)
            if baseline:
                old = inspect(baseline_files[relative])
                new = reports[relative]
                if (
                    new["canvas"] != old["canvas"]
                    or new["durations"] != old["durations"]
                ):
                    failures.append(f"{relative}: 画布、帧数或帧时序改变")
        except Exception as error:
            failures.append(f"{relative}: {error}")

    if failures:
        print("GIF 验证失败：")
        for failure in failures:
            print(f"  - {failure}")
        raise SystemExit(1)

    visible_widths = [
        report["visible"][2] - report["visible"][0]
        for report in reports.values()
        if report["visible"]
    ]
    visible_heights = [
        report["visible"][3] - report["visible"][1]
        for report in reports.values()
        if report["visible"]
    ]
    print(
        f"GIF 验证通过：{len(reports)} 个，"
        f"{sum(report['bytes'] for report in reports.values()) / 1024:.1f}KiB，"
        f"最大单文件 {max(report['bytes'] for report in reports.values()) / 1024:.1f}KiB，"
        f"可见范围宽 {min(visible_widths)}-{max(visible_widths)}，"
        f"高 {min(visible_heights)}-{max(visible_heights)}"
    )


if __name__ == "__main__":
    main()
