# open-lifu-transmitter-bl — Secure Boot: Build, Sign & Flash Guide

Target: **STM32L443** (256 KB flash, 2 KB pages)
Crypto scheme: `SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256`
Bootloader: `0x08000000` (64 KB); anti-rollback log page at `0x0803F000`
DFU transports: **USB DfuSe** (host detected) or **I2C slave @ 0x72** (no USB host) — see `test/README.md`
Application slot: `0x08010000` (188 KB, 94 × 2 KB pages)
Application **runs from**: `0x08010400` (slot + `SFU_IMG_IMAGE_OFFSET`)

> The bootloader authenticates the application before every boot: the firmware
> header is ECDSA-P256 signed and the firmware body is checked with SHA-256.
> A bare-metal application must be **relinked to `0x08010400`** and **signed**
> before it can be installed.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| CMake | ≥ 3.22 | |
| Ninja | any | |
| arm-none-eabi-gcc | 13.3.1 | tested |
| OpenOCD | any | flash bootloader via ST-Link |
| dfu-util | ≥ 0.9 | install application via USB DFU (option A) |
| Python | ≥ 3.9 | key generation, signing, pure-Python DFU flasher |
| pyusb | ≥ 1.3 | only for the pure-Python flasher (`flash_firmware.py`) |

---

## 1. Python environment

Run once from the repository root:

```sh
cd py-tools
python -m venv .venv

# Windows
.venv\Scripts\activate
# Linux / macOS
source .venv/bin/activate

pip install -r requirements.txt   # cryptography, pyusb
```

---

## 2. Generate keys  *(once per device family)*

```sh
# From repo root, with the .venv active:
python py-tools/generate_keys.py          # add --force to overwrite existing keys
```

What it does:
- Generates an **ECDSA P-256** key pair and a **16-byte AES-128** key.
- Writes to `bl-keys/transmitter/` next to the repo checkout (override with the
  `LIFU_BL_KEYS` environment variable); a copy of the public key is kept in
  `py-tools/keys/`:
  - `ecdsa_private.pem` — **keep secret, never commit**
  - `ecdsa_public.pem`, `pub_key_x.bin`, `pub_key_y.bin`
  - `aes128.bin` — **keep secret, never commit**
- Rewrites `SECoreBin/Startup/se_key.s` with the public key + AES key embedded
  as ARM MOVW/MOVT instructions (commit this file).

> Regenerating keys invalidates all previously signed firmware. **Rebuild the
> bootloader (step 3)** afterwards so the new public key is embedded in SECoreBin.

---

## 3. Build the SECoreBin + bootloader

Two-step CMake build: **SECoreBin** (the Secure Engine binary) is compiled first
and embedded into the SBSFU bootloader via `.incbin`. The preset handles both.

```sh
# Configure (Debug = SBSFU UART traces on USART2 @ 115200 (COM5); Release = quiet)
cmake --preset Debug

# Build (SECoreBin then open-lifu-transmitter-bl)
cmake --build build/Debug --target all -j 10
```

| Output | Description |
|--------|-------------|
| `build/Debug/open-lifu-transmitter-bl.hex` | Bootloader (SBSFU + embedded SECoreBin) — flash this |
| `build/Debug/open-lifu-transmitter-bl.bin` | Raw binary |
| `build/Debug/SECoreBin/SECoreBin.bin` | SE Core binary (embedded automatically) |

---

## 4. Flash the bootloader  *(ST-Link)*

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program build/Debug/open-lifu-transmitter-bl.hex verify; reset run; exit"
```

This writes the bootloader to `0x08000000`. With an empty application slot the
bootloader boots, finds no valid firmware, and **enters USB DFU download mode**
(LED blinks, `0483:df11` enumerates). Confirm:

```sh
dfu-util -l        # or:  python py-tools/flash_firmware.py list
```

USART2 (PA2, half-duplex, 115200 8N1 — COM5) shows:

```
= [SBOOT] STATE: CHECK USER FW STATUS
	  No valid FW found - entering USB DFU download mode
