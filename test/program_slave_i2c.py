#!/usr/bin/env python3
"""Program a SLAVE transmitter over I2C through the MASTER's USB VCP, using the
openlifu-sdk, against the SECURE bootloader.

Why this exists instead of `LIFUDFUManager.program_i2c`:
  * program_i2c consumes a legacy PGK1 package and, as part of the flow,
    ERASES and WRITES the metadata page 0x0800F800. Under the secure
    bootloader that page is READ-ONLY (it now holds the anti-rollback floor at
    0x0803F000 and the app-owned user-config page at 0x0803F800), so those
    steps fail with BAD_ADDR.
  * The secure signing (open-lifu-transmitter-bl/py-tools/sign_firmware.py)
    produces a RAW image: [320B SFU1 header][0xFF pad][clear firmware],
    written whole to the slot base 0x08010000 — there is no separate metadata
    page.

So this driver bypasses the package layer and drives STM32I2CDFUviaMaster
directly: mass_erase -> write_memory(0x08010000, raw) -> manifest -> reset.

Usage:
  python program_slave_i2c.py <signed.bin> [--module N] [--i2c-addr 0x72]
                              [--already-in-dfu] [--dfu-wait 6]
"""
from __future__ import annotations
import argparse, sys, time

from openlifu_sdk.io.LIFUTXDevice import TxDevice
from openlifu_sdk.io.LIFUDFU import STM32I2CDFUviaMaster

SLOT_BASE = 0x08010000   # signed image is written here whole


def _progress(written: int, total: int, label: str = "write") -> None:
    pct = 100 * written // total if total else 100
    bar = "#" * (pct // 5) + "-" * (20 - pct // 5)
    print(f"\r  {label}: [{bar}] {pct:3d}%  ({written}/{total} B)", end="", flush=True)
    if written >= total:
        print()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("signed", help="raw signed image from sign_firmware.py")
    ap.add_argument("--module", type=int, default=1,
                    help="slave module index as seen by the master (default 1)")
    ap.add_argument("--i2c-addr", type=lambda x: int(x, 0), default=0x72,
                    help="slave bootloader I2C DFU address (default 0x72)")
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=0x0483)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=0x57AF,
                    help="master TX VCP PID (default 0x57AF)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--already-in-dfu", action="store_true",
                    help="skip enter-DFU; slave is already in its bootloader")
    ap.add_argument("--dfu-wait", type=float, default=6.0,
                    help="seconds to wait after enter-DFU for the slave BL to "
                         "come up and start listening on I2C (default 6)")
    args = ap.parse_args()

    raw = open(args.signed, "rb").read()
    if raw[0:4] != b"SFU1":
        print(f"WARNING: {args.signed} does not start with 'SFU1' — is this a "
              f"sign_firmware.py image?", file=sys.stderr)
    print(f"signed image : {len(raw)} bytes -> slave slot 0x{SLOT_BASE:08X}")
    print(f"slave module  : {args.module}   BL I2C addr: 0x{args.i2c_addr:02X}")

    tx = TxDevice(vid=args.vid, pid=args.pid, baudrate=args.baud)
    if not tx.connect():
        print("ERROR: could not connect to master TX VCP "
              f"(VID 0x{args.vid:04X} PID 0x{args.pid:04X})", file=sys.stderr)
        sys.exit(1)
    print(f"master        : {tx.get_version(module=0)}")

    try:
        # 1) Put the slave app into DFU (master forwards OW_CMD_DFU addr=module).
        if not args.already_in_dfu:
            n = tx.get_module_count()
            print(f"module_count  : {n}")
            if args.module >= n:
                print(f"ERROR: master does not see module {args.module} "
                      f"(only {n} module(s) enumerated). The master runs slave "
                      f"discovery at boot — make sure the slave is powered and "
                      f"ready on the one-wire line, then reset the master.",
                      file=sys.stderr)
                sys.exit(2)
            print(f"requesting DFU on slave module {args.module}...")
            tx.enter_dfu(module=args.module)
            print(f"waiting {args.dfu_wait:.0f}s for slave bootloader to start "
                  f"I2C DFU...")
            time.sleep(args.dfu_wait)

        # 2) Talk to the slave bootloader over I2C passthrough.
        dfu = STM32I2CDFUviaMaster(uart=tx.uart, i2c_addr=args.i2c_addr)
        blver = dfu.get_version()
        print(f"slave BL ver  : {blver.decode(errors='replace') if isinstance(blver, (bytes, bytearray)) else blver}")

        # 3) Program: mass-erase app region, write the raw signed image, manifest.
        print("mass-erasing slave application slot...")
        dfu.mass_erase()
        print("writing image...")
        dfu.write_memory(SLOT_BASE, raw, progress_callback=_progress)
        print("manifest...")
        dfu.manifest()
        print("resetting slave (secure BL will verify signature + launch app)...")
        dfu.reset()
    finally:
        pass

    # 4) Confirm the slave app came back (forwarded OW_CMD_VERSION addr=module).
    print(f"waiting {args.dfu_wait:.0f}s for slave app to boot...")
    time.sleep(args.dfu_wait)
    try:
        ver = tx.get_version(module=args.module)
        print(f"slave app ver : {ver}")
        print("SLAVE I2C UPDATE COMPLETE")
    except Exception as e:
        print(f"post-update version read failed: {e}")
        print("(slave may still be verifying/booting — retry a version query)")
    finally:
        tx.disconnect()


if __name__ == "__main__":
    main()
