#! /usr/bin/env python
# -*- coding: utf-8 -*-

# Copyright (c) 2024 Realtek Semiconductor Corp.
# SPDX-License-Identifier: Apache-2.0

import sys
import os
import argparse
import base64
import json
import subprocess
import time

PROJECT_ROOT_DIR = os.path.realpath(os.path.dirname(os.path.abspath(__file__)))
PROFILE_NOR = os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../tools/ameba/Flash/Devices/Profiles/AmebaGreen2_FreeRTOS_NOR.rdev"))
PROFILE_NAND = os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../tools/ameba/Flash/Devices/Profiles/AmebaGreen2_FreeRTOS_NAND.rdev"))
PROFILE = {'nor': PROFILE_NOR, 'nand': PROFILE_NAND}
FLASH_TOOL = os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../tools/ameba/Flash/AmebaFlash.py"))
RESET_CFG = os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../tools/ameba/Flash/Reset.cfg"))
if os.getcwd() != PROJECT_ROOT_DIR:
    IMAGE_DIR = os.path.join(os.getcwd(), 'build')
else:
    IMAGE_DIR = PROJECT_ROOT_DIR

class MemoryInfo:
    MEMORY_TYPE_RAM = 0
    MEMORY_TYPE_NOR = 1
    MEMORY_TYPE_NAND = 2


def run_flash(argv):
    cmds = [sys.executable, FLASH_TOOL] + argv
    result = subprocess.run(cmds)
    return result.returncode


def _open_serial(port: str, baud: int, timeout: float = 0.2, set_idle: bool = True):
    """
    Open a serial port and (best-effort) deassert DTR/RTS immediately.

    pyserial often asserts DTR/RTS on open; on boards that wire these to BOOT/RESET,
    that can accidentally latch ROM download mode. We try to minimize that risk.
    """
    import serial  # type: ignore

    ser = serial.Serial(port, baudrate=baud, timeout=timeout)
    if set_idle:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
    return ser


def _post_reset(port: str, mode: str, inverted: bool = False) -> None:
    if mode == "none":
        return
    try:
        import serial  # type: ignore
    except Exception:
        # Best-effort: flashing still succeeded; user may reset manually.
        return

    use_dtr = mode in ("dtr", "both")
    use_rts = mode in ("rts", "both")

    # Some USB-UART adapters invert the physical reset/boot wiring.
    seq = [True, False, True] if inverted else [False, True, False]

    ser = _open_serial(port, 115200, timeout=0.2, set_idle=True)
    try:
        # Put lines into a known state then pulse.
        if use_dtr:
            ser.dtr = False
        if use_rts:
            ser.rts = False
        time.sleep(0.05)

        for level in seq:
            if use_dtr:
                ser.dtr = level
            if use_rts:
                ser.rts = level
            time.sleep(0.05)
    finally:
        # Hold both lines deasserted briefly so BOOT/RESET isn't strapped by adapter defaults.
        try:
            ser.dtr = False
            ser.rts = False
            time.sleep(0.6)
        except Exception:
            pass
        try:
            ser.close()
        except Exception:
            pass


def _bootstrap_reset_attempt(port: str, *, reset_line: str, boot_line: str, reset_assert: bool, boot_download: bool) -> None:
    """
    Best-effort: treat UART DTR/RTS as (RESET, BOOT) and reboot into normal flash boot.

    We brute-force common mappings/polarities since USB-UART boards wire/invert these differently.
    """
    ser = _open_serial(port, 115200, timeout=0.2, set_idle=True)
    try:
        def set_line(name: str, level: bool) -> None:
            if name == "dtr":
                ser.dtr = level
            elif name == "rts":
                ser.rts = level

        boot_normal_level = not boot_download

        # Hold BOOT in "normal" state, then pulse RESET.
        try:
            set_line(boot_line, boot_normal_level)
        except Exception:
            pass
        time.sleep(0.05)

        try:
            set_line(reset_line, reset_assert)
        except Exception:
            pass
        time.sleep(0.10)

        try:
            set_line(reset_line, not reset_assert)
        except Exception:
            pass
        time.sleep(0.10)

        # Leave both lines low by default.
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        time.sleep(0.6)
    finally:
        try:
            ser.close()
        except Exception:
            pass