```

---

## 5. Prepare a bare-metal application for secure boot

A normal CubeMX/bare-metal app links to `0x08000000` and boots directly. To run
under the bootloader it must live in the active slot **at `0x08010400`** (the
slot starts at `0x08010000`; the first `0x400` bytes hold the signed header).
Two edits:

**5a. Linker script** — set the FLASH region origin/length:

```ld
/* application linker script */
FLASH (rx) : ORIGIN = 0x08010400, LENGTH = 187K
```
*(App slot 1 is 188K at 0x08010000; the app runs at 0x08010400, so the usable
length is 188K − 0x400 = 187K. See Core/Inc/memory_map.h.)*

**5b. Vector table relocation (VTOR)** — in `Core/Src/system_stm32h7xx.c`:

```c
#define USER_VECT_TAB_ADDRESS                  /* uncomment to enable relocation */
...
#define VECT_TAB_BASE_ADDRESS   0x08010400U    /* was FLASH_BASE */
```

Then build the application normally to produce `your_app.bin`. Verify the vector
table landed correctly:

```sh
arm-none-eabi-objdump -h build/Debug/your_app.elf | grep isr_vector
#   0 .isr_vector  ...  08020400  08020400  ...
```

> Nothing else is required — clocks, peripherals, UART, etc. are configured by
> the application as usual. The bootloader hands off with interrupts enabled,
> exactly like a normal reset.

---

## 6. Sign the application

Signing lives in the **openlifu-sdk** (`openlifu_sdk.io.LIFUCrypto`) — the
single source of truth for the image format and the `FwVersion` encoding. The
local `py-tools/sign_firmware.py` is retired (it only prints a pointer to the
SDK).

```sh
pip install 'openlifu-sdk[crypto]'
python -m openlifu_sdk.io.LIFUCrypto sign \
    --keys     py-tools/keys \
    --firmware path/to/your_app.bin \
    --version  0.0.1 \
    --output   your_app_signed.bin
```

| Option | Description |
|--------|-------------|
| `--keys` | Directory holding `ecdsa_private.pem` + `aes128.bin` |
| `--firmware` | Raw `.bin` built for `0x08010400` (step 5) |
| `--version` | Semver `major.minor.patch` (bitfield-encoded) or raw 16-bit integer — see [Firmware versioning & anti-rollback](#firmware-versioning--anti-rollback) |
| `--version-header` | Alternative to `--version`: read `FW_VERSION` from the build's generated `version.h` |
| `--output` | Signed image output path |

Inspect / validate a signed image:

```sh
python -m openlifu_sdk.io.LIFUCrypto info   your_app_signed.bin
python -m openlifu_sdk.io.LIFUCrypto verify your_app_signed.bin --keys py-tools/keys
```

### Signed-image layout (what gets flashed to the slot)

```
Offset 0x000 [320 B]  Header  (SFU1 magic, version, sizes, SHA-256 FW tag,
                               IV, 64-B ECDSA signature, image-state, fingerprint)
