#!/usr/bin/env python3
"""在主机端跑纯逻辑单元测试(等价于 `pio test -e native`)。

存在的理由:PlatformIO 的 native 平台不自带编译器,它用的是操作系统里的
GCC/Clang。Windows 上默认没有,而手装 MinGW 对只想改一行 UI 的人是负担。
PlatformIO 自己的包目录里已经躺着一份 MinGW(工具链缓存的一部分),本脚本
把它临时挂到 PATH 上,用完即散 —— 不改系统环境,不写注册表。

    python tools/run_host_tests.py             # 跑全部主机端测试
    python tools/run_host_tests.py -f "*theme*" # 只跑名字匹配的用例

Linux/macOS 与 CI 上系统自带 g++,直接 `pio test -e native` 即可,
本脚本在这两种环境下只做一层转发,行为完全一致。
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

PROJECT_DIR = Path(__file__).resolve().parent.parent
IS_WINDOWS = os.name == "nt"
EXE = ".exe" if IS_WINDOWS else ""


def find_pio() -> str:
    """优先用 PATH 上的 pio,退回 PlatformIO 的 venv。"""
    found = shutil.which("pio")
    if found:
        return found
    venv_bin = "Scripts" if IS_WINDOWS else "bin"
    candidate = Path.home() / ".platformio" / "penv" / venv_bin / f"pio{EXE}"
    if candidate.exists():
        return str(candidate)
    sys.exit("找不到 pio。请先安装 PlatformIO Core(https://platformio.org/install)。")


def find_host_toolchain() -> Optional[Path]:
    """返回一份可用的主机端 GCC 的 bin 目录,找不到返回 None。"""
    # 1) 系统里已有 g++/clang++ 就直接用,不掺和。
    for name in ("g++", "clang++"):
        if shutil.which(name):
            return None

    # 2) PlatformIO 包目录里的 MinGW(Windows 常见路径)。
    pkg_root = Path.home() / ".platformio" / "packages"
    for pkg in ("toolchain-gccmingw32", "toolchain-gcc"):
        bin_dir = pkg_root / pkg / "bin"
        if (bin_dir / f"g++{EXE}").exists():
            return bin_dir
    return None


def main() -> int:
    env = os.environ.copy()
    toolchain = find_host_toolchain()

    if toolchain:
        # MinGW 的运行时 DLL(libstdc++-6.dll / libgcc_s_dw2-1.dll)也在 bin 下,
        # 所以这一条 PATH 同时解决了"编译找不到 g++"和"跑起来找不到 DLL"。
        env["PATH"] = str(toolchain) + os.pathsep + env.get("PATH", "")
        print(f"[host-test] 主机端工具链:{toolchain}")
    elif not any(shutil.which(n) for n in ("g++", "clang++")):
        sys.exit(
            "[host-test] 找不到主机端 C++ 编译器。\n"
            "  Windows:  pio pkg install --global --tool platformio/toolchain-gccmingw32\n"
            "  Debian:   sudo apt-get install -y g++\n"
            "  macOS:    xcode-select --install"
        )

    cmd = [find_pio(), "test", "-e", "native", *sys.argv[1:]]
    print(f"[host-test] {' '.join(cmd)}  (cwd={PROJECT_DIR})")
    return subprocess.call(cmd, cwd=str(PROJECT_DIR), env=env)


if __name__ == "__main__":
    raise SystemExit(main())
