# Cybersecurity Documentation for Premarket Submission (FDA 510(k))

**Device / Subsystem:** openLIFU Transmitter — Secure Bootloader & Secure Firmware Update (STM32L443 MCU subsystem)
**Manufacturer:** Openwater
**Document type:** Premarket cybersecurity documentation
**Status:** DRAFT — engineering input for 510(k) submission
**Version:** 0.3
**Date:** 2026-07-12

> **Scope & disclaimer.** This document provides the **engineering/technical** cybersecurity
> content that supports a 510(k) premarket submission for the LIFU transmitter's embedded
> firmware-security subsystem (secure boot and secure firmware update). It is organized
> per the FDA guidance *Cybersecurity in Medical Devices: Quality System Considerations
> and Content of Premarket Submissions* (Sept 2023) and references **ANSI/AAMI SW96:2023**,
> **AAMI TIR57**, **IEC 81001-5-1**, and **NIST SP 800-30/-218**. It is **not** regulatory or
> legal advice. Device-level regulatory fields (intended use, classification, predicate,
> clinical claims) and the overall **security risk management file** must be completed and
> approved by Openwater Regulatory Affairs and Quality. Placeholders are marked `[[TODO: ...]]`.

---

## Table of contents

1. [Introduction and scope](#1-introduction-and-scope)
2. [Device description (security-relevant)](#2-device-description-security-relevant)
3. [Security risk management](#3-security-risk-management)
4. [Threat model](#4-threat-model)
5. [Security architecture](#5-security-architecture)
6. [Security controls](#6-security-controls)
7. [Software bill of materials (SBOM)](#7-software-bill-of-materials-sbom)
8. [Cybersecurity testing and verification](#8-cybersecurity-testing-and-verification)
9. [Vulnerability and postmarket management](#9-vulnerability-and-postmarket-management)
10. [Security labeling](#10-security-labeling)
11. [Unresolved anomalies](#11-unresolved-anomalies)
12. [Production hardening requirements](#12-production-hardening-requirements-critical)
13. [Appendix A — Security requirements traceability matrix](#appendix-a--security-requirements-traceability-matrix)
14. [Appendix B — Cryptographic details](#appendix-b--cryptographic-details)
15. [Appendix C — Glossary](#appendix-c--glossary)

---

## 1. Introduction and scope

### 1.1 Purpose
This document describes the cybersecurity design, risk management, controls, and verification
for the openLIFU transmitter **secure bootloader** and **secure firmware update** subsystem. This
subsystem establishes a hardware **Root of Trust** on each transmitter board that ensures only
firmware authored and signed by Openwater can execute, and that firmware updates are authenticated
and integrity-checked — whether the update is delivered directly over USB or **relayed to a
downstream board over the inter-board I²C bus** (see §2.2, §5.4).

### 1.2 Scope
**In scope:**
- The immutable secure bootloader (SBSFU/SECoreBin) executing on each STM32L443 transmitter MCU.
- Cryptographic verification of application firmware (authenticity + integrity) on every boot.
- The secure firmware-update paths: **USB DFU** (host-connected board) and **I²C DFU** (downstream
  board updated via a relaying upstream board); both install signed images only.
- The **one-wire back-channel** used to enumerate a bootloader-mode board onto the I²C bus (Phase 2).
- Flash memory partitioning and access controls (read-only bootloader, anti-rollback, and user-config regions).
- Key provisioning and management for firmware signing/verification.

**Out of scope (covered in separate documents):**
- The application firmware's own clinical/functional cybersecurity controls (repo `openlifu-transmitter-fw`). `[[TODO: reference]]`
- Host/companion software, cloud services, and network interfaces. `[[TODO: reference]]`
- Physical/electrical safety (IEC 60601). `[[TODO: reference]]`

### 1.3 Intended use / device context
`[[TODO: Regulatory — insert device intended use, indications, classification (e.g., Class II),
product code, predicate device(s), and whether the device is networked, multi-patient, or
life-sustaining. These drive the cybersecurity risk tier per FDA guidance.]]`

---

## 2. Device description (security-relevant)

### 2.1 Hardware platform
| Item | Detail |
|------|--------|
| MCU | STMicroelectronics STM32L443xx (Arm Cortex-M4F) |
| Internal flash | 256 KB, single-bank, 128 × 2 KB erase pages |
| SRAM | 48 KB SRAM1 (0x20000000) + 16 KB SRAM2 (0x10000000) |
| Security IP | Secure Engine (SE) call-gate isolation, MPU, Flash WRP/RDP/PCROP, option bytes, IWDG |
| External interfaces | USB FS (DFU update interface; application VCP), USART2/PA2 (half-duplex debug console **and** one-wire CALL_IN back-channel), USART3/PC10 (one-wire CALL_OUT relay), I²C1/PB6-PB7 (I²C-DFU slave + inter-board data bus), I²C2 (local sensor bus), SWD debug |

**System topology.** Transmitter boards form a **daisy chain** (`master → slave → slave …`, up to 6
boards). One board is connected to the host over USB (the *master*); the others are *slaves*. Role is
selected at runtime by USB presence. Post-enumeration, boards share an I²C data bus; the master relays
host commands (including firmware-update traffic) to slaves over I²C.

### 2.2 Software components
| Component | Role | Mutability |
|-----------|------|-----------|
| SECoreBin (Secure Engine) | Crypto primitives + key custody inside protected region | Immutable (programmed at manufacture) |
| SBSFU bootloader | Root of Trust; verifies + launches application; secure update (USB + I²C DFU) | Immutable (programmed at manufacture) |
| Application firmware | Device clinical/functional firmware (`openlifu-transmitter-fw`) | Field-updatable (signed) |

> The ST HAL, CMSIS, and USB Device Library components are baselined on the **STM32CubeL4** firmware
> package; the SBSFU/SECoreBin Root of Trust is ported from ST **X-CUBE-SBSFU V2.8.0** (NUCLEO-L432KC
> 1-image reference). Exact component versions are in the SBOM (§7).

### 2.3 Flash partition map
Defined in `Core/Inc/memory_map.h` (single source of truth; repeated by cross-reference in the linker
scripts, `usbd_dfu_if.c`, and the `py-tools` signing/flash defaults):

```
Addr range              Size   Pages   Region                Access control
-----------------------------------------------------------------------------------
0x08000000-0x0800FFFF   64K    0-31    BOOTLOADER (SBSFU+SE)  read-only via DFU (immutable)
0x08010000-0x0802FFFF   128K   32-95   APP SLOT 1 (active)    updatable (signed) — DFU writable window
                                        · SFU1 header @ 0x08010000 (0x140 bytes)
                                        · app body/vectors @ 0x08010400 (VTOR)
0x08030000-0x0803EFFF   60K    96-125  PRESET SLOTS (15x4K)   updatable (signed PRS1 preset) via DFU
0x0803F000-0x0803F7FF   2K     126     ANTI-ROLLBACK FLOOR    read-only via DFU (bootloader-managed)
0x0803F800-0x0803FFFF   2K     127     RUNTIME CONFIG         read-only via DFU (application-managed)
```

The DFU update interface (USB **and** I²C) restricts firmware erase/write to the **active
application slot** (`0x08010000`–`0x0802FFFF`, the SBSFU `SLOT_ACTIVE_1` extent), so all firmware it
writes is covered by secure-boot slot verification. The only other region it writes is the **preset
slots** (15 slots of 4 KB, `0x08030000 + N × 0x1000`), and each one only with a **signed preset**
whose `PRS1` header verified first (§5.4). The bootloader pages (0–31), the anti-rollback floor page
(126), and the runtime-config page (127) are **not erasable or writable** through either update
interface. The anti-rollback floor page is written only by the bootloader during verified boot
(§5.6); the runtime-config page is owned by the application.

The USB DfuSe alt-setting descriptor encodes this map as `32*002Ka,64*002Kg,30*002Kg,02*002Ka` (32
read-only bootloader pages, 64 slot pages, 30 preset-slot pages, 2 read-only pages for the floor and
the runtime config).

The bootloader publishes this layout in a 32-byte **info block** at `0x080001E0` (end of the vector
area; `bl_info.c`): magic `OWBI`, layout, slot range, preset range, capability word. The application
stores presets only under a bootloader that has one.

### 2.4 Cryptographic scheme
`SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256`:
- **Authenticity:** ECDSA P-256 signature over the firmware image header (public key in device).
- **Integrity:** SHA-256 digest of the firmware binary, bound into the signed header.
- **Confidentiality (transport):** AES-128-CBC for the firmware payload during distribution.

---

## 3. Security risk management

### 3.1 Process
Security risk management is integrated with the product risk management process (ISO 14971)
and performed per **AAMI TIR57 / ANSI-AAMI SW96** using **NIST SP 800-30** methodology.
Security risks that can lead to patient harm are escalated to the safety risk file.
`[[TODO: Quality — reference the controlled Security Risk Management Plan, file ID, and approvals.]]`

### 3.2 Security objectives
| ID | Objective |
|----|-----------|
| SO-1 | Only firmware authentic to Openwater executes on the device (authenticity). |
| SO-2 | Firmware cannot be silently modified without detection (integrity). |
| SO-3 | Confidential firmware IP is protected in distribution (confidentiality). |
| SO-4 | The Root of Trust (bootloader, keys) is immutable and tamper-resistant. |
| SO-5 | A failed/forged update cannot brick the device or run unauthenticated code (resilience). |
| SO-6 | Field updates are controlled, authenticated, and auditable — over both USB and I²C (updateability). |

### 3.3 Risk acceptance
`[[TODO: define residual-risk acceptance criteria and link to the security risk assessment
worksheet. Each threat in §4 maps to a control in §6 and a verification in §8 — see Appendix A.]]`

---

## 4. Threat model

Methodology: **STRIDE** over the data-flow and trust boundaries of the boot/update subsystem, including
the **inter-board relay** path unique to the daisy-chained transmitter architecture.

### 4.1 Assets
- A1: Application firmware authenticity/integrity (and patient safety that depends on it).
- A2: Firmware confidential IP.
- A3: Cryptographic keys (signing private key off-device; public + symmetric keys on-device).
- A4: Root of Trust (immutable bootloader / SECoreBin) on **every** board.
- A5: Device availability (each board must boot or safely enter recovery).

### 4.2 Trust boundaries & attack surfaces
| # | Surface | Description |
|---|---------|-------------|
| AS-1 | USB DFU interface | Firmware install endpoint on the USB-connected board when no valid firmware present (or on request). |
| AS-2 | Physical flash / debug (SWD/JTAG) | Direct read/modify of flash or RAM via debugger. |
| AS-3 | Firmware distribution channel | Path by which signed images reach the device/host tool. |
| AS-4 | Application ↔ bootloader handoff | Control transfer and shared state at launch. |
| AS-5 | Signing infrastructure (off-device) | Where the ECDSA private key is held/used. |
| AS-6 | **Inter-board I²C-DFU relay** | A bootloader-mode slave accepts a firmware image over I²C (default addr 0x72, or a unique address assigned via the one-wire back-channel), relayed by the master. |
| AS-7 | **One-wire back-channel (Phase 2)** | Local half-duplex UART used to enumerate/address boards; does not carry firmware or credentials. |

> **Trust-boundary note (relay is a transport, not a trust anchor).** The master board that relays a
> firmware image to a slave over I²C performs **no** authentication of that image; it only forwards
> bytes. Authenticity/integrity are verified **at the target board's own secure boot** before the
> image is ever executed (§5.1). A compromised or malfunctioning master therefore **cannot** cause a
> slave to run unsigned code — it can at most deliver an image the slave will reject. Each board's
> Root of Trust is independent.

### 4.3 STRIDE threats and mitigations
| ID | STRIDE | Threat | Mitigation (control) | Ref |
|----|--------|--------|----------------------|-----|
| T-1 | Tampering | Attacker replaces/patches application firmware in flash. | Secure boot verifies ECDSA header signature + SHA-256 FW tag every boot; rejects on mismatch. | C-1, C-2 |
| T-2 | Spoofing | Attacker installs forged firmware via USB **or** I²C DFU. | Downloaded image must carry a valid ECDSA signature; the **target** board verifies before execution regardless of transport. | C-1, C-4 |
| T-3 | Tampering | Attacker modifies the bootloader or keys. | Bootloader + SECoreBin in read-only pages 0–31; (production) Flash WRP/RDP/PCROP and SE MPU isolation. | C-3, C-7 |
| T-4 | Information disclosure | Firmware IP extracted in transit. | AES-128-CBC encryption of the distributed payload. | C-5 |
| T-5 | Information disclosure | Keys/firmware read out via debug port. | (Production) RDP Level ≥ 1, DAP disable, PCROP on key region; SE call-gate isolation. | C-7 |
| T-6 | Denial of service | Corrupt/partial update bricks a board. | Update is verified before execution; invalid slot → safe recovery (DFU re-entry), never executes bad code; boot-fail counter forces recovery DFU. A wrong-key, tampered, downgraded or oversized image is refused **before the installed image is erased** (§5.4). Residual: the DFU erase itself is not authenticated, so a host with access can still interrupt an accepted download or send a genuine header with a corrupted body, leaving the board in recoverable DFU. | C-2, C-6 |
| T-7 | Tampering | "Additional code beyond firmware" hidden in slot. | Bootloader verifies the unused slot region is empty before launch. | C-2 |
| T-8 | Elevation of privilege | Application calls protected Secure Engine services illegitimately. | SE call-gate validates caller region; (production) MPU privilege isolation. | C-7 |
| T-9 | Tampering | Rollback to a known-vulnerable signed firmware. | Signed-header firmware version (`FwVersion`) + **boot-time anti-rollback**: the bootloader refuses to launch any image whose verified version is below a persistent monotonic floor, and invalidates the downgrade so it cannot boot (§5.6). **Implemented & bench-verified on STM32L443.** | C-8 |
| T-10 | Spoofing | Compromise of the off-device signing key. | HSM/controlled key custody, least-privilege signing, key rotation procedure. | C-9 |
| T-11 | Spoofing / Tampering | Compromised **master** relays a malicious image to a slave over I²C. | Slave verifies the image with its **own** secure boot before execution; the relay is not a trust anchor (§4.2 note). Unsigned/tampered relayed images are rejected. | C-1, C-2, C-4 |
| T-12 | Denial of service | Malicious traffic on the one-wire back-channel or I²C bus disrupts enumeration/addressing. | Back-channel carries only enumeration/addressing (no firmware, no credentials); local wired interface inside the sealed enclosure; watchdog + fail-safe boot; addressing errors are recoverable by re-enumeration/power cycle. Does **not** affect image authentication. | C-6 |

### 4.4 Multi-patient / safety impact
`[[TODO: Risk — assess whether a single exploited unit can affect multiple patients or whether
a vulnerability is reproducible fleet-wide. FDA requires a multi-patient harm view for networked/
fleet devices. The per-board Root of Trust limits fleet compromise to entities possessing the signing
key; the inter-board relay does not weaken per-board authentication (§4.2 note).]]`

---

## 5. Security architecture

### 5.1 Architecture overview (Root of Trust)
On every reset the **immutable secure bootloader** on each board executes first (Root of Trust). It
initializes the Secure Engine, then:
1. Detects an application image in the active slot.
2. Verifies the image **header signature** (ECDSA P-256) using the embedded public key.
3. Verifies the firmware **integrity** (SHA-256) against the value bound in the signed header.
4. Verifies no extraneous code exists beyond the firmware in the slot.
5. Only if all checks pass, transfers control to the application; otherwise it does not execute
   the image and enters the controlled update (DFU) path.

```
        ┌───────────────┐  verify (ECDSA + SHA-256)   ┌──────────────────┐
 RESET ─► Secure Boot    ├────────────────────────────►  Application FW   │
        │ (Root of Trust)│  pass → launch              │  (active slot)    │
        └──────┬────────┘                               └──────────────────┘
               │ fail / no valid FW / update requested
               ▼
        ┌───────────────┐  authenticated install (signed image only)
        │  DFU update    │◄───── USB host tool (dfu-util / signed installer)   [USB-connected board]
        │  (USB or I²C)  │◄───── upstream master relaying over I²C @0x72/assigned [downstream board]
        └───────────────┘
```

**Transport selection.** After the secure-boot service returns (no valid app, or an app-requested
update), the bootloader looks for a USB host. If a host is present it serves **USB DFU**. If no USB
host is detected, it falls back to acting as an **I²C-DFU slave** so an upstream board can relay a
signed image to it. In both cases the downloaded image is subject to full secure-boot verification on
the next reset before it can execute.

### 5.2 Security architecture views (FDA-required)
- **Global system view:** §2.1–2.3 (hardware, software, partitions, daisy-chain topology).
- **Updateability / patchability view:** §5.4 and §9 (USB and I²C update paths).
- **Multi-patient harm view:** §4.4 `[[TODO]]`.
- **Security use case views:** boot-time verification (§5.1), USB field update (§5.4), I²C relay update (§5.4), key provisioning (§6 C-9).

### 5.3 Secure boot (detailed)
- Bootloader and Secure Engine reside in flash **pages 0–31 (0x08000000–0x0800FFFF)**, marked read-only
  over the update interface and (in production) write-protected by hardware Option Bytes.
- The Secure Engine exposes cryptographic services through a **call-gate** that validates the caller
  originates from the trusted bootloader interface region; in production the SE RAM/ROM is isolated by
  MPU (`SFU_ISOLATE_SE_WITH_MPU` — the L4 port uses the MPU in place of the H7 firewall IP).
- Verification is **fail-closed**: any signature, hash, or layout check failure prevents execution of
  the candidate image.
- A **boot-fail counter** (in an RTC backup register) forces the controlled DFU recovery path if the
  application repeatedly fails to reach a healthy state, preventing a boot loop from stranding the device.

### 5.4 Secure firmware update (detailed)
- Update images are produced by the controlled signing tool (the openlifu-sdk's `LIFUCrypto` signer): a 320-byte
  (0x140) SFU1 header (magic, version, sizes, SHA-256 FW tag, IV, **ECDSA-P256 signature**, image
  state, previous-header fingerprint) followed by the clear firmware body at the slot execution offset
  (0x400).
- **USB DFU** is served when a USB host is detected within the boot-time detection window. Standard
  DfuSe download (dfu-util or the pure-Python flasher) writes the signed image to the active slot.
- **I²C DFU** is served when no USB host is present. The board listens as an I²C slave (default 7-bit
  address **0x72**, runtime-reassignable — see §5.7). The upstream master relays the download as a
  sequence of I²C transactions (write-block / erase / manifest / reset). Byte-for-byte the same signed
  image is written to the slot.
- Both interfaces restrict firmware writes/erases to the **application slot**; the bootloader,
  anti-rollback, and runtime-config pages are rejected (out-of-window addresses return an error).
- **Signed preset slots (both interfaces, `Core/Src/dfu_preset.c`).** A preset slot is written only
  as: (1) **stage** the preset's 320-byte `PRS1` header at the virtual address `0xFFFFFF20` — checked
  for the `PRS1` magic, a blob size that fits a slot, and its **ECDSA signature through the Secure
  Engine** (the same key as the firmware: the Secure Engine maps `PRS1` to the `SLOT_ACTIVE_1` key;
  the distinct magic keeps a preset header from passing as a firmware header and vice versa); a refused
  header changes nothing; (2) **erase** one slot, which binds the staged header to it; (3) **write**
  the slot — the header bytes must be the staged ones and everything past the blob `0xFF`; the block
  that completes the blob checks the header in flash against the staged one and the blob's SHA-256
  against it, and **erases the slot** on a mismatch. One stage covers one slot. Slot verification at
  boot ignores the preset slots (they are outside the slot), and a firmware update never erases them.
- **Capabilities.** The capability word (`OWC1` + bits: 1 = header check before erase, 2 = signed
  preset slots) is served at the USB virtual upload address `0xFFFFFF18` and by the I²C `GETCAPS`
  command (`0x07`), and copied into the info block (§2.3).
- **Header check before erase (both interfaces, `Core/Src/dfu_guard.c`).** There is one slot, so a
  download must erase the installed image before the new one is complete. Erase requests are
  therefore recorded, not executed: the first block a host writes must be the signed header at the
  slot start, and it is checked first — magic, **ECDSA header signature through the Secure Engine**,
  `FwVersion` not below the installed version or the persistent floor (§5.6), and `FwSize` fits the
  slot. Only then is each requested page erased, just before its first block is programmed; requested
  pages the image does not reach are erased when the download finishes (USB: after manifestation;
  I²C: on MANIFEST, or on RESET). A wrong-key image (another board's or another trust domain's), a
  tampered header, a downgrade, or an oversized image is refused with the **installed image intact**
  (USB: the DNLOAD fails, `dfuERROR`; I²C: status `0x05` REJECTED). Each host request still carries
  at most one page erase, so the host protocol is unchanged. The body hash is still only checked at
  boot: a genuine header with a corrupted body is accepted, then refused at the next boot (TC-DFU-08).
- **Timing.** The signature check takes ~1.5 s with interrupts masked (Secure Engine call). USB
  advertises a 2.5 s poll interval for the header block. On I²C the slave leaves the bus for the
  check, so the master's status polls are NACKed (instead of the clock being held past the master's
  250 ms transaction timeout); the host must retry the poll until a status arrives. **Host tools
  that treat a failed poll as fatal (openlifu-sdk before the matching change) abort an I²C slave
  update after the header block**, leaving the slave in DFU with an incomplete image (recoverable
  by repeating the update with a fixed host).
- After download the board resets and re-runs full verification before executing the new image.

### 5.5 Cryptographic design
See Appendix B. Public ECDSA key and symmetric key are embedded in the Secure Engine binary at
manufacture; the **ECDSA private signing key never resides on the device**.

### 5.6 Anti-rollback (downgrade protection)
To prevent an attacker from installing an older, **validly-signed but known-vulnerable** firmware
version (threat T-9), the bootloader enforces a monotonic version floor:

- **Version source.** Each signed image carries a 16-bit `FwVersion` in its ECDSA-signed header,
  supplied by the controlled build/CI pipeline at signing time. The release semver is packed as a
  bitfield `major[15:11].minor[10:5].patch[4:0]` (major 0–31, minor 0–63, patch 0–31), which is
  strictly monotonic with release ordering — the bootloader's integer compare needs no knowledge of
  the scheme.
- **Persistent floor.** The bootloader stores accepted versions in a dedicated flash **page (page 126,
  `0x0803F000`)** as an append-only log of 8-byte doubleword entries (`version | ~version`). This page
  is **outside the DFU writable window**, so a firmware update cannot erase or lower it; it is
  non-volatile, so the floor survives power cycles and SWD reflashes of the application slot.
- **Boot-time enforcement (primary).** Enforcement occurs **after** secure boot has authenticated the
  header signature and verified the firmware hash — i.e., the `FwVersion` used is signature-verified.
  If `FwVersion < floor`, the bootloader (a) records a version-rejected error, (b) **invalidates the
  image** (erases its header) so it can never execute — even after a power cycle — and (c) resets into
  the controlled DFU recovery path. If `FwVersion ≥ floor`, the floor is raised and the image launches.
- **DFU-time check (secondary / fast-fail).** Before anything is erased, the bootloader checks the
  incoming header's signature and compares its (now authenticated) `FwVersion` against the higher of
  the installed version and the persistent floor; a downgrade is refused with the installed image
  intact (§5.4). The boot-time floor remains authoritative.
- **Residual risk.** The floor can only be cleared by erasing page 126, which requires debug/SWD
  access. Production units lock the debug port (RDP, §12), so the floor cannot be reset in the field.

This control is **fail-safe**: a flash-write failure leaves the floor unchanged (never lowered), and a
rejected downgrade results in safe DFU recovery rather than execution of vulnerable code.

### 5.7 Inter-board enumeration & I²C-DFU addressing (Phase 2 back-channel)
On the daisy chain, a board with no USB falls back to I²C-DFU mode. So that **two or more DFU-mode
boards do not collide on the default I²C address 0x72**, a bootloader-mode board joins the same
one-wire discovery enumeration the application uses and accepts a **unique** assigned I²C address
(0x20, 0x21, …); the master then reaches each downstream board at its unique address.

- **Security relevance is limited to availability/addressing, not trust.** The back-channel carries
  only discovery/addressing messages (a board reports whether it is running the application or the
  bootloader, and takes an offered address). It carries **no firmware and no credentials**, and address
  assignment does **not** influence image authentication — every relayed image is still verified by the
  target board's secure boot before execution (§5.1, §4.2 note).
- The back-channel is a **local wired** interface inside the sealed enclosure (not externally exposed).
  Malformed traffic can at most disrupt enumeration, which is recoverable by re-enumeration or a power
  cycle; it cannot cause execution of unsigned code (T-12).

---

## 6. Security controls

| ID | Control | Implements | Verification |
|----|---------|-----------|--------------|
| C-1 | ECDSA P-256 authentication of firmware header (all transports) | SO-1, T-1/T-2/T-11 | §8 TC-AUTH-* |
| C-2 | SHA-256 integrity + empty-slot check | SO-2, T-1/T-6/T-7 | §8 TC-INTEG-* |
| C-3 | Immutable bootloader/SE in read-only pages 0–31 | SO-4, T-3 | §8 TC-IMMUT-* |
| C-4 | DFU (USB **and** I²C) restricted to app slot; signed-only install | SO-6, T-2/T-11 | §8 TC-DFU-*, TC-I2CDFU-* |
| C-5 | AES-128-CBC transport confidentiality | SO-3, T-4 | §8 TC-CONF-* |
| C-6 | Fail-closed verification; safe recovery on invalid FW; boot-fail counter | SO-5, T-6/T-12 | §8 TC-FAILCLOSED-* |
| C-7 | Hardware protections: RDP, WRP, PCROP, DAP, MPU/SE isolation | SO-4, T-3/T-5/T-8 | §8 TC-HWPROT-* / §12 |
| C-8 | Signed `FwVersion` + boot-time monotonic anti-rollback floor (§5.6) | SO-5, T-9 | §8 TC-ROLLBACK-* |
| C-9 | Off-device key custody (HSM), provisioning, rotation | A3, T-10 | §8 TC-KEY-* `[[TODO procedure]]` |
| C-10 | Protected runtime-config page (no DFU erase/write) | data integrity | §8 TC-USERCFG-* |
| C-12 | Preset slots written only with a staged, signature-verified `PRS1` preset; blob hash checked on completion, slot erased on mismatch | SO-1, SO-2, T-1/T-2 | §8 TC-PRESET-* |
| C-11 | Relay is transport-only; per-board Root of Trust independent (§4.2, §5.7) | SO-1, T-11 | §8 TC-I2CDFU-02 |

---

## 7. Software bill of materials (SBOM)

The SBOM must be delivered in a machine-readable format (**SPDX** or **CycloneDX**) per FDA guidance.
The table below is the human-readable summary; `[[TODO: generate the machine-readable SBOM in CI and
attach it; include exact versions, suppliers, licenses, and known-vulnerability status (e.g., via NVD).]]`

| Component | Supplier | Version | Type | License | Notes |
|-----------|----------|---------|------|---------|-------|
| STM32 Secure Engine (SBSFU/SECoreBin) | STMicroelectronics (X-CUBE-SBSFU) | 2.8.0 (ported to L443) | Middleware | ST SLA | Root of Trust |
| STM32 Cryptographic Library | STMicroelectronics | V3.0.0 CM4 GCC (`libSTM32CryptographicV3.0.0_CM4_GCC.a`) | Crypto lib | ST SLA | AES/ECDSA/SHA-256 (no 3.1.1 GCC build shipped for L4) |
| STM32L4 HAL / CMSIS | STMicroelectronics | STM32CubeL4 (CMSIS Core v5.x) | HAL/driver | BSD-3 / Apache-2.0 | Sourced from STM32CubeL4 firmware package |
| STM32 USB Device Library (DFU/CDC) | STMicroelectronics | STM32CubeL4 | Middleware | ST SLA | USB DFU update + application VCP |
| I²C DFU slave interface | Openwater | in-repo (`Core/Src/i2c_dfu_if.c`) | Firmware | Proprietary | I²C update transport (byte-identical protocol to legacy tool) |
| One-wire back-channel | Openwater | in-repo (`Core/Src/ow_backchannel.c`) | Firmware | Proprietary | Enumeration/addressing (Release builds) |
| arm-none-eabi-gcc | Arm | 15.2 | Toolchain | GPL (toolchain) | Build only |
| CMake + Ninja | Kitware / Ninja | ≥ 3.22 / 1.x | Build system | BSD / Apache-2.0 | Build only |
| Python `cryptography` | PyCA | ≥ 41.0 | Build/sign tool | Apache-2.0/BSD | Off-device signing (`py-tools`) |
| Python `pyusb` + libusb / dfu-util | PyUSB / libusb / dfu-util | 1.x / 1.0 / 0.9+ | Build/host tool | BSD / LGPL-2.1 / GPL-2.0 | Off-device flasher |

> Off-device build/sign/flash tools are listed because they form part of the secure-update **supply
> chain** even though they do not run on the device.

---

## 8. Cybersecurity testing and verification

FDA expects evidence across four areas: security-requirements testing, threat-mitigation testing,
vulnerability testing, and penetration testing.

### 8.1 Security requirements / threat-mitigation testing

> **Verification status (STM32L443 target).** The core secure-boot and dual-transport update behaviors
> were bench-verified during development and integration on the STM32L443 transmitter hardware: signed
> image verify-and-launch, no-FW → DFU recovery, USB-DFU install, **I²C-DFU install end-to-end** (master
> relays a signed image to a downstream slave; slave verifies + launches), DFU write-bounds enforcement,
> boot-fail counter, DFU-time and **boot-time anti-rollback** (downgrade rejected + image invalidated +
> floor persistence), and the **Phase 2 two-board I²C-DFU enumeration** (two DFU-mode boards take unique
> addresses 0x20/0x21 — no 0x72 collision — and are each programmed at their assigned address). Rows
> marked `[[TODO: capture on L443]]` require formal test-record capture (logs/artifacts) on the L443
> target for submission; the underlying mechanism is the same SBSFU verification exercised by the passing
> rows. Rows marked `[[TODO: production build]]` require the hardware-protection production build (§12).

| Test ID | Objective | Method | Result |
|---------|-----------|--------|--------|
| TC-AUTH-01 | Valid signed image boots | Sign with provisioned key, install (USB), observe execution | **Pass** (verified on STM32L443) |
| TC-AUTH-02 | Image with tampered body is rejected | Flip bytes after signing; attempt boot | Expected fail-closed (signature/hash mismatch → not executed). `[[TODO: capture on L443]]` |
| TC-AUTH-03 | Image signed with wrong key is rejected | Sign with a non-provisioned ECDSA P-256 key; install via DFU | Expected reject at signature verify; enters DFU. `[[TODO: capture on L443]]` |
| TC-INTEG-01 | SHA-256 mismatch rejected | Valid header, flip one body byte without re-signing; install | Expected integrity-check fail; not executed; enters DFU. `[[TODO: capture on L443]]` |
| TC-INTEG-02 | Extra code beyond firmware rejected | Write bytes within the slot beyond the firmware via SWD; reset | Expected `VerifyActiveSlot` reject. `[[TODO: capture on L443]]` |
| TC-DFU-01 | USB DFU install of signed image succeeds | dfu-util + pure-Python flasher | **Pass** (verified on STM32L443) |
| TC-DFU-02 | DFU write to bootloader pages rejected | `dfu-util` download targeting 0x08000000 | **Pass** (out-of-window rejected; bootloader pages read-only in descriptor) |
| TC-DFU-03 | DFU write outside the active slot rejected | `dfu-util` download above the slot end (0x0803F000 / 0x0803F800) | **Pass** (floor + user-config pages rejected) |
| TC-DFU-04 | Wrong-key image refused before erase (USB + I²C) | Image signed with the console key (other board); host erases the slot, then writes | **Pass** (2026-11-17, L443, research bench build): header block refused (USB `dfuERROR`; I²C status 0x05); slot read back over SWD identical to the installed image |
| TC-DFU-05 | Tampered header refused before erase (USB + I²C) | `FwVersion` byte flipped after signing | **Pass** (2026-11-17): refused at the header block; installed image intact |
| TC-DFU-06 | Downgrade refused before erase (USB + I²C) | (a) version below installed; (b) version equal to installed but below the persistent floor | **Pass** (2026-11-17): both refused at the header block; installed image intact |
| TC-DFU-07 | Oversized image refused before erase (USB + I²C) | Genuinely signed header with `FwSize` > slot | **Pass** (2026-11-17): refused at the header block; installed image intact |
| TC-DFU-08 | Genuine header, corrupted body | Body byte flipped after signing | **Pass** (2026-11-17, USB): accepted before erase (expected), refused at boot (slot zeroed), board in recoverable DFU |
| TC-DFU-09 | Upgrade and smaller-image tail (USB + I²C) | Install v+1; then a 20 KB larger image; then a normal-size image over it | **Pass** (2026-11-17): each boots, floor raised each time; pages beyond the smaller image erased (USB after manifestation, I²C on MANIFEST); SDK `program_transmitter` path unchanged |
| TC-USERCFG-01 | DFU write/erase of user-config rejected | Target 0x0803F800 | **Pass** (outside DFU writable window) |
| TC-FAILCLOSED-01 | No/invalid FW → safe recovery, no code exec | Empty slot | **Pass** (enters DFU; no unauthenticated execution) |
| TC-PRESET-01 | Signed preset accepted into its slot; slots outside slot verification | Stage a `PRS1` header signed with the trusted key at `0xFFFFFF20`, erase + write one slot; reset; then a firmware update | **Pass** (2026-11-19, L443, research bench build): USB (master) and I²C (slave, through the master); slot read back identical; app boots; a USB firmware update left the slot unchanged |
| TC-PRESET-02 | Wrong-key / tampered / unstaged preset rejected | (a) header signed with another board's key, (b) blob altered after signing, (c) slot erase or write without staging | **Pass** (2026-11-19, USB and I²C): (a) refused at staging, slot untouched; (b) completing block refused and the slot erased; (c) refused |
| TC-IMMUT-01 | Bootloader not modifiable via update path | Hash bootloader pages 0–31 before/after a DFU write attempt to 0x08000000 | Write rejected; hash-invariance `[[TODO: capture on L443]]` |
| TC-I2CDFU-01 | I²C-DFU install of signed image succeeds | Master relays signed image to a downstream slave over I²C; slave verifies + boots | **Pass** (verified on STM32L443, full cycle) |
| TC-I2CDFU-02 | I²C-DFU install of unsigned/tampered image rejected by slave | Relay a tampered image over I²C | **Pass** (2026-11-17): the slave refuses the header before erasing (status 0x05; TC-DFU-04/05); relay is transport-only |
| TC-I2CDFU-03 | I²C-DFU write outside the active slot rejected | Relay a write targeting the bootloader / floor / config pages | **Pass** (I²C-DFU writable window clamped to slot; out-of-window returns BAD_ADDR) |
| TC-BACKCHAN-01 | Two DFU-mode boards get unique I²C addresses | Drop two slaves into DFU; re-enumerate; verify both at unique 0x20/0x21 (not 0x72) and each independently programmable | **Pass** (verified on STM32L443; both programmed at assigned addresses, both returned to app) |
| TC-HWPROT-01 | RDP/WRP/PCROP/DAP effective | Read/modify attempts via SWD | `[[TODO: production build]]` |
| TC-ROLLBACK-01 | Older version rejected (boot-time floor) | Boot vN (floor→N); flash v(N-1) to slot via SWD (floor page untouched); reset | **Pass** (verified on STM32L443): version below floor → launch refused; image invalidated; enters DFU |
| TC-ROLLBACK-02 | Equal/higher version accepted, floor raised | Flash v(N+1); reset | **Pass** (boots; floor raised) |
| TC-ROLLBACK-03 | Floor persists across app-slot reflash / SWD | Reflash app slot only (not floor page); verify floor retained | **Pass** (floor read back on a subsequent downgrade attempt) |
| TC-ROLLBACK-04 | Floor page not erasable via DFU | Target 0x0803F000 over DFU (USB and I²C) | **Pass** (outside DFU writable window; erase/write rejected) |

### 8.2 Vulnerability testing
`[[TODO: known-vulnerability scan of SBOM components against NVD/ICS-CERT; static analysis
(e.g., MISRA/compiler -Wall clean — current build is warning-clean except intentional ST
dev-mode protection reminders); fuzzing of the DFU/header parser (USB and I²C paths) and the
one-wire discovery frame parser.]]`

### 8.3 Penetration testing
`[[TODO: independent penetration test of the boot/update subsystem — fault injection, debug-port
attacks, signature-bypass attempts, USB and I²C update-path abuse, and inter-board relay abuse
(malicious master feeding a slave). Provide report and remediation.]]`

---

## 9. Vulnerability and postmarket management

`[[TODO: Quality/Security — reference the controlled Cybersecurity Management Plan. Include:]]`
- **Monitoring:** subscribe to ST PSIRT, NVD, and component advisories for SBOM items.
- **Coordinated disclosure:** intake channel and SLA for externally reported vulnerabilities.
- **Patch/update mechanism:** the secure firmware update path (§5.4) is the supported remediation
  channel; signed updates can be deployed in the field via USB (host-connected board) and over I²C
  (downstream boards), each verified by the target board's secure boot.
- **Update authenticity in the field:** in-application DFU re-entry must itself be access-controlled
  so only authorized operators can place a board into update mode. `[[TODO: define trigger &
  authorization — e.g., authenticated host command + signed update.]]`
- **Patch cadence / end-of-support:** `[[TODO]]`.

---

## 10. Security labeling

Per FDA labeling expectations, provide to users/operators: `[[TODO: Regulatory to finalize]]`
- A description of the device's cybersecurity controls (secure boot, signed updates over USB and I²C).
- Instructions for performing secure firmware updates (host-connected and daisy-chained boards) and verifying success.
- A statement that only Openwater-signed firmware will run on any transmitter board.
- Guidance on physical security of the device and its update/host environment.
- A point of contact for reporting suspected vulnerabilities.
- SBOM availability to operators on request.

---

## 11. Unresolved anomalies

`[[TODO: link each item to the controlled defect system and complete the risk assessment.]]`

| ID | Anomaly | Risk assessment | Disposition |
|----|---------|-----------------|-------------|
| AN-1 | Master-only reboot (master resets while downstream boards stay powered) can intermittently wedge the shared inter-board I²C bus, requiring bus recovery / re-enumeration before forwarded reads succeed. | **Low** (availability only; **no** effect on image authentication or secure boot — the Root of Trust is unaffected). Whole-rack power-up is unaffected. | **MITIGATED / OPEN.** Master-side I²C bus recovery (9-clock release) + slave-side I²C self-heal added; materially improved but not fully deterministic. Whole-rack power cycle is the reliable recovery. Full closure needs bench instrumentation (logic analyzer) of the one-wire + I²C timing. `[[TODO: complete analysis + fix; risk-assess residual.]]` |

---

## 12. Production hardening requirements (CRITICAL)

> The bootloader is built in two variants (`LIFU_BL_VARIANT`, `SBSFU/App/Inc/app_sfu.h`). The
> **research** variant disables the STM32 hardware security IP for debuggability
> (`SECBOOT_DISABLE_SECURITY_IPS`), which the build surfaces as intentional `#warning
> "SFU_*_PROTECT_DISABLED"` reminders. **A device shipped for clinical use MUST run the clinical
> variant** (preset `Clinical`), which applies the protections below on first boot (check-and-apply,
> `SECBOOT_OB_DEV_MODE`), also on the forced-DFU path (`SFU_BOOT_ApplyStaticProtectionsForDfu`).
> This is a release gate.

| Protection | Research variant | Clinical variant |
|------------|------------------|------------------|
| RDP (readout protection) | Off (Level 0) | **RDP Level 1** (`SFU_RDP_PROTECT_ENABLE`). Level 2 (`SFU_FINAL_SECURE_LOCK_ENABLE`) stays off: it is irreversible. |
| WRP (write protection) | Off | **WRP area A over pages 0–31** (`0x08000000`–`0x0800FFFF`: bootloader + SE) |
| PCROP (code readout protection) | Off | **PCROP over the SE key region** (`0x08000400`–`0x080004FF`; `se_key.s` is execute-only MOVW/MOVT code) |
| DAP / debug access | Open | **SWD pins released** by SBSFU before launch (`SFU_DAP_PROTECT_ENABLE`); RDP1 blocks flash access from a debugger |
| DMA | Untouched | **DMA1/DMA2 clocks disabled** while SBSFU runs (`SFU_DMA_PROTECT_ENABLE`) |
| MPU / SE isolation | Off | **Off** — the application runs privileged, so the MPU cannot fence it out of the SE; compensating controls: PCROP on the key, SE-RAM key wipe on exit (`SFU_WIPE_SE_RAM_ON_EXIT`, both variants), SE call-gate caller check. The L443 firewall (`SFU_FWALL_PROTECT_ENABLE`) is not used. |
| IWDG watchdog | Armed by `main()` (~8.2 s) | Same |
| Secure user memory | N/A (STM32H7 feature) | N/A (`#error` if enabled) |
| Trusted signing key | Research key (`py-tools/keys/research/`) | Cloud KMS HSM key (`py-tools/keys/clinical/`) |

`[[TODO: Manufacturing — document the Option Byte provisioning step and verification at production
test, and confirm keys are unique-per-product or per-product-family per the security risk assessment.
Note the L4 port isolates the Secure Engine with the MPU (not the H7 firewall IP).]]`

---

## Appendix A — Security requirements traceability matrix

| Threat | Objective | Control | Test |
|--------|-----------|---------|------|
| T-1 | SO-1, SO-2 | C-1, C-2 | TC-AUTH-02, TC-INTEG-01 |
| T-2 | SO-1, SO-6 | C-1, C-4 | TC-AUTH-03, TC-DFU-01, TC-I2CDFU-01 |
| T-3 | SO-4 | C-3, C-7 | TC-IMMUT-01, TC-HWPROT-01 |
| T-4 | SO-3 | C-5 | TC-CONF-* |
| T-5 | SO-4 | C-7 | TC-HWPROT-01 |
| T-6 | SO-5 | C-2, C-6 | TC-FAILCLOSED-01 |
| T-7 | SO-2 | C-2 | TC-INTEG-02 |
| T-8 | SO-4 | C-7 | TC-HWPROT-01 |
| T-9 | SO-5 | C-8 | TC-ROLLBACK-01..04 |
| T-10 | A3 | C-9 | TC-KEY-* |
| T-11 | SO-1 | C-1, C-2, C-4, C-11 | TC-I2CDFU-02 |
| T-12 | SO-5 | C-6 | TC-BACKCHAN-01, TC-FAILCLOSED-01 |

---

## Appendix B — Cryptographic details

| Function | Algorithm | Parameters | Role |
|----------|-----------|-----------|------|
| Firmware authentication | ECDSA | curve P-256 (secp256r1), SHA-256 | Signs/verifies the firmware header (authenticity) |
| Firmware integrity | SHA-256 | 256-bit digest | Digest of firmware binary, bound in signed header |
| Firmware confidentiality (transport) | AES-CBC | AES-128, random IV per image | Protects firmware IP in distribution |

**Key inventory**
| Key | Type | Location | Protection |
|-----|------|----------|-----------|
| Firmware signing key | ECDSA P-256 private | **Off-device** (signing infrastructure) | HSM/controlled custody `[[TODO]]` |
| Firmware verification key | ECDSA P-256 public | On-device (Secure Engine) | Immutable; (prod) PCROP/WRP |
| Firmware symmetric key | AES-128 | On-device (Secure Engine) | (prod) PCROP/WRP; SE isolation |

**Key management requirements:** generation in a controlled environment; private key never exported in
clear; documented rotation and revocation; per-product or per-family scoping per risk assessment;
re-signing of firmware on key rotation. Private key material is kept **outside** the repository (the
repo retains only the ECDSA **public** key; see the project README). `[[TODO: reference the Key
Management Procedure SOP.]]`

---

## Appendix C — Glossary

| Term | Definition |
|------|------------|
| Root of Trust | Immutable code (secure boot) trusted implicitly, anchoring all subsequent trust. |
| SBSFU | Secure Boot and Secure Firmware Update (ST framework). |
| SECoreBin | Secure Engine binary holding crypto + keys in a protected region. |
| DFU | Device Firmware Upgrade — used here over USB (DfuSe) and over I²C (relayed). |
| I²C DFU | The inter-board firmware-update transport: an upstream board relays a signed image to a downstream board over I²C. |
| One-wire back-channel | Local half-duplex UART link used to enumerate/address boards on the daisy chain (Phase 2). |
| SBOM | Software Bill of Materials. |
| RDP / WRP / PCROP | STM32 flash readout / write / code-readout protections (Option Bytes). |
| Fail-closed | On verification failure, the system denies execution rather than proceeding. |

---

*End of document. Sections marked `[[TODO]]` require completion/approval by Regulatory Affairs,
Quality, and Security before submission.*
