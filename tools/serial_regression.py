#!/usr/bin/env python3
"""读串口周期上报并按区间断言,把"烧录后手工盯日志"换成一条命令。

起因:整机没有测试,每次改动都要人肉烧录、盯屏幕、凭记忆判断"好像正常"。
本脚本把其中最值得自动化的那部分——开机是否走到位、温湿度是否落在物理
合理区间、传感器是否在线——变成可复现的判据。

    # 抓 30 秒:断言有启动标记、有周期上报、读数在合理区间
    python tools/serial_regression.py --port COM4 --seconds 30

    # 更严:要求 AHT20 与 INA226 都在线、且读数不是 nan
    python tools/serial_regression.py --port COM4 --seconds 30 --require-sensors

    # 复核别人抓好的日志(不需要设备)
    python tools/serial_regression.py --log serial.txt

    # 设备已在上电运行、只补抓周期上报(跳过启动标记断言)
    python tools/serial_regression.py --port COM4 --seconds 20 --no-boot-check

⚠️ 抓不到日志时先看这一条:platformio.ini 里的 `-DARDUINO_USB_CDC_ON_BOOT=1`
让 Arduino `Serial` 走 S3 原生 USB-CDC(板载 USB 口);FTDI 接的 UART0 上只剩
ESP-ROM 与 IDF 日志。所以在 FTDI 的 COM4 上看不到 `chamber=` 是正常的,不代表
设备卡死 —— 要抓应用日志请接板载 USB 口,或临时注释该宏后重烧。

依赖 pyserial:`pip install pyserial`。
"""

import argparse
import re
import sys

# 周期上报间隔 2s(main.cpp),这里留 25% 余量,避免偶发丢帧导致误判。
REPORT_INTERVAL_S = 2.0
REPORT_TOLERANCE = 0.75

BOOT_MARKER = "ESP32-S3 N16R8 temperature-control board starting"
REPORT_RE = re.compile(
    r"chamber=(?P<t>[-+]?[\d.]+|nan)C\s+humidity=(?P<h>[-+]?[\d.]+|nan)%"
)
# 只取家族名做键。真机日志是 "INA226 (0x40, 10mOhm): detected",
# 若把括号里的说明一起当名字,后面按名字比对就会误报"没有检测输出"。
SENSOR_RE = re.compile(r"(?P<name>AHT20|INA226)[^:]*:\s*(?P<state>detected|not detected)")
FAULT_RE = re.compile(r"FAULT:\s*(?P<reason>.+)")

# 物理合理区间:超出即说明传感器/换算坏了,而不是环境真的到了这个温度。
CHAMBER_RANGE = (-20.0, 120.0)
HUMIDITY_RANGE = (0.0, 100.0)


def read_serial(port: str, seconds: float, baud: int) -> str:
    try:
        import serial
    except ImportError:
        sys.exit("缺少 pyserial。请先安装:pip install pyserial")

    with serial.Serial(port, baud, timeout=0.2) as handle:
        # 与 .workbuddy/serial_dump.py 一致:不动 DTR/RTS,否则部分板子会复位。
        handle.setDTR(False)
        handle.setRTS(False)
        handle.reset_input_buffer()
        chunks = []
        import time

        deadline = time.time() + seconds
        while time.time() < deadline:
            data = handle.read(4096)
            if data:
                chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


