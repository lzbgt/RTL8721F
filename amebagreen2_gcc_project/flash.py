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

    ser = serial.Serial(port, baudrate=115200, timeout=0.2)
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
        ser = serial.Serial(port, baudrate=baud, timeout=0.2)
    except Exception:
        return "unknown"

    buf = bytearray()
    try:
        ser.reset_input_buffer()
        end = time.time() + 0.6
        while time.time() < end:
            b = ser.read(4096)
            if b:
                buf.extend(b)

        # Try to elicit a prompt if we're already in shell state.
        try:
            ser.write(b"\r\n")
        except Exception:
            pass

        end = time.time() + 0.6
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
    if _sniff_uart_state(port, baud) != "rom":
        return

    candidates = [
        # (reset-line mode, optional inverted)
        ("rts", False),
        ("rts", True),
        ("dtr", False),
        ("dtr", True),
        ("both", False),
        ("both", True),
    ]

    for mode, inverted in candidates:
        try:
            _post_reset(port, mode, inverted=inverted)
        except Exception:
            continue
        time.sleep(0.25)
        if _sniff_uart_state(port, baud) != "rom":
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

    sys.exit(0)


if __name__ == "__main__":
    main()
