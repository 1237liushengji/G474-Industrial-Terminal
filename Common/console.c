/**
  ******************************************************************************
  * @file    console.c
  * @brief   Serial command console implementation (v0.2, FreeRTOS era).
  *
  *  Commands (type 'help'):
  *    help                list commands
  *    version             firmware version / node role
  *    stats               comm statistics + reset cause
  *    bitrate <kbps>      switch bus bitrate (125/250/500/1000), both nodes!
  *    sniff <on|off>      bus monitor mode
  *    send <id> [b0..b7]  transmit one frame (id/data hex)
  *    diag <cmd>          UDS diagnostics (gateway role only)
  *
  *  Design notes:
  *   - line editing: echo, backspace (0x08/0x7F), overlong truncation,
  *     \r\n treated as one terminator;
  *   - parsing with strtok + strtol (hex), no dynamic memory;
  *   - output ASCII only, through BSP_Printf (per-task pool + mutex);
  *   - runs in its own task since v0.2 (was main-loop context in v3.0).
  ******************************************************************************
  */
#include "console.h"

#include "app_board_cfg.h"
#include "app_rtos.h"
#include "bridge.h"
#include "bsp_console.h"
#include "bsp_printf.h"
#include "bsp_rs485.h"
#include "log_service.h"
#include "modbus_register.h"
#include "uds.h"
#include "uplink_port.h"
#if (ROLE_IS_GW && (FORM_MODE == FORM_M2))
#include "backfill.h"
#include "mqtt_client.h"
#endif

#include "main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONSOLE_LINE_MAX     40U

/*====================================================================*/
/* Local state                                                         */
/*====================================================================*/
static char    s_line[CONSOLE_LINE_MAX + 1U];
static uint8_t s_lineLen;
static char    s_prevChar;
static uint8_t s_sniffOn;

/*====================================================================*/
/* Helpers                                                             */
/*====================================================================*/
static void Put(const char *s)
{
    BSP_Console_Puts(s);
}

/*====================================================================*/
/* Commands                                                            */
/*====================================================================*/
static void Console_PrintHelp(void)
{
    Put("commands:\r\n");
    Put("  help               this help\r\n");
    Put("  version            fw version / node role\r\n");
    Put("  stats              comm statistics\r\n");
    Put("  bitrate <kbps>     125/250/500/1000, switch on BOTH nodes\r\n");
    Put("  sniff <on|off>     bus monitor mode\r\n");
    Put("  send <id> [b0..b7] send frame, hex, e.g. send 321 11 22\r\n");
#if ROLE_IS_GW
    Put("  cmd <reg> <val>    send 0x301 CMD to the node (decimal PDU addr)\r\n");
    Put("  snap               request a register snapshot now (0x1A0)\r\n");
    Put("  diag <cmd>         UDS: ver/up/stat/dtc/ext/def/tp/led 0|1/raw ..\r\n");
#endif
}

static void Cmd_Version(void)
{
#if ROLE_IS_GW
    const char *role = "gateway";
#else
    const char *role = "node";
#endif
    BSP_Printf("fw v0.2 role=%s build=%s %s bitrate=%lubps\r\n",
               role, __DATE__, __TIME__,
               (unsigned long)CAN_GetBitrate());
}