Offset 0x140 [704 B]  0xFF padding  (header region padded up to SFU_IMG_IMAGE_OFFSET)
Offset 0x400 [FwSize] Firmware body  (CLEAR — see note)
```

> **Note (single-slot / NO_LOADER):** the active slot stores the firmware **in
> clear**. The bootloader's boot-time check is SHA-256 only (it does not decrypt);
> AES-CBC decryption belongs to the OTA install path, which is not used here. The
> header is still ECDSA-signed, so the image is authenticated. The SDK signer
> emits the clear body at offset `0x400` automatically.

### Firmware versioning & anti-rollback

The signed header carries a **16-bit `FwVersion`** (offset `0x006`, inside the
ECDSA-signed region); because it is signed, it cannot be altered without
re-signing.

**Encoding convention — 16-bit bitfield.** The release semantic version is
packed as `major[15:11] . minor[10:5] . patch[4:0]`:

```
FwVersion = (major << 11) | (minor << 5) | patch
```

| Semver | `FwVersion` |
|--------|-------------|
| 0.0.1    | `1` |
| 1.0.0    | `2048` |
| 1.2.6    | `2118` |
| 2.0.0    | `4096` |
| 31.63.31 | `65535` (max) |

Ranges: **major 0–31, minor 0–63, patch 0–31** (max `31.63.31` = `0xFFFF`;
`0.0.0` is invalid). The packing is **strictly monotonic** with semver ordering,
so the bootloader's anti-rollback integer compare needs no knowledge of the
scheme. Pre-release / git-describe suffixes are **not** encoded — `1.8.0-rc.1`,
`1.8.0-dev.2` and `1.8.0` all encode identically.

The encoding is owned by the SDK signer — pass the semver string and it packs
it for you:

```sh
python -m openlifu_sdk.io.LIFUCrypto sign --keys py-tools/keys \
    --firmware app.bin --version 1.8.0 --output app_signed.bin   # FwVersion 2304
```

> **Migration note.** This replaces the earlier decimal `MMmmpp` convention
> (`major*10000 + minor*100 + patch`, major ≤ 6). Old encodings are numerically
> much larger (1.2.6 was `10206`, is now `2118`), so a unit that latched an
> anti-rollback floor under the old scheme rejects new-scheme images until the
> floor is reset (full-chip erase via debugger).

**Anti-rollback (downgrade protection).** The bootloader keeps a persistent,
monotonic **version floor** — the highest `FwVersion` it has ever launched —
stored in a flash page (0x0800F800) that the DFU update path cannot erase. After verifying
an image's signature, it compares the (now-trusted) `FwVersion` to the floor:

- `FwVersion ≥ floor` → launch the app, and raise the floor to this version.
- `FwVersion < floor` → **reject**: the image is invalidated so it can never boot
  (even after a power cycle), and the device drops to USB DFU:

  ```
  = [SBOOT] Anti-rollback: rejected older firmware version
  ```

Practical effect: you may re-flash the **same** version or install a **higher**
one, but never a lower one. To recover from a bad release, ship a build whose
version is **≥** the current floor (bump the patch if needed). The floor resets
only on a full-chip erase via debugger, which production RDP locks out.

> This is the **application** firmware version. It is distinct from the
> **bootloader's own** version string (git `describe`) shown on the boot banner
> and read back with `flash_firmware.py version` — see
> [Bootloader version](#bootloader-version).

---

## 7. Install the application firmware

The bootloader must be in **USB DFU mode** (empty/invalid slot — see step 4; to
re-enter DFU on a programmed device, erase the slot header and reset, or just
flash a new image which replaces the old one). The image is written to
`0x08010000`; the device then resets, verifies the signature, and boots the app.

### Option A — dfu-util

```sh
dfu-util -D your_app_signed.bin -a 0 -s 0x08010000:leave
```
*(The `Error during download get_status` after `:leave` is benign — the device
detaches/resets immediately. `Invalid DFU suffix` is also expected.)*

### Option B — pure-Python flasher (no dfu-util)

```sh
python py-tools/flash_firmware.py your_app_signed.bin

# helpers:
python py-tools/flash_firmware.py list                 # list DFU devices
python py-tools/flash_firmware.py read 0x08010000 320  # dump the signed header
python py-tools/flash_firmware.py version              # read the bootloader version
python py-tools/flash_firmware.py leave                # reset device into the app
```

`flash_firmware.py` uses `stm32dfu.py` (pure-Python DfuSe over pyusb). It
auto-detects the device's DFU transfer size, erases the affected slot page(s),
writes the image to `0x08010000`, and resets — the equivalent of the dfu-util
command above.

> **Windows / pyusb driver:** the "STM32 BOOTLOADER" device must be bound to a
> WinUSB/libusb driver (use [Zadig](https://zadig.akeo.ie/)), or `stm32dfu.py`
> will fall back to the libusb-1.0.dll bundled with STM32CubeProgrammer (see
> `_CUBEPROG_LIBUSB_PATHS` in `stm32dfu.py`).

### Option C — direct ST-Link (no DFU)

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program your_app_signed.bin 0x08010000 verify; reset run; exit"
```

