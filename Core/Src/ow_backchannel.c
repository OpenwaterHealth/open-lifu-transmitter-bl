/*
 * ow_backchannel.c
 *
 * Phase 2 one-wire back-channel for the secure bootloader. See ow_backchannel.h
 * for the rationale. Wire protocol is byte-identical to the application firmware
 * (openlifu-transmitter-fw uart_comms.c / common.h):
 *
 *   [0xAA][id:2 BE][type][cmd][addr][reserved][len:2 BE][data...][crc16 BE][0xDD]
 *   crc16 = util_crc16(&buf[1], len + 8)  (CRC16-CCITT, init 0xFFFF)
 *
 * This file is compiled ONLY in Release builds (see CMakeLists.txt); in Debug the
 * USART2/PA2 wire is the debug console.
 */

#include "ow_backchannel.h"
#include "i2c_dfu_if.h"
#include "utils.h"       /* util_crc16, delay_ms (tick-free) */
#include <string.h>

/* -------------------------------------------------------------------------
 * OW protocol constants (must match openlifu-transmitter-fw/Core/Inc/common.h)
 * ---------------------------------------------------------------------- */
#define OW_START_BYTE        0xAAU
#define OW_END_BYTE          0xDDU

#define OW_TYPE_ONE_WIRE     0xE5U   /* OW_ONE_WIRE       */
#define OW_TYPE_ONEWIRE_RESP 0xECU   /* OW_ONEWIRE_RESP   */
#define OW_TYPE_TIMEOUT      0xEEU   /* OW_TIMEOUT        */
#define OW_TYPE_ERROR        0xEFU   /* OW_ERROR          */

#define OW_CMD_CLEAR_CONFIG  0x0BU
#define OW_CMD_DISCOVERY     0x0CU

#define NODE_MODE_BOOTLOADER 0x02U   /* NodeMode reported in discovery response */

#define OW_ADDR_MIN          0x20U
#define OW_ADDR_MAX          0x25U

/* Frame sizing: OW discovery/clear frames are tiny (header 9 + <=1 data + crc 2 +
 * end 1). 64 bytes leaves generous headroom and absorbs line noise. */
#define OW_BUF_SIZE          64U
#define OW_MIN_FRAME         12U     /* 9 header + 2 crc + 1 end, len == 0 */

/* Bounded busy-wait budgets. delay_ms() is a CPU loop (~3x wall time), so these
 * are iteration counts, not milliseconds; ~200 gives roughly half a second of
 * real time — comparable to the application's 500 ms ONEWIRE_TIMEOUT. */
#define OW_TX_WAIT           200U
#define OW_RX_WAIT           200U

/* -------------------------------------------------------------------------
 * Handles / externs
 * ---------------------------------------------------------------------- */
UART_HandleTypeDef huart3;                 /* CALL_OUT (PC10), downstream relay */
extern UART_HandleTypeDef huart2;          /* CALL_IN  (PA2),  upstream (main.c) */
extern IWDG_HandleTypeDef hiwdg;           /* refreshed during bounded waits     */

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */
static volatile uint8_t  rx_callin_flag  = 0U;   /* frame received on CALL_IN  */
static volatile uint16_t rx_callin_size  = 0U;
static volatile uint8_t  rx_callout_flag = 0U;   /* frame received on CALL_OUT */
static volatile uint16_t rx_callout_size = 0U;
static volatile uint8_t  tx_callin_flag  = 0U;   /* CALL_IN  transmit complete */
static volatile uint8_t  tx_callout_flag = 0U;   /* CALL_OUT transmit complete */

static bool     ow_claimed   = false;            /* have we taken an assigned addr? */
static uint8_t  ow_module_id = 0U;

static uint8_t  callin_rx[OW_BUF_SIZE];          /* CALL_IN  receive buffer  */
static uint8_t  callout_rx[OW_BUF_SIZE];         /* CALL_OUT receive buffer  */
static uint8_t  tx_scratch[OW_BUF_SIZE];         /* frame build / relay scratch */

typedef struct
{
    uint16_t       id;
    uint8_t        type;
    uint8_t        cmd;
    uint8_t        addr;
    uint8_t        reserved;
    uint16_t       len;
    const uint8_t *data;   /* points into the receive buffer */
} ow_packet_t;

/* -------------------------------------------------------------------------
 * Low-level helpers
 * ---------------------------------------------------------------------- */

static bool ow_wait_flag(volatile uint8_t *flag, uint32_t budget)
{
    uint32_t i = 0U;
    while (*flag == 0U)
    {
        if (i >= budget)
        {
            return false;
        }
        delay_ms(1U);
        (void)HAL_IWDG_Refresh(&hiwdg);   /* keep the watchdog alive during waits */
        i++;
    }
    return true;
}

