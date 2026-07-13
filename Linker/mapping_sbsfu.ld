/*
*****************************************************************************
**  File        : mapping_sbsfu.ld
**  Abstract    : SBSFU memory region mapping for STM32L443 (256 KB flash,
**                48 KB SRAM1). Defines the SE secure zone and the SBSFU
**                code/RAM regions. Region sizes follow the ST NUCLEO-L432KC
**                1_Image reference (same flash/RAM geometry).
**                See Core/Inc/memory_map.h for the full flash partition map.
*****************************************************************************
*/

VECTOR_SIZE = 0x200;

/* SE Code region */
__ICFEDIT_SE_Code_region_ROM_start__       = 0x08000000 + VECTOR_SIZE;
__ICFEDIT_SE_CallGate_region_ROM_start__   = __ICFEDIT_SE_Code_region_ROM_start__;
__ICFEDIT_SE_CallGate_region_ROM_end__     = __ICFEDIT_SE_Code_region_ROM_start__ + 0x1FF;

/* SE key region (protected by PCROP in production) */
__ICFEDIT_SE_Key_region_ROM_start__        = __ICFEDIT_SE_CallGate_region_ROM_end__ + 0x1;
__ICFEDIT_SE_Key_region_ROM_end__          = __ICFEDIT_SE_Key_region_ROM_start__ + 0xFF;

/* SE Startup */
__ICFEDIT_SE_Startup_region_ROM_start__    = __ICFEDIT_SE_Key_region_ROM_end__ + 0x1;
__ICFEDIT_SE_Code_nokey_region_ROM_start__ = __ICFEDIT_SE_Startup_region_ROM_start__ + 0x100;
/* SE code end. The linked SECoreBin (callgate + key + startup + SE code +
   crypto lib, all -Os) measures 19136 B from the callgate base; the region is
   sized to 0x4F00 (20224 B, ~1 KB headroom) so the SBSFU code region below
   gets every spare byte -- it has to hold both the USB and the I2C DFU
   transports. */
__ICFEDIT_SE_Code_region_ROM_end__         = 0x080050FF;

/* SE Interface ROM: code outside the secure zone (called by SBSFU) */
__ICFEDIT_SE_IF_region_ROM_start__         = __ICFEDIT_SE_Code_region_ROM_end__ + 1;
__ICFEDIT_SE_IF_region_ROM_end__           = __ICFEDIT_SE_IF_region_ROM_start__ + 0x4FF; /* 1280 B; measured use 1168 B (-Os) */

/* SBSFU Code region: extends to the end of the 64 KB bootloader area. The
   application slot starts at 0x08010000; the anti-rollback log page lives at
   0x0803F000 (see memory_map.h). */
__ICFEDIT_SB_region_ROM_start__            = __ICFEDIT_SE_IF_region_ROM_end__ + 1;
__ICFEDIT_SB_region_ROM_end__              = 0x0800FFFF;

/* No HDP / secure-user memory on STM32L4 (that is an STM32H7 feature). The
   symbols are kept (as empty regions) because mapping_export.h references
   them for code that is compile-gated off in this port. */
__ICFEDIT_SB_HDP_region_ROM_start__        = __ICFEDIT_SB_region_ROM_start__;
__ICFEDIT_SB_HDP_region_ROM_end__          = __ICFEDIT_SB_region_ROM_start__ - 1;

SE_Entry_Secure_ROM_Region_Length = __ICFEDIT_SE_CallGate_region_ROM_end__ - __ICFEDIT_SE_CallGate_region_ROM_start__ + 1;
SE_Key_region_ROM_Length          = __ICFEDIT_SE_Key_region_ROM_end__ - __ICFEDIT_SE_Key_region_ROM_start__ + 1;
SE_Startup_region_ROM_Length      = __ICFEDIT_SE_Code_nokey_region_ROM_start__ - __ICFEDIT_SE_Startup_region_ROM_start__;
SE_ROM_region_Length              = __ICFEDIT_SE_Code_region_ROM_end__ - __ICFEDIT_SE_Code_nokey_region_ROM_start__ + 1;
SE_IF_region_ROM_Length           = __ICFEDIT_SE_IF_region_ROM_end__ - __ICFEDIT_SE_IF_region_ROM_start__ + 1;
SB_ROM_region_Length              = __ICFEDIT_SB_region_ROM_end__ - __ICFEDIT_SB_region_ROM_start__ + 1;

/* RAM section */
/* SE RAM region: SE stack placed first so stack overflow stays within SE RAM */
__ICFEDIT_SE_region_RAM_start__     = 0x20000000;
__ICFEDIT_SE_region_RAM_stack_top__ = 0x20000400;
__ICFEDIT_SE_region_RAM_end__       = 0x20000FFF;

/* SBSFU RAM region (rest of SRAM1) */
__ICFEDIT_SB_region_RAM_start__     = __ICFEDIT_SE_region_RAM_end__ + 1;
__ICFEDIT_SB_region_RAM_end__       = 0x2000BFFF;

/* Empty HDP-code RAM region (see the ROM note above) */
__ICFEDIT_SB_HDP_Code_region_RAM_start__ = __ICFEDIT_SB_region_RAM_start__;
__ICFEDIT_SB_HDP_Code_region_RAM_end__   = __ICFEDIT_SB_region_RAM_start__ - 1;

SE_RAM_region_Length          = __ICFEDIT_SE_region_RAM_end__ - __ICFEDIT_SE_region_RAM_stack_top__ + 1;
SB_RAM_region_Length          = __ICFEDIT_SB_region_RAM_end__ - __ICFEDIT_SB_region_RAM_start__ + 1;

MEMORY
{
 SE_Entry_Secure_ROM_Region (rx)  : ORIGIN = __ICFEDIT_SE_CallGate_region_ROM_start__,  LENGTH = SE_Entry_Secure_ROM_Region_Length
 SE_Key_region_ROM (rx)           : ORIGIN = __ICFEDIT_SE_Key_region_ROM_start__,        LENGTH = SE_Key_region_ROM_Length
 SE_Startup_region_ROM (rx)       : ORIGIN = __ICFEDIT_SE_Startup_region_ROM_start__,    LENGTH = SE_Startup_region_ROM_Length
 SE_ROM_region (rx)               : ORIGIN = __ICFEDIT_SE_Code_nokey_region_ROM_start__, LENGTH = SE_ROM_region_Length
 SE_IF_region_ROM (rx)            : ORIGIN = __ICFEDIT_SE_IF_region_ROM_start__,         LENGTH = SE_IF_region_ROM_Length
 SB_ROM_region (rx)               : ORIGIN = __ICFEDIT_SB_region_ROM_start__,            LENGTH = SB_ROM_region_Length
 SE_RAM_region (xrw)              : ORIGIN = __ICFEDIT_SE_region_RAM_stack_top__,        LENGTH = SE_RAM_region_Length
 SB_RAM_region (xrw)              : ORIGIN = __ICFEDIT_SB_region_RAM_start__,            LENGTH = SB_RAM_region_Length
 RAM2 (xrw)                       : ORIGIN = 0x10000000,                                 LENGTH = 16K
}