### Success (USART2 @ 115200, COM5)

```
= [SBOOT] STATE: VERIFY USER FW SIGNATURE
= [SBOOT] STATE: EXECUTE USER FIRMWARE
<your application output>
```

### Bootloader version

The **bootloader** has its own version string — the git `describe` of the
`open-lifu-transmitter-bl` repo, generated into `version.h` by CMake at configure time
(`FW_VERSION`; e.g. `1.4.0` for a tagged build, or `fd1546a-dirty` for an
untagged/dirty tree). It is independent of the application `FwVersion` above.

It is reported in two places:

- **Boot banner** (USART2 @ 115200, COM5), on every boot:

  ```
  = [SBOOT] Bootloader version: 1.4.0
  ```

- **Over DFU** (no UART needed), via a read-only query:

  ```sh
  python py-tools/flash_firmware.py version    # alias: dfu_ver  ->  1.4.0
  ```

  Internally this UPLOADs from the virtual address `0xFFFFFF00`, which the
  bootloader intercepts to return the string; no flash is read or written.

---

## Flash memory map

Defined in `Core/Inc/memory_map.h` (single source of truth). 256 KB flash, 128 × 2 KB pages:

```
Addr range              Size   Pages   Region                   DFU access
---------------------------------------------------------------------------
0x08000000-0x0800FFFF    64K   0-31    BOOTLOADER               read-only
  0x08000000  ISR vectors
  0x08000200  SE CallGate + SECoreBin (key @ 0x08000400)
  0x08005600  SBSFU code (USB + I2C DFU transports)
0x08010000-0x0803EFFF   188K   32-125  APP SLOT 1 (active)      read/erase/write
  0x08010000    └ signed header (0x400)
  0x08010400    └ application firmware (execution address)
0x0803F000-0x0803F7FF     2K   126     ANTI-ROLLBACK FLOOR LOG  read-only
0x0803F800-0x0803FFFF     2K   127     USER CONFIG (app-owned)  read-only
0x08040000  End of flash
```

> The bootloader pages, the anti-rollback log page and the USER CONFIG page cannot be erased or written
> over DFU (the bootloader may still *read* user config). The bootloader boots SLOT 1 (single-image configuration).

---

## Key files reference

```
py-tools/
  generate_keys.py     Generate ECC P-256 + AES-128 keys, update se_key.s
  gen_se_key_s.py      Low-level: raw key bytes -> ARM MOVW/MOVT asm
  sign_firmware.py     RETIRED stub — signing moved to openlifu-sdk (LIFUCrypto)
  flash_firmware.py    Pure-Python USB DFU installer (uses stm32dfu.py)
  stm32dfu.py          Pure-Python STM32 DfuSe protocol (pyusb)
  requirements.txt     cryptography, pyusb
  keys/
    ecdsa_private.pem  PRIVATE — never commit
    ecdsa_public.pem, pub_key_x.bin, pub_key_y.bin
    aes128.bin         PRIVATE — never commit

SECoreBin/Startup/
  se_key.s             Auto-generated by generate_keys.py — commit this
```

---

## Quick reference

```sh
# one-time
python py-tools/generate_keys.py
cmake --preset Debug && cmake --build build/Debug --target all -j 10
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program build/Debug/open-lifu-transmitter-bl.hex verify; reset run; exit"

# per application build
#   (1) link app at 0x08010400 + VTOR 0x08010400, then build your_app.bin
python -m openlifu_sdk.io.LIFUCrypto sign --keys py-tools/keys --firmware your_app.bin --version 0.0.1 --output your_app_signed.bin
python py-tools/flash_firmware.py your_app_signed.bin        # or: dfu-util -D your_app_signed.bin -a 0 -s 0x08010000:leave
```