def analyze(text: str, args) -> tuple:
    """返回 (failures, notes, stats)。"""
    failures, notes = [], []
    lines = text.splitlines()

    stats = {
        "reports": 0,
        "invalid": 0,
        "temps": [],
        "humidity": [],
        "sensors": {},
        "boot": BOOT_MARKER in text,
        "faults": [],
    }

    for line in lines:
        report = REPORT_RE.search(line)
        if report:
            stats["reports"] += 1
            temp, hum = report.group("t"), report.group("h")
            if temp == "nan" or hum == "nan":
                stats["invalid"] += 1
            else:
                stats["temps"].append(float(temp))
                stats["humidity"].append(float(hum))
            continue
        sensor = SENSOR_RE.search(line)
        if sensor:
            stats["sensors"][sensor.group("name").strip()] = sensor.group("state")
            continue
        fault = FAULT_RE.search(line)
        if fault:
            stats["faults"].append(fault.group("reason").strip())

    # ---- 断言 ----
    if not args.no_boot_check:
        if stats["boot"]:
            notes.append(f"启动标记已出现:{BOOT_MARKER!r}")
        else:
            failures.append(
                "未见启动标记。设备可能没复位、串口接错,或是 FTDI 口上抓不到应用日志"
                "(见本脚本开头的说明)"
            )

    # 复核历史日志时,抓取时长只能靠 --seconds 告知;没说就不猜条数。
    if args.seconds is None:
        notes.append(f"周期上报 {stats['reports']} 条(未指定 --seconds,跳过条数断言)")
    else:
        expected = max(1, int(args.seconds / REPORT_INTERVAL_S * REPORT_TOLERANCE))
        if stats["reports"] >= expected:
            notes.append(f"周期上报 {stats['reports']} 条(预期至少 {expected} 条)")
        else:
            failures.append(
                f"周期上报只有 {stats['reports']} 条,预期至少 {expected} 条 —— "
                "主循环可能卡住(热控节拍或渲染阻塞)"
            )

    if stats["temps"]:
        low, high = min(stats["temps"]), max(stats["temps"])
        if CHAMBER_RANGE[0] <= low and high <= CHAMBER_RANGE[1]:
            notes.append(f"仓温 {low:.1f}–{high:.1f} ℃,在 {CHAMBER_RANGE} 内")
        else:
            failures.append(f"仓温 {low:.1f}–{high:.1f} ℃ 超出合理区间 {CHAMBER_RANGE}")

    if stats["humidity"]:
        low, high = min(stats["humidity"]), max(stats["humidity"])
        if HUMIDITY_RANGE[0] <= low and high <= HUMIDITY_RANGE[1]:
            notes.append(f"湿度 {low:.1f}–{high:.1f} %,在 {HUMIDITY_RANGE} 内")
        else:
            failures.append(f"湿度 {low:.1f}–{high:.1f} % 超出合理区间 {HUMIDITY_RANGE}")

    reported = stats["reports"]
    if reported and len(stats["temps"]) == 0:
        failures.append(f"{reported} 条上报全是 nan —— I²C 传感器没有任何有效读数")
    elif stats["invalid"]:
        notes.append(f"其中 {stats['invalid']} 条为 nan(其余有效)")

    if args.require_sensors:
        for name, state in stats["sensors"].items():
            if state != "detected":
                failures.append(f"{name} 未检出({state})")
        missing = {"AHT20", "INA226"} - set(stats["sensors"])
        for name in missing:
            failures.append(f"{name} 没有任何检测结果输出")
        if not stats["sensors"] and not args.no_boot_check:
            failures.append("完全没有传感器检测输出,开机流程可能没走到 I²C 初始化")
    elif stats["sensors"]:
        notes.append("传感器检测:" + ", ".join(f"{k}={v}" for k, v in stats["sensors"].items()))

    for reason in stats["faults"]:
        failures.append(f"设备进入故障态:{reason}")

    return failures, notes, stats


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="串口设备名,如 COM4 / /dev/ttyUSB0")
    source.add_argument("--log", help="改为复核已有日志文件,不接设备")
    parser.add_argument(
        "--seconds",
        type=float,
        default=None,
        help="抓取时长;--port 模式默认 30 秒,--log 模式须显式给出才会断言上报条数",
    )
    parser.add_argument("--baud", type=int, default=115200, help="波特率(默认 115200)")
    parser.add_argument(
        "--require-sensors",
        action="store_true",
        help="要求 AHT20 与 INA226 均在线且读数非 nan(未接传感器的板子会失败,属预期)",
    )
    parser.add_argument(
        "--no-boot-check",
        action="store_true",
        help="跳过启动标记断言(设备已在运行、只补抓周期上报时用)",
    )
    args = parser.parse_args()

    if args.log:
        from pathlib import Path

        text = Path(args.log).read_text(encoding="utf-8", errors="replace")
        print(f"复核日志:{args.log}({len(text)} 字符)")
    else:
        if args.seconds is None:
            args.seconds = 30.0
        print(f"抓取串口:{args.port} @ {args.baud},持续 {args.seconds:g}s …")
        text = read_serial(args.port, args.seconds, args.baud)

    failures, notes, stats = analyze(text, args)

    print("\n观察到:")
    for note in notes:
        print(f"  · {note}")
    if not notes:
        print("  · (无)")

    print("\n结论:")
    if failures:
        for item in failures:
            print(f"  ✗ {item}")
        print(f"\n回归未通过:{len(failures)} 项不满足")
        return 1
    print("  ✓ 全部断言通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
