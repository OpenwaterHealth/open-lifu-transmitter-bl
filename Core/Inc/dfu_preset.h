/**
 * @file dfu_preset.h
 * @brief Signed preset writes into the preset slots, shared by the USB and the
 *        I2C DFU transports.
 *
 * Each of the MEM_PRESET_SLOT_COUNT slots (memory_map.h) holds one preset: a
 * PRS1 header (the SFU1 header layout, signed with this bootloader's key, the
 * same key as firmware headers) and the preset blob after it, at
 * MEM_PRESET_BODY_OFFSET. A slot is written in three steps:
 *
 *   1. stage   DNLOAD the 320-byte header to DFU_PRESET_STAGE_VIRT_ADDR. It is
 *              checked (PRS1 magic, body size fits a slot, signature) before
 *              any slot is touched; a refused header changes nothing.
 *   2. erase   ERASE any address in the target slot: erases that slot (both
 *              pages) and binds the staged header to it.
 *   3. write   DNLOAD the slot image (header, then blob) to the slot. The header
 *              bytes must be the staged ones and everything past the blob must
 *              be 0xFF. The block that reaches the end of the blob checks that
 *              the header in flash is the staged (verified) one and the blob's
 *              SHA-256 against it; on a mismatch the slot is erased again.
 *
 * One stage covers one slot write. The application only reads the slots; a
 * slot that is not a complete, verified preset reads as empty.
 */
#ifndef DFU_PRESET_H
#define DFU_PRESET_H

#include <stdint.h>

/* Virtual DNLOAD address (outside flash) for a preset header (step 1). */
#define DFU_PRESET_STAGE_VIRT_ADDR  0xFFFFFF20UL

/* Virtual UPLOAD address (outside flash) for the capability word: "OWC1", then
 * BL_CAPS (u32 little-endian). The I2C transport serves it as GETCAPS. */
#define DFU_CAPS_VIRT_ADDR          0xFFFFFF18UL
#define DFU_CAPS_MAGIC              "OWC1"
#define DFU_CAPS_LEN                8U

typedef enum
{
  DFU_PRESET_OK = 0,
  DFU_PRESET_REFUSED,     /* not staged, refused header, wrong slot or bytes */
  DFU_PRESET_FLASH_ERR    /* an erase or program failed */
} DFU_PresetStatus;

/* New transport session: nothing staged. */
void DFU_Preset_StartSession(void);

/* 1 if addr is inside the preset slots. */
uint8_t DFU_Preset_InRegion(uint32_t addr);

/* 1 while a header is staged (step 1 passed) and its slot not yet complete. */
uint8_t DFU_Preset_Staged(void);

/* 1 if writing [addr, addr+len) would complete the bound slot's blob, so the
 * write also runs the blob's hash check (Secure Engine, interrupts masked). */
uint8_t DFU_Preset_Completes(uint32_t addr, uint32_t len);

/* Step 1. Runs the Secure Engine signature check (~1.5 s, interrupts masked). */
DFU_PresetStatus DFU_Preset_Stage(const uint8_t *src, uint32_t len);

/* Step 2: erase the slot holding addr, and bind the staged header to it. */
DFU_PresetStatus DFU_Preset_EraseSlot(uint32_t addr);

/* Step 3: program [addr, addr+len) of the bound slot; the block that reaches
 * the end of the blob also verifies the slot (and erases it on failure). */
DFU_PresetStatus DFU_Preset_Write(const uint8_t *src, uint32_t addr, uint32_t len);

/* The capability reply: DFU_CAPS_MAGIC then BL_CAPS, DFU_CAPS_LEN bytes. */
void DFU_Preset_Caps(uint8_t out[DFU_CAPS_LEN]);

#endif /* DFU_PRESET_H */
