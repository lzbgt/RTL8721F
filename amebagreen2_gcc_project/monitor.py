#! /usr/bin/env python
# -*- coding: utf-8 -*-

import argparse
import sys
import time
import os
import subprocess

PROJECT_ROOT_DIR = os.path.realpath(os.path.dirname(os.path.abspath(__file__)))
MONITOR_TOOL = os.path.realpath(os.path.join(PROJECT_ROOT_DIR, "../tools/ameba/Monitor/monitor.py"))

def _pulse_port(port: str, mode: str, inverted: bool = False) -> None:
    if mode == "none":
        return
    try:
        import serial  # type: ignore
    except Exception:
        return

    use_dtr = mode in ("dtr", "both")
    use_rts = mode in ("rts", "both")
    seq = [True, False, True] if inverted else [False, True, False]

    ser = serial.Serial(port, baudrate=115200, timeout=0.2)
    try:
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


def pre_reset(port: str, mode: str) -> None:
    if mode == "auto":
        _pulse_port(port, "both", inverted=False)
        time.sleep(0.2)
        _pulse_port(port, "both", inverted=True)
        return
    if mode.endswith("-inv"):
        base = mode[:-4]
        _pulse_port(port, base, inverted=True)
        return
    _pulse_port(port, mode, inverted=False)

def run_monitor(argv):
    cmds = [sys.executable, MONITOR_TOOL] + argv
    p = None
    try:
        p = subprocess.Popen(cmds)
        while True:
            # Poll the subprocess status in real time
            ret = p.poll()
            if ret is not None:
                print(f"Subprocess has exited, exit code: {ret}")
                break
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("Detected Ctrl+C, interrupting the main process!")
        # Notify the subprocess to exit gracefully
        if p is not None and p.poll() is None:
            print("About to terminate the subprocess...")
            # time.sleep(1)
            p.terminate()  # Send SIGTERM
            try:
                p.wait(timeout=0.02)
                print("Subprocess exited gracefully.")
            except subprocess.TimeoutExpired:
                print("Subprocess did not respond in time, force killing...")
                p.kill()
                p.wait()
        sys.exit(0)  # Finally, exit the main process

def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='Serial Monitor (Real-time Sending, No Local Echo)')
    parser.add_argument('-p', '--port', help='Serial port name, e.g., COM3 (Windows) or /dev/ttyUSB0 (Linux)')
    parser.add_argument('-b', '--baudrate', type=int, help='Serial baud rate, e.g., 9600, 115200')
    parser.add_argument(
        '--pre-reset',
        choices=['none', 'dtr', 'rts', 'both', 'dtr-inv', 'rts-inv', 'both-inv', 'auto'],
        default='auto',
        help='Pulse UART DTR/RTS before attaching to the serial monitor (best-effort).',
    )
    parser.add_argument('-reset', action='store_true', 
                       help='Enable reset mode: Wait 100ms after connection to send "reboot" command, start output only after detecting "ROM:["')
    parser.add_argument('-debug', action='store_true', 
                       help='Enable debug mode: Display raw hexadecimal data of sent and received bytes')
    parser.add_argument('--remote-server', type=str, help='remote serial server IP address')
    parser.add_argument('--remote-password', type=str, help='remote serial server validation password')

    parser.add_argument("--decode-coredumps", action="store_true",
                        help=f"If set will handle core dumps found in serial output. Default is False")
    parser.add_argument("--enable-address-decoding", action="store_true",
                        help="Print lines about decoded addresses from the application ELF file, default is False")
    parser.add_argument("--axf-file", nargs="?", help="AXF file of application")
    parser.add_argument("--toolchain-dir", help="Set toolchain dir. If not set, will get from config.")
    parser.add_argument('--log', action='store_true', 
                       help='Enable logging mode: save logs to log file')
    parser.add_argument('--log-dir', type=str, default="",
                       help='Specify the target log file directory, if not, the logs will save to under xxxx_gcc_project when logging enabled')
    parser.add_argument('--logAGG', nargs='+',  
                        help='the logAGG enabled and source marked ')
    args = parser.parse_args()

    port = args.port
    baudrate = args.baudrate
    reset = args.reset
    debug = args.debug
    log = args.log
    log_dir = args.log_dir
    logAGG = args.logAGG

    remote_server = args.remote_server
    remote_password = args.remote_password

    if not port:
        raise ValueError("Serial port is invalid")

    # Default behavior: try to reset once so the monitor usually starts at boot logs.
    # This cannot override a physical BOOT/download-mode strap/button.
    try:
        pre_reset(port, args.pre_reset)
    except Exception:
        pass

    cmds = []
    cmds.append("--device")
    cmds.append("amebagreen2")
    cmds.append("--port")
    cmds.append(f"{port}")
    cmds.append("--baud")
    cmds.append(f"{baudrate}")
    cmds.append("--timestamps")
    cmds.append("--target-os")
    cmds.append("freertos")
    if reset:
        cmds.append("--reset")
    if debug:
        cmds.append("--debug")
    if log:
        cmds.append("--log")
        cmds.append("--log-dir")
        cmds.append(log_dir)
    if logAGG:
        cmds.append("--logAGG")
        cmds.extend(logAGG)

    if remote_server:
        cmds.append("--remote-server")
        cmds.append(remote_server)
    if remote_password:
        cmds.append("--remote-password")
        cmds.append(remote_password)

    if args.decode_coredumps:
        cmds.append("--decode-coredumps")
    if args.enable_address_decoding:
        cmds.append("--enable-address-decoding")
    if args.axf_file:
        cmds.append("--axf-file")
        cmds.append(f"{args.axf_file}")
    if args.toolchain_dir:
        cmds.append("--toolchain-dir")
        cmds.append(f"{args.toolchain_dir}")

    run_monitor(cmds)

if __name__ == "__main__":
    main()