static void Cmd_Stats(void)
{
    const CAN_Stats *st = CAN_GetStats();

    BSP_Printf("uptime=%lus bitrate=%lubps\r\n",
               (unsigned long)(HAL_GetTick() / 1000U),
               (unsigned long)CAN_GetBitrate());
    BSP_Printf("tx=%lu rx=%lu txErr=%lu ovr=%lu busOff=%lu errPas=%lu txEvtLost=%lu protoErr=%lu\r\n",
               (unsigned long)st->txCount, (unsigned long)st->rxCount,
               (unsigned long)st->txErrors, (unsigned long)st->rxOverruns,
               (unsigned long)st->busOffCount, (unsigned long)st->errPassiveCount,
               (unsigned long)st->txEvtLost, (unsigned long)st->protocolErrors);
    BSP_Printf("txAck=%lu txMaxLatency=%lums drops(485tx=%lu log=%lu)\r\n",
               (unsigned long)st->txAckCount, (unsigned long)st->txMaxLatencyMs,
               (unsigned long)RS485_GetTxDropCount(),
               (unsigned long)LOG_SVC_GetDropCount());
    BSP_Printf("dbg poll=%lu hx=%lu cmd=%lu resp=%lu\r\n",
               (unsigned long)g_dbgPoll, (unsigned long)g_dbgHx,
               (unsigned long)g_dbgCmd, (unsigned long)g_dbgResp);
    BSP_Printf("link mask=0x%02lX peer=%u hb_age=%lums\r\n",
               (unsigned long)MB_REG_GetHolding(MB_REG_HOLD_LINK_MASK),
               (unsigned)Bridge_PeerUp(),
               (unsigned long)Bridge_GetHbAgeMs());
#if ROLE_IS_GW
    BSP_Printf("snap ok=%lu err=%lu age=%lums  cmd_result=%u/%u\r\n",
               (unsigned long)Bridge_GetSnapOkCount(),
               (unsigned long)Bridge_GetSnapErrCount(),
               (unsigned long)Bridge_GetSnapAgeMs(),
               (unsigned)Bridge_GetLastCmdResult(),
               (unsigned)Bridge_GetLastCmdExc());
    BSP_Printf("uplink frames=%lu drops=%lu hash_mismatch=%lu\r\n",
               (unsigned long)Uplink_GetFrameCount(),
               (unsigned long)Uplink_GetDropCount(),
               (unsigned long)Bridge_GetSnapHashMismatch());
#if (FORM_MODE == FORM_M2)
    BSP_Printf("mqtt esp=%u conn=%u recon=%lu pubfail=%lu  backfill backlog=%lu replayed=%lu lost=%lu\r\n",
               (unsigned)Mqtt_LinkBits(),
               (unsigned)Mqtt_IsConnected(),
               (unsigned long)Mqtt_GetReconnectCount(),
               (unsigned long)Mqtt_GetPublishFailCount(),
               (unsigned long)Backfill_GetBacklog(),
               (unsigned long)Backfill_GetReplayedCount(),
               (unsigned long)Backfill_GetLostCount());
#endif
#endif
    BSP_Printf("wdt task_ovr=%lu (must stay 0) reset=0x%02lX (bit0 pin/1 sw/2 iwdg/4 bor)\r\n",
               (unsigned long)App_GetTaskOvr(),
               (unsigned long)LOG_SVC_GetResetFlags());
}

static void Cmd_Bitrate(char *args)
{
    long kbps;

    if ((args == NULL) || (*args == '\0'))
    {
        BSP_Printf("usage: bitrate <125|250|500|1000>, now %lubps\r\n",
                   (unsigned long)CAN_GetBitrate());
        return;
    }

    kbps = strtol(args, NULL, 10);
    if ((kbps != 125L) && (kbps != 250L) && (kbps != 500L) && (kbps != 1000L))
    {
        Put("only 125/250/500/1000 kbps supported\r\n");
        return;
    }

    if (CAN_SetBitrate((uint32_t)kbps * 1000U) == HAL_OK)
    {
        BSP_Printf("switched to %ldkbps (set the SAME on the other node!)\r\n", kbps);
    }
    else
    {
        Put("switch failed\r\n");
    }
}

static void Cmd_Sniff(char *args)
{
    if ((args != NULL) && (strncmp(args, "on", 2U) == 0))
    {
        if (CAN_SetPromiscuous(true) == HAL_OK)
        {
            s_sniffOn = 1U;
            Put("sniff ON, format: RX <ms> <ID> <DLC> <data...>\r\n");
        }
    }
    else if ((args != NULL) && (strncmp(args, "off", 3U) == 0))
    {
        if (CAN_SetPromiscuous(false) == HAL_OK)
        {
            s_sniffOn = 0U;
            Put("sniff OFF\r\n");
        }
    }
    else
    {
        Put("usage: sniff <on|off>\r\n");
    }
}

static void Cmd_Send(char *args)
{
    char   *tok;
    long    id;
    uint8_t data[CAN_PAYLOAD_LEN];
    uint8_t len = 0U;

    if ((args == NULL) || (*args == '\0'))
    {
        Put("usage: send <id> [b0..b7]  (hex)\r\n");
        return;
    }

    tok = strtok(args, " ");
    if (tok == NULL)
    {
        return;
    }
    id = strtol(tok, NULL, 16);
    if ((id < 0L) || (id > 0x7FFL))
    {
        Put("bad id\r\n");
        return;
    }

    while (((tok = strtok(NULL, " ")) != NULL) && (len < CAN_PAYLOAD_LEN))
    {
        data[len++] = (uint8_t)(strtol(tok, NULL, 16) & 0xFFL);
    }

    if (CAN_Send((uint32_t)id, data, len) == HAL_OK)
    {
        BSP_Printf("sent id=0x%lX len=%u\r\n", id, (unsigned)len);
    }
    else
    {
        Put("send failed (tx fifo full?)\r\n");
    }
}