static bool ow_parse(const uint8_t *buf, uint16_t size, ow_packet_t *p)
{
    if (size < OW_MIN_FRAME || buf[0] != OW_START_BYTE)
    {
        return false;
    }
    p->id       = (uint16_t)((buf[1] << 8) | buf[2]);
    p->type     = buf[3];
    p->cmd      = buf[4];
    p->addr     = buf[5];
    p->reserved = buf[6];
    p->len      = (uint16_t)((buf[7] << 8) | buf[8]);

    /* full frame = 9 header + len + 2 crc + 1 end */
    if (((uint32_t)p->len + 12U) > (uint32_t)size)
    {
        return false;
    }
    p->data = &buf[9];

    uint16_t rx_crc = (uint16_t)((buf[9 + p->len] << 8) | buf[10 + p->len]);
    uint16_t calc   = util_crc16(&buf[1], (uint32_t)p->len + 8U);
    if (rx_crc != calc || buf[11 + p->len] != OW_END_BYTE)
    {
        return false;
    }
    return true;
}

static uint16_t ow_build(uint8_t *buf, uint16_t id, uint8_t type, uint8_t cmd,
                         uint8_t addr, uint8_t reserved,
                         const uint8_t *data, uint16_t len)
{
    uint16_t i = 0U;
    buf[i++] = OW_START_BYTE;
    buf[i++] = (uint8_t)(id >> 8);
    buf[i++] = (uint8_t)(id & 0xFFU);
    buf[i++] = type;
    buf[i++] = cmd;
    buf[i++] = addr;
    buf[i++] = reserved;
    buf[i++] = (uint8_t)(len >> 8);
    buf[i++] = (uint8_t)(len & 0xFFU);
    if (len > 0U && data != NULL)
    {
        memcpy(&buf[i], data, len);
        i = (uint16_t)(i + len);
    }
    uint16_t crc = util_crc16(&buf[1], (uint32_t)len + 8U);
    buf[i++] = (uint8_t)(crc >> 8);
    buf[i++] = (uint8_t)(crc & 0xFFU);
    buf[i++] = OW_END_BYTE;
    return i;
}

/* Send a reply upstream on CALL_IN (USART2, half-duplex). */
static bool ow_send_callin(const uint8_t *buf, uint16_t n)
{
    tx_callin_flag = 0U;
    if (HAL_HalfDuplex_EnableTransmitter(&huart2) != HAL_OK)      { return false; }
    if (HAL_UART_Transmit_IT(&huart2, (uint8_t *)buf, n) != HAL_OK) { return false; }
    return ow_wait_flag(&tx_callin_flag, OW_TX_WAIT);
}

/* (Re)arm the CALL_IN receiver for the next upstream frame. */
static void ow_arm_callin(void)
{
    rx_callin_flag = 0U;
    if (HAL_HalfDuplex_EnableReceiver(&huart2) != HAL_OK)          { return; }
    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart2, callin_rx, OW_BUF_SIZE);
}

/* Relay a frame downstream on CALL_OUT (USART3) and capture the reply.
 * Returns the reply length in *rxn (0 on downstream timeout). */
static bool ow_relay_downstream(const ow_packet_t *in, uint16_t *rxn)
{
    uint16_t n = ow_build(tx_scratch, in->id, in->type, in->cmd,
                          in->addr, in->reserved, in->data, in->len);

    /* transmit downstream */
    tx_callout_flag = 0U;
    if (HAL_HalfDuplex_EnableTransmitter(&huart3) != HAL_OK)        { return false; }
    if (HAL_UART_Transmit_IT(&huart3, tx_scratch, n) != HAL_OK)     { return false; }
    if (!ow_wait_flag(&tx_callout_flag, OW_TX_WAIT))                { return false; }

    /* receive the downstream reply */
    rx_callout_flag = 0U;
    if (HAL_HalfDuplex_EnableReceiver(&huart3) != HAL_OK)           { return false; }
    if (HAL_UARTEx_ReceiveToIdle_IT(&huart3, callout_rx, OW_BUF_SIZE) != HAL_OK) { return false; }
    if (!ow_wait_flag(&rx_callout_flag, OW_RX_WAIT))
    {
        *rxn = 0U;   /* no downstream node -> caller reports OW_TIMEOUT */
        return true;
    }
    *rxn = rx_callout_size;
    return true;
}

/* -------------------------------------------------------------------------
 * Command handling
 * ---------------------------------------------------------------------- */