def _apply_dtr_rts_timing_file(port: str, cfg_path: str, *, swap_lines: bool = False, invert_levels: bool = False) -> None:
    """
    Apply a Reset.cfg/Reburn.cfg-style DTR/RTS timing file:
      - lines like: dtr=0, rts=1, delay=200

    This matches the GUI ImageTool behavior and tends to be safer than ad-hoc pulses.
    """
    if not os.path.exists(cfg_path):
        return

    steps: list[tuple[str, int]] = []
    try:
        with open(cfg_path, "r", encoding="utf-8", errors="ignore") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith(("#", ";", "//")):
                    continue
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k = k.strip().lower()
                v = v.strip()
                try:
                    ival = int(v, 10)
                except Exception:
                    continue
                steps.append((k, ival))
    except Exception:
        return

    if not steps:
        return

    try:
        # Deassert lines immediately on open, then apply timing steps.
        ser = _open_serial(port, 115200, timeout=0.2, set_idle=True)
    except Exception:
        return

    try:
        for k, ival in steps:
            if k == "dtr":
                try:
                    level = (ival != 0)
                    if invert_levels:
                        level = not level
                    if swap_lines:
                        ser.rts = level
                    else:
                        ser.dtr = level
                except Exception:
                    pass
            elif k == "rts":
                try:
                    level = (ival != 0)
                    if invert_levels:
                        level = not level
                    if swap_lines:
                        ser.dtr = level
                    else:
                        ser.rts = level
                except Exception:
                    pass
            elif k == "delay":
                time.sleep(max(ival, 0) / 1000.0)
    finally:
        try:
            # Ensure we don't accidentally hold BOOT/RESET strapped via the USB-UART adapter.
            ser.dtr = False
            ser.rts = False
            time.sleep(0.6)
        except Exception:
            pass
        try:
            ser.close()
        except Exception:
            pass


def post_reset(port: str, mode: str) -> None:
    """
    Try to reset the board via UART control lines.

    Note: This cannot override a physical BOOT/download-mode strap/button.
    """
    if mode == "auto":
        _post_reset(port, "both", inverted=False)
        time.sleep(0.2)
        _post_reset(port, "both", inverted=True)
        return
    if mode.endswith("-inv"):
        base = mode[:-4]
        _post_reset(port, base, inverted=True)
        return
    _post_reset(port, mode, inverted=False)


def _sniff_uart_state(port: str, baud: int) -> str:
    """
    Best-effort classification:
      - 'rom'    : mostly 0x15 (NAK) spam
      - 'alive'  : boot logs or shell prompt observed
      - 'unknown': nothing meaningful observed
    """
    try:
        import serial  # type: ignore
    except Exception:
        return "unknown"

    try:
        ser = _open_serial(port, baud, timeout=0.2, set_idle=True)
    except Exception:
        return "unknown"

    buf = bytearray()
    try:
        ser.reset_input_buffer()
        end = time.time() + 1.2
        while time.time() < end:
            b = ser.read(4096)
            if b:
                buf.extend(b)

        # Try to elicit a prompt if we're already in shell state.
        try:
            ser.write(b"\r\n")
        except Exception:
            pass

        end = time.time() + 1.2
        while time.time() < end:
            b = ser.read(4096)
            if b:
                buf.extend(b)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    if not buf:
        return "unknown"

    if b"BOOT" in buf or b"ROM:[" in buf or b"IMG" in buf or b"\n#\r\n" in buf or b"\r\n#\r\n" in buf:
        return "alive"

    if all(c == 0x15 for c in buf) and len(buf) >= 16:
        return "rom"

    if buf.count(0x15) >= 16 and set(buf).issubset({0x15, 0x0D, 0x0A}):
        return "rom"

    return "unknown"


