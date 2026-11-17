# STM32 USB Device Library — local changes

SOUP item: STMicroelectronics `stm32-mw-usb-device`
(https://github.com/STMicroelectronics/stm32-mw-usb-device).

Upstream version in this tree: **v2.11.6** (`Core/` and `Class/DFU/`; only the DFU
class is built).

Updated `Core/` from v2.11.5 (the version STM32CubeL4 v1.18.2 pins) to v2.11.6 on
2026-11-16 for the `usbd_ctlreq.c` fixes: `USBD_StdEPReq` rejects an endpoint number
above 15 before indexing `ep_in[]` / `ep_out[]`, and `USBD_GetString` is bounded by the
destination length. The DFU class files did not change upstream between v2.11.5 and
v2.11.6. The v2.8.0 `wLength` bounds (`MIN(req->wLength, USBD_DFU_XFER_SIZE)`) were
already present.

Every file is byte-identical to upstream v2.11.6 except the one listed below. Verify
with a `diff` against the upstream tag before recording a new version here. The
generator `scripts/gen_sbom.py` pins these files by hash (`scripts/sbom-manifest.json`),
so any further change fails CI until the manifest is reviewed.

## Class/DFU/Src/usbd_dfu.c — manifestation-tolerant descriptor

`USBD_DFU_CfgDesc` `bmAttributes` is `0x0F` instead of upstream `0x0B`:
`bitManifestationTolerant` is set.

Why: with the upstream value the device self-resets inside `DFU_Leave()` after a
download, so the host's final GET_STATUS fails (`dfu-util`: "Error during download
get_status"). With manifestation tolerance the device returns to `dfuIDLE`, the host
reads a clean status, and the bootloader main loop performs the reboot once
`DFU_ImageDownloadComplete()` reports manifestation complete (`USB_DEVICE/App/
usbd_dfu_if.c`, `Core/Src/main.c`). Same change as Open-Motion's bootloaders.

This change is functional, not a security fix, and must be re-applied on every
library update.

## Glue outside this directory (CubeMX `USB_DEVICE/Target`)

Already in place for the v2.11 API: `USBD_LL_Transmit` / `USBD_LL_PrepareReceive` take a
`uint32_t` size, and `USBD_memset` / `USBD_memcpy` alias `memset` / `memcpy` (v2.11
classes clear their handle with `USBD_memset` on every class init; an empty alias
would skip that).