static void ow_handle(const ow_packet_t *p)
{
    uint8_t  txbuf[OW_BUF_SIZE];
    uint16_t txn = 0U;

    if (p->type != OW_TYPE_ONE_WIRE)
    {
        return;   /* not a discovery/clear control frame — ignore silently */
    }

    if (p->cmd == OW_CMD_DISCOVERY)
    {
        if (ow_claimed && p->reserved == ow_module_id)
        {
            /* Idempotent re-claim of our own module_id: re-confirm, keep our
             * assigned I2C address (adopt a changed one if the master reassigns). */
            if (p->addr >= OW_ADDR_MIN && p->addr <= OW_ADDR_MAX)
            {
                I2C_DFU_SetAddress(p->addr);   /* no-op if unchanged */
            }
            uint8_t mode = NODE_MODE_BOOTLOADER;
            txn = ow_build(txbuf, p->id, OW_TYPE_ONEWIRE_RESP, p->cmd,
                           p->addr, p->reserved, &mode, 1U);
        }
        else if (ow_claimed)
        {
            /* Discovery for a different module_id -> relay it to the next node. */
            uint16_t rxn = 0U;
            if (ow_relay_downstream(p, &rxn) && rxn > 0U && rxn <= OW_BUF_SIZE)
            {
                memcpy(txbuf, callout_rx, rxn);   /* pass reply back upstream verbatim */
                txn = rxn;
            }
            else
            {
                /* end of chain / no reply -> tell the master to stop walking */
                txn = ow_build(txbuf, p->id, OW_TYPE_TIMEOUT, p->cmd, 0U, 0U, NULL, 0U);
            }
        }
        else if (p->reserved == 0U || p->addr < OW_ADDR_MIN || p->addr > OW_ADDR_MAX)
        {
            txn = ow_build(txbuf, p->id, OW_TYPE_ERROR, p->cmd, 0U, 0U, NULL, 0U);
        }
        else
        {
            /* Fresh claim: take the assigned address as our I2C DFU listen address
             * and report that we are a bootloader node. */
            ow_module_id = p->reserved;
            ow_claimed   = true;
            I2C_DFU_SetAddress(p->addr);
            uint8_t mode = NODE_MODE_BOOTLOADER;
            txn = ow_build(txbuf, p->id, OW_TYPE_ONEWIRE_RESP, p->cmd,
                           p->addr, p->reserved, &mode, 1U);
        }
    }
    else if (p->cmd == OW_CMD_CLEAR_CONFIG)
    {
        /* Propagate downstream first (only if we relay, i.e. we are claimed), then
         * drop our own assignment and revert to the default 0x72 DFU address. */
        if (ow_claimed)
        {
            uint16_t rxn = 0U;
            (void)ow_relay_downstream(p, &rxn);   /* best-effort; reply discarded */
        }
        ow_claimed   = false;
        ow_module_id = 0U;
        I2C_DFU_SetAddress(DFU_I2C_ADDRESS);
        txn = ow_build(txbuf, p->id, OW_TYPE_ONEWIRE_RESP, p->cmd, 0U, 0U, NULL, 0U);
    }
    else
    {
        txn = ow_build(txbuf, p->id, OW_TYPE_ERROR, p->cmd, 0U, 0U, NULL, 0U);
    }

    if (txn > 0U)
    {
        (void)ow_send_callin(txbuf, txn);
    }
}

/* -------------------------------------------------------------------------
 * USART3 (CALL_OUT) bring-up
 * ---------------------------------------------------------------------- */

static void ow_usart3_init(void)
{
    huart3.Instance                    = USART3;
    huart3.Init.BaudRate               = 115200;
    huart3.Init.WordLength             = UART_WORDLENGTH_8B;
    huart3.Init.StopBits               = UART_STOPBITS_1;
    huart3.Init.Parity                 = UART_PARITY_NONE;
    huart3.Init.Mode                   = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart3.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_HalfDuplex_Init(&huart3) != HAL_OK)   /* calls HAL_UART_MspInit (msp.c) */
    {
        Error_Handler();
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void OW_BL_Init(void)
{
    ow_claimed      = false;
    ow_module_id    = 0U;
    rx_callin_flag  = 0U;
    rx_callout_flag = 0U;
    tx_callin_flag  = 0U;
    tx_callout_flag = 0U;

    ow_usart3_init();
    ow_arm_callin();
}

void OW_BL_Process(void)
{
    if (rx_callin_flag != 0U)
    {
        uint16_t size = rx_callin_size;
        rx_callin_flag = 0U;

        ow_packet_t p;
        if (ow_parse(callin_rx, size, &p))
        {
            ow_handle(&p);
        }

        /* re-arm for the next upstream frame */
        ow_arm_callin();
    }
    else if (huart2.RxState == HAL_UART_STATE_READY)
    {
        /* The CALL_IN listener is idle with no frame pending — this happens if a
         * line/framing error aborted the receive without an idle event. Re-arm so
         * the node cannot silently go deaf and miss the master's discovery. */
        ow_arm_callin();
    }
}

void OW_BL_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        tx_callin_flag = 1U;
    }
    else if (huart->Instance == USART3)
    {
        tx_callout_flag = 1U;
    }
}

/* HAL one-wire RX-idle event: HAL calls this (weak override) for both links. */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance == USART2)
    {
        rx_callin_size = size;
        rx_callin_flag = 1U;
    }
    else if (huart->Instance == USART3)
    {
        rx_callout_size = size;
        rx_callout_flag = 1U;
    }
}