def auto_exit_rom_download_mode(port: str, baud: int) -> None:
    """
    If the board appears stuck in ROM download mode after flashing, try common
    DTR/RTS reset/boot sequences to return to normal flash boot.

    This is best-effort and depends on your USB-UART wiring.
    """
    # Give the ROM/floader reset time to complete before sniffing.
    time.sleep(1.0)

    # Best-effort: release both lines (some adapters assert DTR/RTS by default).
    try:
        ser = _open_serial(port, 115200, timeout=0.2, set_idle=True)
        time.sleep(0.05)
        ser.close()
    except Exception:
        pass

    state = _sniff_uart_state(port, baud)
    if state == "alive":
        return

    # If nothing is readable yet, wait a bit longer and try again (some boards only
    # start the 0x15 NAK stream after a short delay).
    if state == "unknown":
        time.sleep(2.0)
        state = _sniff_uart_state(port, baud)
        if state == "alive":
            return

    def try_reset_variant(*, swap_lines: bool, invert_levels: bool) -> bool:
        _apply_dtr_rts_timing_file(port, RESET_CFG, swap_lines=swap_lines, invert_levels=invert_levels)
        time.sleep(0.8)
        return _sniff_uart_state(port, baud) == "alive"

    # First, try the same DTR/RTS reset timing as the GUI tool (Reset.cfg), plus common variants
    # for boards that swap/invert the physical wiring.
    for _attempt in range(2):
        for swap_lines in (False, True):
            for invert_levels in (False, True):
                if try_reset_variant(swap_lines=swap_lines, invert_levels=invert_levels):
                    return

    # Try a simple pulse first (legacy behavior).
    for _attempt in range(2):
        for mode, inverted in (("both", False), ("both", True), ("rts", False), ("rts", True), ("dtr", False), ("dtr", True)):
            try:
                _post_reset(port, mode, inverted=inverted)
            except Exception:
                pass
            time.sleep(0.4)
            if _sniff_uart_state(port, baud) == "alive":
                return

    # Brute-force BOOT/RESET mappings (typical "auto download" wiring).
    candidates = [
        ("dtr", "rts"),
        ("rts", "dtr"),
    ]
    for _attempt in range(3):
        for reset_line, boot_line in candidates:
            for reset_assert in (False, True):
                for boot_download in (False, True):
                    try:
                        _bootstrap_reset_attempt(
                            port,
                            reset_line=reset_line,
                            boot_line=boot_line,
                            reset_assert=reset_assert,
                            boot_download=boot_download,
                        )
                    except Exception:
                        pass
                    time.sleep(0.4)
                    if _sniff_uart_state(port, baud) == "alive":
                        return


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument('-p', '--port', nargs="+", help='serial port')
    parser.add_argument('-b', '--baudrate', type=int, default=1500000, help='serial port baud rate')
    parser.add_argument('-m', '--memory-type', choices=['nor', 'nand', 'ram'], default="nor", help='specified memory type')
    parser.add_argument('-i', '--image', nargs=3, action='append', metavar=('image-name', 'start-address', 'end-address'),
                        help="user define image layout")
    parser.add_argument('-o', '--log-file', type=str, help='output log file with path')

    parser.add_argument('--remote-server', type=str, help='remote serial server IP address')
    parser.add_argument('--remote-password', type=str, help='remote serial server validation password')

    parser.add_argument('--chip-erase', action='store_true', help='chip erase')
    parser.add_argument('--log-level', default='info', help='log level')
    parser.add_argument('--no-reset', action='store_true', help='do not reset after flashing finished')
    parser.add_argument(
        '--post-reset',
        choices=['none', 'dtr', 'rts', 'both', 'dtr-inv', 'rts-inv', 'both-inv', 'auto'],
        default='auto',
        help=(
            'After flashing succeeds, optionally pulse UART DTR/RTS to reset (best-effort). '
            "Default is 'auto' (only attempts if the device appears stuck in ROM download mode)."
        ),
    )
    parser.add_argument(
        '--verify',
        action='store_true',
        help='After flashing, run a quick UART boot verification (uses tools/verify_boot.py if available).',
    )
    parser.add_argument(
        '--no-verify',
        action='store_true',
        help='Do not run UART boot verification after flashing (default).',
    )
    parser.add_argument(
        '--verify-seconds',
        type=float,
        default=8.0,
        help='UART capture duration for verification (default: 8).',
    )
    parser.add_argument(
        '--verify-delay',
        type=float,
        default=2.0,
        help='Delay between flash completion and verification (seconds, default: 2).',
    )
    parser.add_argument(
        '--verify-reset',
        choices=['none', 'dtr', 'rts', 'both', 'dtr-inv', 'rts-inv', 'both-inv', 'auto'],
        default='none',
        help='Verification reset mode passed to verify_boot.py (default: none).',
    )
    parser.add_argument(
        '--verify-recover',
        choices=['none', 'auto'],
        default='none',
        help='Verification recovery mode passed to verify_boot.py (default: none).',
    )

    args = parser.parse_args()
    ports = args.port
    serial_baudrate = args.baudrate
    mem_t = args.memory_type
    images = args.image

    log_file = args.log_file
    log_level = args.log_level.upper()

    remote_server = args.remote_server

    cmds = ["--download", "--profile", PROFILE.get(mem_t, 'nor')]

    if log_file is not None:
        log_path = os.path.dirname(log_file)
        if log_path:
            if not os.path.exists(log_path):
                os.makedirs(log_path, exist_ok=True)
            log_f = log_file
        else:
            log_f = os.path.join(os.getcwd(), log_file)

        cmds.append("--log-file")
        cmds.append(log_f)
    else:
        log_f = None

    if not ports:
        raise ValueError("Serial port is invalid")

    cmds.append("--port")
    cmds.extend(ports)
    cmds.append(f"--baudrate")
    cmds.append(f"{serial_baudrate}")
    cmds.append(f"--memory-type")
    cmds.append(f"{mem_t}")
    cmds.append(f"--log-level")
    cmds.append(f"{log_level}")

    if remote_server:
        cmds.append("--remote-server")
        cmds.append(remote_server)
    if args.remote_password:
        cmds.append("--remote-password")
        cmds.append(str(args.remote_password))

    if args.chip_erase:
        cmds.append("--chip-erase")

    if args.no_reset:
        cmds.append("--no-reset")

    if not images:
        cmds.append(f"--image-dir")
        cmds.append(IMAGE_DIR)
    else:
        partition_table = []

        if mem_t == "nand":
            memory_type = MemoryInfo.MEMORY_TYPE_NAND
        elif mem_t == "ram":
            memory_type = MemoryInfo.MEMORY_TYPE_RAM
        else:
            memory_type = MemoryInfo.MEMORY_TYPE_NOR

        # 1. Argparse.images format [[image-name, start-address, end-address], ...]
        for group in images:
            image_name_with_path = os.path.realpath(os.path.join(IMAGE_DIR, group[0]))
            image_name = os.path.basename(image_name_with_path)
            try:
                start_addr = int(group[1], 16)
            except Exception as err:
                raise ValueError(f"Start addr in invalid: {err}")
                sys.exit(1)

            try:
                end_addr = int(group[2], 16)
            except Exception as err:
                raise ValueError(f"End addr in invalid: {err}")
                sys.exit(1)

            partition_table.append({
                "ImageName": image_name_with_path,
                "StartAddress": start_addr,
                "EndAddress": end_addr,
                "FullErase": False,
                "MemoryType": memory_type,
                "Mandatory": True,
                "Description": image_name
            })

        # 2. Convert list to json-str
        partition_table_json_string = json.dumps(partition_table)

        # 3. Encode json-str to bytes
        partition_table_bytes = partition_table_json_string.encode('utf-8')

        # 4. Base64 encode bytes
        partition_table_base64 = base64.b64encode(partition_table_bytes).decode('utf-8')

        cmds.append(f"--partition-table")
        cmds.append(f"{partition_table_base64}")

    rc = run_flash(cmds)
    if rc != 0:
        sys.exit(1)

    # Realtek flasher already resets the device unless --no-reset is used.
    # Optional DTR/RTS handling helps if the board is stuck in ROM download mode.
    if (not args.no_reset) and ports and args.post_reset != "none":
        try:
            if args.post_reset == "auto":
                auto_exit_rom_download_mode(ports[0], serial_baudrate)
            else:
                post_reset(ports[0], args.post_reset)
        except Exception:
            pass

    # Optional: verify UART boot (best-effort). This is intentionally OFF by default:
    # touching the COM port immediately after flashing can be timing-sensitive.
    if args.verify and args.no_verify:
        raise ValueError("Use at most one of --verify or --no-verify")
    if args.verify and ports:
        # Give the board time to reboot and the COM port to settle.
        time.sleep(max(args.verify_delay, 0.0))

        # Try to locate the repo's verify script relative to this project.
        candidates = [
            os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../../../tools/verify_boot.py")),
            os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../../tools/verify_boot.py")),
        ]
        verify_script = next((p for p in candidates if os.path.exists(p)), None)
        if verify_script:
            verify_cmd = [
                sys.executable,
                verify_script,
                "-p",
                ports[0],
                "-b",
                str(serial_baudrate),
                "-t",
                str(args.verify_seconds),
                "--reset",
                args.verify_reset,
                "--recover",
                args.verify_recover,
            ]
            try:
                ret = subprocess.run(verify_cmd).returncode
                if ret != 0:
                    sys.exit(ret)
            except Exception:
                pass

    sys.exit(0)


if __name__ == "__main__":
    main()
