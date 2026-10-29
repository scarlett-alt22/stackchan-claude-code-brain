#!/usr/bin/env python3
"""Deploy the gateway outside Documents so a macOS LaunchAgent can read it."""

import argparse
import datetime
import difflib
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


GATEWAY_DIR = Path(__file__).resolve().parent
ROOT_DIR = GATEWAY_DIR.parent
APP_SUPPORT = Path.home() / "Library/Application Support/StackChanGateway"
LAUNCH_AGENT = Path.home() / "Library/LaunchAgents/com.example.stackchan-gateway.plist"

# ★2026-08-04 防覆盖闸门。
#
# 起因：08-04 发现运行时那份 stackchan_gateway.py 比仓库那份**多**三样东西
# （中文提示词、18 字上限、load_profile 人物注入），全是 07-23/07-24 直接在
# Application Support 里手改的、从没回流。当时差一点就跑这个脚本把它们清空。
#
# 🔴 别用 mtime 判：那天仓库 mtime(08-04) 比运行时(07-24) 新，比时间会直接放行。
#    也别只比"两边一不一样"：正常部署本来就不一样。
#    判据是 **运行时那份有没有在上次安装之后被人动过** —— 所以记安装回执。
#
# 佐证：运行时网关 mtime 07-24 22:24，而 plist 是 07-25 11:20。atomic_write 用
# os.replace，一次安装三个文件的时间应该一起走；对不上就说明那次网关不是装进去的，
# 是手改的。
RECEIPT = APP_SUPPORT / ".installed.json"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_receipt() -> Dict[str, str]:
    try:
        parsed = json.loads(RECEIPT.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    files = parsed.get("files")
    return files if isinstance(files, dict) else {}


def lines_only_in_runtime(source: bytes, runtime: bytes) -> List[str]:
    """安装会**抹掉**的行：只在运行时那份里有的。这是要给人看的东西。

    ⚠️ 比对前必须去掉缩进、且按集合判存在性。
    第一版用 difflib 直接比原始行，结果 plist 只是缩进风格不同就报了 40 行
    「会丢」（真实差异只有 5 行）。**一个见了空白就喊狼来了的闸门，会把人
    训练成习惯性 --force，等于没装。**宁可漏报一点也不能滥报。
    代价是「同一行挪了位置」看不出来——但那本来也不是内容丢失。
    """
    try:
        src = source.decode("utf-8").splitlines()
        dst = runtime.decode("utf-8").splitlines()
    except UnicodeDecodeError:
        return []
    src_norm = {line.strip() for line in src if line.strip()}
    return [
        line.strip()
        for line in dst
        if line.strip() and line.strip() not in src_norm
    ]


def check_clobber(source: bytes, target: Path, receipt: Dict[str, str]) -> Optional[str]:
    """返回拒绝理由；None = 可以安全覆盖。"""
    if not target.is_file():
        return None  # 首次安装
    current = target.read_bytes()
    if current == source:
        return None  # 一模一样，覆盖不损失任何东西
    recorded = receipt.get(target.name)
    if recorded is not None and recorded == sha256_bytes(current):
        return None  # 跟上次装进去的一致 —— 没被手改，正常部署方向
    lost = lines_only_in_runtime(source, current)
    reason = [
        "运行时那份自上次安装后被改过（或没有安装回执），覆盖会丢东西：",
        "  运行时 {}".format(target),
        "  仓库   sha256={}".format(sha256_bytes(source)[:16]),
        "  运行时 sha256={}".format(sha256_bytes(current)[:16]),
    ]
    if lost:
        reason.append("  🔴 这 {} 行只在运行时那份里有，装下去就没了：".format(len(lost)))
        for line in lost[:12]:
            reason.append("    - {}".format(line[:100]))
        if len(lost) > 12:
            reason.append("    …… 还有 {} 行".format(len(lost) - 12))
        reason.append("  先把这些回流到仓库再装。确认真要丢，才加 --force。")
    else:
        # 没有内容会丢 —— 差异只是缩进/顺序/换行。必须说出来：
        # 否则人看到的是一个没有细节的拒绝，分不清"安全"和"危险"，
        # 下次就会条件反射地敲 --force，闸门当场失效。
        reason.append("  ✅ 没有任何内容行会丢失，差异只是排版/顺序。")
        reason.append("  这种情况直接 --force 是安全的（也是首次建立回执的正常路径）。")
    return "\n".join(reason)


def write_receipt(installed: List[Tuple[Path, bytes]]) -> None:
    payload = {
        "installed_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "source": str(GATEWAY_DIR),
        "files": {path.name: sha256_bytes(data) for path, data in installed},
    }
    RECEIPT.parent.mkdir(parents=True, exist_ok=True)
    RECEIPT.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    os.chmod(RECEIPT, 0o600)

sys.path.insert(0, str(GATEWAY_DIR))
from stackchan_gateway import load_device_token, load_stt_token  # noqa: E402


def atomic_write(path: Path, data: bytes, mode: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".installing")
    with temporary.open("wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    os.chmod(temporary, mode)
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--force",
        action="store_true",
        help="即使运行时那份被手改过也照装（会丢掉那些改动）",
    )
    args = parser.parse_args()

    secret_config = ROOT_DIR / "SD-staging/yaml/SC_SecConfig.yaml"
    source_gateway = GATEWAY_DIR / "stackchan_gateway.py"
    # ★2026-10-04 网关 import cc_brain；缺了它不会报错，只会悄悄退回 api 模式
    # （stackchan_gateway.py 里 `except ImportError: cc_brain = None`）。所以必须一起装。
    source_cc_brain = GATEWAY_DIR / "cc_brain.py"
    source_ca = ROOT_DIR / "SD-staging/shim_ca.pem"
    source_plist = GATEWAY_DIR / "com.example.stackchan-gateway.plist"
    for path in (secret_config, source_gateway, source_cc_brain, source_ca, source_plist):
        if not path.is_file():
            print("missing required file: {}".format(path), file=sys.stderr)
            return 1

    token = load_device_token(secret_config)
    token_sha256 = hashlib.sha256(token.encode("utf-8")).hexdigest()
    del token
    stt_token = load_stt_token(secret_config)
    stt_token_sha256 = hashlib.sha256(stt_token.encode("utf-8")).hexdigest()
    del stt_token

    runtime_gateway = APP_SUPPORT / "stackchan_gateway.py"
    runtime_cc_brain = APP_SUPPORT / "cc_brain.py"
    runtime_ca = APP_SUPPORT / "shim_ca.pem"

    plist = source_plist.read_text(encoding="utf-8")
    plist = plist.replace("__HOME__", str(Path.home()))
    plist = plist.replace("__STACKCHAN_DEVICE_TOKEN_SHA256__", token_sha256)
    plist = plist.replace("__STACKCHAN_STT_TOKEN_SHA256__", stt_token_sha256)
    if "__" in plist:
        print("unresolved placeholder in LaunchAgent template", file=sys.stderr)
        return 1

    # ★ 所有产物一起先检查、再一起写 —— 任何一个会丢东西就整批不写，
    #   不要装一半（半装状态比不装更难看出来）。
    planned: List[Tuple[Path, bytes, int]] = [
        (runtime_gateway, source_gateway.read_bytes(), 0o700),
        (runtime_cc_brain, source_cc_brain.read_bytes(), 0o700),
        (runtime_ca, source_ca.read_bytes(), 0o600),
        (LAUNCH_AGENT, plist.encode("utf-8"), 0o600),
    ]
    receipt = load_receipt()
    refusals = [
        reason
        for target, data, _ in planned
        if (reason := check_clobber(data, target, receipt)) is not None
    ]
    if refusals:
        if not args.force:
            print("\n".join(refusals), file=sys.stderr)
            print("\n拒绝安装：没有东西被改动。", file=sys.stderr)
            return 2
        print("--force：明知会丢下面这些还是照装\n", file=sys.stderr)
        print("\n".join(refusals), file=sys.stderr)
        print("", file=sys.stderr)

    for target, data, mode in planned:
        atomic_write(target, data, mode)
    write_receipt([(target, data) for target, data, _ in planned])

    print("installed gateway runtime: {}".format(runtime_gateway))
    print("installed LaunchAgent: {}".format(LAUNCH_AGENT))
    print("installed receipt: {}".format(RECEIPT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