#if ROLE_IS_GW
static void Cmd_Cmd(char *args)
{
    long reg;
    long val;
    char *tok;

    if ((args == NULL) || (*args == '\0'))
    {
        Put("usage: cmd <reg> <val>   (decimal PDU reg 0..63, value 0..65535)\r\n");
        return;
    }
    tok = strtok(args, " ");
    if (tok == NULL)
    {
        return;
    }
    reg = strtol(tok, NULL, 10);
    tok = strtok(NULL, " ");
    if (tok == NULL)
    {
        Put("usage: cmd <reg> <val>\r\n");
        return;
    }
    val = strtol(tok, NULL, 10);

    if ((reg < 0L) || (reg > 63L) || (val < 0L) || (val > 65535L))
    {
        Put("out of range\r\n");
        return;
    }
    if (Bridge_RequestCmd((uint16_t)reg, (uint16_t)val) != 0U)
    {
        BSP_Printf("cmd queued: reg=%ld val=%ld (watch for cmd_ack)\r\n", reg, val);
    }
    else
    {
        Put("cmd busy (previous command still in flight)\r\n");
    }
}

static void Cmd_Snap(void)
{
    Bridge_RequestSnapshot();
    Put("snapshot requested (watch 'link' counters in stats)\r\n");
}

static void Cmd_Diag(char *args)
{
    uint8_t      req[ISOTP_MAX_MSG];
    uint16_t     len = 0U;
    IsoTp_Result r;

    if ((args == NULL) || (*args == '\0'))
    {
        Put("usage: diag ver|up|stat|dtc|ext|def|tp|led 0|1|raw <hex..>\r\n");
        return;
    }

    if (strncmp(args, "ver", 3U) == 0)
    {
        req[0] = 0x22U; req[1] = 0xF0U; req[2] = 0x00U; len = 3U;
    }
    else if (strncmp(args, "up", 2U) == 0)
    {
        req[0] = 0x22U; req[1] = 0xF0U; req[2] = 0x01U; len = 3U;
    }
    else if (strncmp(args, "stat", 4U) == 0)
    {
        req[0] = 0x22U; req[1] = 0xF0U; req[2] = 0x02U; len = 3U;
    }
    else if (strncmp(args, "dtc", 3U) == 0)
    {
        req[0] = 0x19U; req[1] = 0x01U; req[2] = 0xFFU; len = 3U;
    }
    else if (strncmp(args, "ext", 3U) == 0)
    {
        req[0] = 0x10U; req[1] = 0x03U; len = 2U;
    }
    else if (strncmp(args, "def", 3U) == 0)
    {
        req[0] = 0x10U; req[1] = 0x01U; len = 2U;
    }
    else if (strncmp(args, "tp", 2U) == 0)
    {
        req[0] = 0x3EU; req[1] = 0x00U; len = 2U;
    }
    else if (strncmp(args, "led", 3U) == 0)
    {
        const char *v = args + 3;
        while ((*v == ' ') && (*v != '\0')) { v++; }
        if ((*v != '0') && (*v != '1'))
        {
            Put("usage: diag led <0|1>\r\n");
            return;
        }
        req[0] = 0x2EU; req[1] = 0xF0U; req[2] = 0x10U;
        req[3] = (uint8_t)(*v - '0');
        len = 4U;
    }
    else if (strncmp(args, "raw", 3U) == 0)
    {
        char *tok = strtok(args + 3, " ");
        while ((tok != NULL) && (len < (uint16_t)sizeof(req)))
        {
            req[len++] = (uint8_t)(strtol(tok, NULL, 16) & 0xFFL);
            tok = strtok(NULL, " ");
        }
        if (len == 0U)
        {
            Put("raw needs at least one hex byte\r\n");
            return;
        }
    }
    else
    {
        Put("unknown diag subcommand (try: help)\r\n");
        return;
    }

    r = Uds_ClientRequest(req, len);
    if (r != ISOTP_OK)
    {
        BSP_Printf("diag request rejected: %d\r\n", (int)r);
    }
}
#endif /* ROLE_IS_GW */

