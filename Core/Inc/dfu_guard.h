/**
 * @file dfu_guard.h
 * @brief Pre-erase header check shared by the USB and I2C DFU transports.
 *
 * There is one application slot, so a download has to erase the installed
 * image before the new one is complete, and SBSFU only verifies the new image
 * at the next boot. Without a check, a corrupt, wrongly signed, wrong-board or
 * downgraded file costs the device its working firmware before it is refused.
 *
 * So erase requests are recorded, not executed. The first block a host writes
 * must be the signed SFU1 header at the slot start, and it must pass the check
 * (magic, ECDSA header signature through the Secure Engine, FwVersion not below
 * the installed version or the persistent anti-rollback floor, FwSize fits the
 * slot). Only then is a requested page erased, just before its first block is
 * programmed. Requested pages that no block reached (the slot tail) are erased
 * by DFU_Guard_ErasePendingPages() once the download is finished. A refused
 * header fails the write and the installed image stays intact.
 *
 * Each host request still carries at most one page erase, so host tools need
 * no protocol change.
 */
#ifndef DFU_GUARD_H
#define DFU_GUARD_H

#include <stdint.h>

typedef enum
{
  DFU_GUARD_OK = 0,       /* proceed */
  DFU_GUARD_REFUSED,      /* header missing or rejected: nothing was erased */
  DFU_GUARD_FLASH_ERR     /* a page erase failed */
} DFU_GuardStatus;

/* Start a new transport session: no header accepted, no erase pending. */
void DFU_Guard_StartSession(void);

/* Erase request for the page containing page_addr (inside the DFU window).
 * Deferred until a header is accepted; an erase of the header page starts a
 * new download and needs a fresh header check. */
DFU_GuardStatus DFU_Guard_RequestErase(uint32_t page_addr);

/* Call before programming [addr, addr+len). Checks the header if none has been
 * accepted yet, then erases every pending page the block touches. Preserves
 * the flash LOCK state of the caller. */
DFU_GuardStatus DFU_Guard_PrepareWrite(const uint8_t *src, uint32_t addr, uint32_t len);

/* Erase a page now, bypassing deferral (used to destroy a refused image).
 * Clears the failsafe boot counter on success. */
DFU_GuardStatus DFU_Guard_ErasePageNow(uint32_t page_addr);

/* Erase the requested pages the download did not reach. No-op (the requests
 * are dropped) unless a header was accepted, so an aborted or refused
 * download never erases the installed image. */
DFU_GuardStatus DFU_Guard_ErasePendingPages(void);

/* 1 if a header was accepted and requested pages are still unerased. */
uint8_t DFU_Guard_HasPendingPages(void);

/* 1 if the page containing addr has an erase pending. */
uint8_t DFU_Guard_IsErasePending(uint32_t addr);

/* 1 once this download's header passed the check. */
uint8_t DFU_Guard_HeaderAccepted(void);

/* Lowest FwVersion a download may carry: max(installed version at first use,
 * persistent anti-rollback floor). */
uint16_t DFU_Guard_MinVersion(void);

#endif /* DFU_GUARD_H */
