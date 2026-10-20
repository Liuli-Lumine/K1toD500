#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成 data/base.bin —— 从 Python 版脚本里取出内嵌的「基准数据」。

C++ 版不依赖任何外部模板文件: 基准数据(577 行 × 16 字节 = 9232 字节)被编译进 exe。
这个脚本把 Python 版脚本里的 base85+zlib 字符串解出来, 写成二进制, 供 windres/rc 打进资源。

用法:
    python tools/gen_data.py            # 就地更新 data/base.bin
"""
from __future__ import annotations

import base64
import importlib.util
import shutil
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                      # K1toD500_cpp
SRC_DIR = ROOT.parent                   # 泉盛UV-K1\\备份\\信道
PY_SCRIPT = SRC_DIR / "泉盛UV-K1信道转换到即时通D500.py"
BLANK_XLSX = SRC_DIR / "K5Channel.xlsx"


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location("k1_to_d500_py", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    if not PY_SCRIPT.exists():
        print(f"找不到 Python 脚本: {PY_SCRIPT}")
        return 1
    mod = load_module(PY_SCRIPT)
    blob = zlib.decompress(base64.b85decode(mod._BASE_B85))
    expect = len(mod._KEEP_ROWS) * 16
    if len(blob) != expect:
        print(f"长度不符: {len(blob)} != {expect}")
        return 2
    out = ROOT / "data" / "base.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    print(f"wrote {out}  ({len(blob)} bytes, {len(mod._KEEP_ROWS)} rows)")

    if BLANK_XLSX.exists():
        dst = ROOT / "data" / "K5Channel.xlsx"
        shutil.copyfile(BLANK_XLSX, dst)
        print(f"copied {BLANK_XLSX} -> {dst} ({dst.stat().st_size} bytes)")
    else:
        print(f"注意: 没找到空白表格 {BLANK_XLSX}, 跳过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
