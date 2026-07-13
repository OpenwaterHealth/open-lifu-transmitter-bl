#!/usr/bin/env python3
"""Phase 2 back-channel verification.

Exercises the robust enumeration + bootloader-joins-discovery feature:

  1. Re-enumerate the chain (master broadcasts clear-config, then walks discovery).
  2. Print every module's index, operating mode (APP vs BOOTLOADER) and assigned
     I2C address.
  3. For APP modules: read firmware version + hardware id over the normal path.
  4. For BOOTLOADER modules: read the I2C DFU version at the module's *assigned*
     address (0x20, 0x21, ...) — proving a DFU-mode slave took a unique address
     instead of colliding at the fixed 0x72.

Usage:
  python test_phase2_enum.py [--reenumerate]

Options:
  --reenumerate   Force a re-enumeration first (OW_CTRL_ENUMERATE). Without it the
                  boot-time enumeration is used as-is.
"""
from __future__ import annotations
import argparse, sys, time

from openlifu_sdk.io.LIFUTXDevice import TxDevice
from openlifu_sdk.io.LIFUDFU import STM32I2CDFUviaMaster
from openlifu_sdk.io.LIFUConfig import NODE_MODE_APP, NODE_MODE_BOOTLOADER


def mode_name(m: int) -> str:
    return {NODE_MODE_APP: "APP", NODE_MODE_BOOTLOADER: "BOOTLOADER"}.get(m, f"UNKNOWN(0x{m:02X})")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reenumerate", action="store_true",
                    help="force OW_CTRL_ENUMERATE before scanning")
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=0x0483)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=0x57AF)
    args = ap.parse_args()

    tx = TxDevice(vid=args.vid, pid=args.pid)
    for _ in range(6):
        if tx.connect():
            break
        time.sleep(1)
    else:
        print("ERROR: could not connect to master TX VCP", file=sys.stderr)
        sys.exit(1)

    try:
        if args.reenumerate:
            print("Re-enumerating chain (clear-config + discovery walk)...")
            n = tx.enumerate_modules()
            print(f"  -> module_count = {n}")
            time.sleep(0.5)

        modules = tx.scan_module_modes()
        print(f"\nmodule_count = {len(modules)}")
        print(f"{'idx':>3}  {'mode':<11} {'i2c':>5}  detail")
        print("-" * 60)
        for entry in modules:
            m, mode, addr = entry["module"], entry["mode"], entry["i2c_addr"]
            addr_s = f"0x{addr:02X}" if addr else "  -"
            detail = ""
            if mode == NODE_MODE_BOOTLOADER:
                # Probe the DFU bootloader at its ASSIGNED address (not 0x72).
                try:
                    dfu = STM32I2CDFUviaMaster(uart=tx.uart, i2c_addr=addr)
                    blver = dfu.get_version()
                    if isinstance(blver, (bytes, bytearray)):
                        blver = blver.decode(errors="replace").strip("\x00")
                    detail = f"BL DFU @0x{addr:02X} ver='{blver}'"
                except Exception as e:  # noqa: BLE001
                    detail = f"BL DFU @0x{addr:02X} probe FAILED: {e}"
            else:
                # APP (or master): version + hardware id over the normal path.
                try:
                    ver = tx.get_version(module=m)
                    hwid = tx.get_hardware_id(module=m)
                    detail = f"app ver={ver} hwid={hwid}"
                except Exception as e:  # noqa: BLE001
                    detail = f"app query FAILED: {e}"
            print(f"{m:>3}  {mode_name(mode):<11} {addr_s:>5}  {detail}")

        # Collision check: every non-master address must be unique.
        addrs = [e["i2c_addr"] for e in modules if e["i2c_addr"]]
        if len(addrs) == len(set(addrs)):
            print(f"\nOK: {len(addrs)} slave address(es) are unique — no 0x72 collision.")
        else:
            print(f"\nWARNING: duplicate slave addresses detected: {addrs}", file=sys.stderr)
    finally:
        tx.disconnect()


if __name__ == "__main__":
    main()
