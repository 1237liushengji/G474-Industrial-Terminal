/**
  ******************************************************************************
  * @file    dp_table.c
  * @brief   Data-point table + contract hash (docs/03 §3, §6).
  *
  *  The canonical hash string is built row by row with a streaming CRC - the
  *  full string (~1.1 KB) never exists in RAM.  Format (frozen, v0.3):
  *
  *      "key:PP:SS:RR\n" per row, PP = 2 upper-case hex, SS = single digit,
  *      RR = "RO"/"RW"; no separator/trailing bytes.
  *
  *  Example of one row: "temp:00:1:RO"
  ******************************************************************************
  */
#include "dp_table.h"
#include "modbus_crc.h"

/*====================================================================*/
/* The table (PDU ascending; 0x0F hole intentionally absent)           */
/*====================================================================*/
const dp_entry_t g_dp_table[] =
{
    /* 0x00-0x08  live measurements / status / comm counters [v1 frozen] */
    { "temp",              0x00U, 1U, DP_RW_RO },
    { "humi",              0x01U, 1U, DP_RW_RO },
    { "volt",              0x02U, 2U, DP_RW_RO },
    { "curr",              0x03U, 2U, DP_RW_RO },
    { "status",            0x04U, 0U, DP_RW_RO },
    { "errcode",           0x05U, 0U, DP_RW_RO },
    { "rx_cnt",            0x06U, 0U, DP_RW_RO },
    { "tx_cnt",            0x07U, 0U, DP_RW_RO },
    { "crc_err",           0x08U, 0U, DP_RW_RO },

    /* 0x09-0x0E  field-editable configuration (24C02 persisted)          */
    { "temp_limit",        0x09U, 1U, DP_RW_RW },
    { "volt_limit",        0x0AU, 2U, DP_RW_RW },
    { "sample_period_ms",  0x0BU, 0U, DP_RW_RW },
    { "dev_mode",          0x0CU, 0U, DP_RW_RW },
    { "slave_id",          0x0DU, 0U, DP_RW_RW },
    { "baud_idx",          0x0EU, 0U, DP_RW_RW },

    /* 0x0F        sentinel hole - not a data point (always exception 0x02;
     *             a block access CROSSING 0x0F is rejected the same way)  */

    /* 0x10-0x17  gateway / CAN backbone counters (v0.3 new)              */
    { "can_tx_ack",        0x10U, 0U, DP_RW_RO },
    { "can_rx_ovr",        0x11U, 0U, DP_RW_RO },
    { "can_busoff",        0x12U, 0U, DP_RW_RO },
    { "can_tx_lat_max",    0x13U, 0U, DP_RW_RO },
    { "mb_timeout",        0x14U, 0U, DP_RW_RO },
    { "mb_retry",          0x15U, 0U, DP_RW_RO },
    { "mb_exception",      0x16U, 0U, DP_RW_RO },
    { "link_mask",         0x17U, 0U, DP_RW_RO },

    /* 0x18-0x1F  contract metadata (v0.3 new)                            */
    { "contract_hash",     0x18U, 0U, DP_RW_RO },
    { "contract_ver",      0x19U, 0U, DP_RW_RO },
    { "form_mode",         0x1AU, 0U, DP_RW_RO },
    { "fw_ver",            0x1BU, 0U, DP_RW_RO },
    { "hb_seq",            0x1CU, 0U, DP_RW_RO },
    { "uptime_s",          0x1DU, 0U, DP_RW_RO },
    { "free_heap",         0x1EU, 0U, DP_RW_RO },
    { "task_ovr",          0x1FU, 0U, DP_RW_RO },

    /* 0x20-0x2F  extended measurements (v0.3 new)                        */
    { "adc1_raw",          0x20U, 0U, DP_RW_RO },
    { "mcu_temp",          0x21U, 1U, DP_RW_RO },
    { "vref_mv",           0x22U, 0U, DP_RW_RO },
    { "rsv_meas0",         0x23U, 0U, DP_RW_RO },
    { "rsv_meas1",         0x24U, 0U, DP_RW_RO },
    { "rsv_meas2",         0x25U, 0U, DP_RW_RO },
    { "rsv_meas3",         0x26U, 0U, DP_RW_RO },
    { "rsv_meas4",         0x27U, 0U, DP_RW_RO },
    { "rsv_meas5",         0x28U, 0U, DP_RW_RO },
    { "rsv_meas6",         0x29U, 0U, DP_RW_RO },
    { "rsv_meas7",         0x2AU, 0U, DP_RW_RO },
    { "rsv_meas8",         0x2BU, 0U, DP_RW_RO },
    { "rsv_meas9",         0x2CU, 0U, DP_RW_RO },
    { "rsv_meas10",        0x2DU, 0U, DP_RW_RO },
    { "rsv_meas11",        0x2EU, 0U, DP_RW_RO },
    { "rsv_meas12",        0x2FU, 0U, DP_RW_RO },

    /* 0x30-0x3F  downlink command shadow registers (v0.3 new, not saved) */
    { "cmd_write_reg",     0x30U, 0U, DP_RW_RW },
    { "cmd_write_val",     0x31U, 0U, DP_RW_RW },
    { "cmd_id",            0x32U, 0U, DP_RW_RW },
    { "cmd_result",        0x33U, 0U, DP_RW_RO },
    { "cmd_exccode",       0x34U, 0U, DP_RW_RO },
    { "rsv_ctrl0",         0x35U, 0U, DP_RW_RW },
    { "rsv_ctrl1",         0x36U, 0U, DP_RW_RW },
    { "rsv_ctrl2",         0x37U, 0U, DP_RW_RW },
};

const uint16_t g_dp_table_len =
    (uint16_t)(sizeof(g_dp_table) / sizeof(g_dp_table[0]));

/*====================================================================*/
/* Contract hash                                                       */
/*====================================================================*/
static const char s_hex_digits[] = "0123456789ABCDEF";

static uint16_t Dp_Hash_Stream(void)
{
    uint16_t crc = MODBUS_CRC16_Init();
    uint16_t i;

    for (i = 0U; i < g_dp_table_len; i++)
    {
        const dp_entry_t *e = &g_dp_table[i];
        const char *k = e->key;

        if (i != 0U)
        {
            crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)'\n');
        }

        while (*k != '\0')
        {
            crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)*k);
            k++;
        }

        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)':');
        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)s_hex_digits[(e->pdu >> 4U) & 0x0FU]);
        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)s_hex_digits[e->pdu & 0x0FU]);
        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)':');
        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)('0' + e->scale));
        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)':');
        crc = MODBUS_CRC16_UpdateByte(crc, (uint8_t)'R');
        crc = MODBUS_CRC16_UpdateByte(crc,
                                      (uint8_t)((e->rw == DP_RW_RW) ? 'W' : 'O'));
    }

    return crc;
}

uint16_t DP_Table_ContractHash(void)
{
    static uint16_t s_cached = 0U;
    static uint8_t  s_done = 0U;

    if (s_done == 0U)
    {
        s_cached = Dp_Hash_Stream();
        s_done = 1U;
    }
    return s_cached;
}