static void Console_Execute(char *line)
{
    char *cmd  = strtok(line, " ");
    char *args = strtok(NULL, "");

    if (cmd == NULL)
    {
        return;
    }

    if (strcmp(cmd, "help") == 0)
    {
        Console_PrintHelp();
    }
    else if (strcmp(cmd, "version") == 0)
    {
        Cmd_Version();
    }
    else if (strcmp(cmd, "stats") == 0)
    {
        Cmd_Stats();
    }
    else if (strcmp(cmd, "bitrate") == 0)
    {
        Cmd_Bitrate(args);
    }
    else if (strcmp(cmd, "sniff") == 0)
    {
        Cmd_Sniff(args);
    }
    else if (strcmp(cmd, "send") == 0)
    {
        Cmd_Send(args);
    }
#if ROLE_IS_GW
    else if (strcmp(cmd, "cmd") == 0)
    {
        Cmd_Cmd(args);
    }
    else if (strcmp(cmd, "snap") == 0)
    {
        Cmd_Snap();
    }
    else if (strcmp(cmd, "diag") == 0)
    {
        Cmd_Diag(args);
    }
#endif
    else
    {
        BSP_Printf("unknown cmd: %s (try 'help')\r\n", cmd);
    }
}

/*====================================================================*/
/* Public API                                                          */
/*====================================================================*/
void Console_Init(void)
{
#if ROLE_IS_GW
    const char *role = "gateway";
#else
    const char *role = "node";
#endif
    BSP_Printf("\r\n=== STM32G474 acquisition terminal v0.2 | role:%s | %s %s ===\r\n",
               role, __DATE__, __TIME__);
    Put("type 'help' for commands\r\n> ");
}

uint8_t Console_SniffEnabled(void)
{
    return s_sniffOn;
}

void Console_PrintFrame(const CAN_RxMsg *msg)
{
    char     line[8U * 4U + 32U];      /* prefix + up to 8 data bytes */
    int      n;
    int      m;
    uint8_t  i;

    if (msg == NULL)
    {
        return;
    }

    m = snprintf(line, sizeof(line), "RX %lu %03lX %u",
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)msg->id,
                 (unsigned)msg->len);
    if (m < 0)
    {
        return;
    }
    n = m;

    for (i = 0U; (i < msg->len) && (n < (int)sizeof(line) - 4); i++)
    {
        m = snprintf(&line[n], sizeof(line) - (size_t)n, " %02X", msg->data[i]);
        if (m < 0)
        {
            break;
        }
        n += m;                                    /* cannot truncate: >=4 free */
    }

    if (n > (int)sizeof(line) - 3)
    {
        n = (int)sizeof(line) - 3;
    }
    line[n]     = '\r';
    line[n + 1] = '\n';
    line[n + 2] = '\0';

    Put(line);
}

void Console_Task(void)
{
    uint8_t c;

    while (BSP_Console_GetChar(&c) != 0)
    {
        char ch = (char)c;

        if ((ch == '\r') || (ch == '\n'))
        {
            uint8_t isLoneLf = (uint8_t)((ch == '\n') && (s_prevChar == '\r'));

            s_prevChar = ch;
            if (isLoneLf != 0U)
            {
                continue;                              /* second half of \r\n */
            }

            Put("\r\n");
            s_line[s_lineLen] = '\0';
            if (s_lineLen != 0U)
            {
                Console_Execute(s_line);
            }
            s_lineLen = 0U;
            Put("> ");
        }
        else
        {
            s_prevChar = ch;

            if ((ch == 0x08) || (ch == 0x7F))          /* backspace / delete */
            {
                if (s_lineLen != 0U)
                {
                    s_lineLen--;
                    Put("\b \b");
                }
            }
            else if ((ch >= 0x20) && (ch < 0x7F))      /* printable: echo */
            {
                if (s_lineLen < CONSOLE_LINE_MAX)
                {
                    s_line[s_lineLen] = ch;
                    s_lineLen++;
                    BSP_Console_Send((const uint8_t *)&ch, 1U);
                }
            }
            else
            {
                /* other control characters: ignored */
            }
        }
    }
}
