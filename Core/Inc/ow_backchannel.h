/*
 * ow_backchannel.h
 *
 * Phase 2 one-wire back-channel for the secure bootloader.
 *
 * When a slave transmitter has no USB host it falls back to I2C DFU mode. Before
 * this module existed such a node was invisible to the master's one-wire slave
 * discovery, so two DFU-mode slaves both listened at the fixed I2C address 0x72
 * and collided on the shared bus.
 *
 * This module lets a bootloader-mode node join the SAME OW discovery enumeration
 * the application firmware uses (see openlifu-transmitter-fw uart_comms.c):
 *   - listen for discovery on CALL_IN  (USART2 / PA2, half-duplex, upstream)
 *   - relay discovery to the next node on CALL_OUT (USART3 / PC10, half-duplex)
 *   - answer discovery identifying as NODE_MODE_BOOTLOADER and take the assigned
 *     0x20/0x21/... address as its I2C DFU listen address (via I2C_DFU_SetAddress)
 *   - honor OW_CMD_CLEAR_CONFIG (revert to 0x72, relay onward)
 *
 * Built ONLY in Release (non-DEBUG) builds: in Debug, USART2/PA2 is the debug
 * console (DEBUG_UART), and the Debug flash budget is nearly full. Slave nodes
 * must run the Release bootloader anyway (a Debug build would corrupt the
 * back-channel wire with trace output).
 *
 * All timing on this path is tick-free (delay_ms busy-loop): the HAL TIM6 tick
 * can be stalled on the DFU fallback path, so HAL_Delay()/HAL_GetTick() must not
 * be used here.
 */

#ifndef INC_OW_BACKCHANNEL_H_
#define INC_OW_BACKCHANNEL_H_

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** USART3 handle for the downstream CALL_OUT relay link (defined in this module). */
extern UART_HandleTypeDef huart3;

/**
 * @brief  Bring up USART3 (CALL_OUT) and arm the CALL_IN (USART2) receiver so the
 *         bootloader can participate in one-wire discovery. Call once on the I2C
 *         DFU fallback path before the service loop.
 */
void OW_BL_Init(void);

/**
 * @brief  Service any received one-wire frame (claim address / relay / clear).
 *         Call from the I2C DFU main loop next to I2C_DFU_Process(). Non-blocking
 *         except for bounded relay waits.
 */
void OW_BL_Process(void);

/**
 * @brief  UART TX-complete hook, dispatched from HAL_UART_TxCpltCallback (main.c)
 *         in Release builds. Sets the internal transmit-done flag.
 */
void OW_BL_TxCpltCallback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* INC_OW_BACKCHANNEL_H_ */
