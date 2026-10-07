/**
  ******************************************************************************
  * @file    isotp.c
  * @brief   ISO 15765-2（ISO-TP）传输层精简实现（经典CAN，8字节帧）
  *
  * 状态机：
  *   接收方向： IDLE --SF--> 投递； IDLE --FF--> RX_COLLECT(回FC) --CF...--> 投递
  *   发送方向： IDLE --SF--> 直接发送
  *              IDLE --FF--> TX_WAIT_FC --FC(CTS)--> TX_SENDING --CF块完且BS>0--> TX_WAIT_FC
  *   超时： TX_WAIT_FC超N_Bs复位； RX_COLLECT超N_Cr复位；均经errCb上报。
  *
  *  v0.4: 多实例（isotp_ctx_t）实现，算法与状态语义与 v3.0 单例版完全一致。
  *        调用者的上下文约束：同一 ctx 只允许一个任务在跑 IsoTp_Task；
  *        IsoTp_OnCanFrame 由 CAN 分发任务调用。本系统两个持有者
  *        （uds.c 的 can_task 上下文 / bridge.c 的 bridge_task 上下文）互不交叉。
  *
  * 参考对标：lishen2/isotp-c（平台无关C实现），本文为自研精简版，
  * 状态与参数命名对照ISO 15765-2标准术语。
  ******************************************************************************
  */
#include "isotp.h"
#include "can_drv.h"
#include <string.h>

/**
  * @brief  初始化ISO-TP通道
  */
void IsoTp_Init(isotp_ctx_t *ctx, uint32_t rxId, uint32_t txId,
                IsoTp_RxCallback rxCb, IsoTp_ErrCallback errCb)
{
  if (ctx == NULL)
  {
    return;
  }
  (void)memset(ctx, 0, sizeof(*ctx));
  ctx->rx_id  = rxId;
  ctx->tx_id  = txId;
  ctx->rx_cb  = rxCb;
  ctx->err_cb = errCb;
  ctx->rx_state = ISOTP_RX_IDLE;
  ctx->tx_state = ISOTP_TX_IDLE;
}

uint8_t IsoTp_IsBusy(const isotp_ctx_t *ctx)
{
  return (ctx != NULL) && (ctx->tx_state != ISOTP_TX_IDLE);
}

/**
  * @brief  发送一个3字节FC流控帧到总线（结果不关注：FC丢失由对端超时兜底）
  */
static void IsoTp_SendFc(const isotp_ctx_t *ctx, uint8_t fs, uint8_t bs, uint8_t stmin)
{
  uint8_t frame[3];
  frame[0] = fs;
  frame[1] = bs;
  frame[2] = stmin;
  (void)CAN_Send(ctx->tx_id, frame, 3U);
}

/**
  * @brief  发起一次应用报文发送
  */
IsoTp_Result IsoTp_Send(isotp_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
  uint8_t frame[CAN_PAYLOAD_LEN];

  if ((ctx == NULL) || (data == NULL) || (len == 0U))
  {
    return ISOTP_TOOLONG;
  }
  if (len > ISOTP_MAX_MSG)
  {
    return ISOTP_TOOLONG;
  }
  if (ctx->tx_state != ISOTP_TX_IDLE)
  {
    return ISOTP_BUSY;
  }

  if (len <= 7U)
  {
    /* 单帧SF */
    frame[0] = (uint8_t)(0x0U | len);
    for (uint16_t i = 0U; i < len; i++)
    {
      frame[1U + i] = data[i];
    }
    (void)memset(&frame[1U + len], 0, (size_t)(7U - len));
    if (CAN_Send(ctx->tx_id, frame, CAN_PAYLOAD_LEN) != HAL_OK)
    {
      return ISOTP_BUSY;                          /* CAN驱动忙（Tx FIFO满） */
    }
    return ISOTP_OK;
  }

  /* 首帧FF */
  memcpy(ctx->tx_buf, data, len);
  ctx->tx_len      = len;
  ctx->tx_sent     = 6U;                          /* FF携带前6字节 */
  ctx->tx_next_sn  = 1U;                          /* FF后的首个CF SN=1 */
  frame[0]     = (uint8_t)(0x10U | ((len >> 8) & 0x0FU));
  frame[1]     = (uint8_t)(len & 0xFFU);
  for (uint8_t i = 0U; i < 6U; i++)
  {
    frame[2U + i] = data[i];
  }
  if (CAN_Send(ctx->tx_id, frame, CAN_PAYLOAD_LEN) != HAL_OK)
  {
    return ISOTP_BUSY;
  }

  ctx->tx_state = ISOTP_TX_WAIT_FC;
  ctx->tx_last_tick = HAL_GetTick();
  return ISOTP_OK;
}

/**
  * @brief  接收方向：处理一帧（已确认ID匹配）
  */
static void IsoTp_RxFrame(isotp_ctx_t *ctx, const uint8_t *data, uint8_t len)
{
  uint8_t pci = (uint8_t)(data[0] >> 4U);

  if (pci == 0x0U)                                /* SF */
  {
    uint8_t sfLen = (uint8_t)(data[0] & 0x0FU);
    if ((sfLen >= 1U) && (sfLen <= 7U) && (sfLen <= (uint8_t)(len - 1U)))
    {
      ctx->rx_state = ISOTP_RX_IDLE;              /* SF中断任何未完成接收 */
      if (ctx->rx_cb != NULL)
      {
        ctx->rx_cb(&data[1], sfLen);
      }
    }
  }
  else if (pci == 0x1U)                           /* FF */
  {
    uint16_t total = (uint16_t)(((uint16_t)(data[0] & 0x0FU) << 8) | data[1]);
    if (total > ISOTP_MAX_MSG)
    {
      /* 超出本机缓冲：回FC溢出，复位 */
      IsoTp_SendFc(ctx, 0x32U, 0U, 0U);           /* FS=OVFLW */
      ctx->rx_state = ISOTP_RX_IDLE;
      return;
    }

    ctx->rx_expect = total;
    ctx->rx_got    = 6U;                          /* FF携带前6字节 */
    for (uint8_t i = 0U; i < 6U; i++)
    {
      ctx->rx_buf[i] = data[2U + i];
    }
    ctx->rx_next_sn  = 1U;
    ctx->rx_state    = ISOTP_RX_COLLECT;
    ctx->rx_last_tick = HAL_GetTick();

    /* FC: FS=CTS, BS=0(不限块), STmin=2ms */
    IsoTp_SendFc(ctx, 0x30U, 0x00U, 0x02U);
  }
  else if (pci == 0x2U)                           /* CF */
  {
    if (ctx->rx_state != ISOTP_RX_COLLECT)
    {
      return;                                     /* 无接收事务，丢弃 */
    }

    uint8_t sn = (uint8_t)(data[0] & 0x0FU);
    if (sn != ctx->rx_next_sn)
    {
      ctx->rx_state = ISOTP_RX_IDLE;
      if (ctx->err_cb != NULL)
      {
        ctx->err_cb(ISOTP_SN_ERROR);
      }
      return;
    }
    ctx->rx_next_sn = (uint8_t)((ctx->rx_next_sn + 1U) & 0x0FU);

    for (uint8_t i = 1U; (i < len) && (ctx->rx_got < ctx->rx_expect); i++)
    {
      ctx->rx_buf[ctx->rx_got++] = data[i];
    }
    ctx->rx_last_tick = HAL_GetTick();

    if (ctx->rx_got >= ctx->rx_expect)
    {
      ctx->rx_state = ISOTP_RX_IDLE;
      if (ctx->rx_cb != NULL)
      {
        ctx->rx_cb(ctx->rx_buf, ctx->rx_expect);
      }
    }
  }
  else if (pci == 0x3U)                           /* FC（对端对我方多帧发送的流控） */
  {
    if (ctx->tx_state != ISOTP_TX_WAIT_FC)
    {
      return;
    }

    uint8_t fs = (uint8_t)(data[0] & 0x0FU);
    if (fs == 0x0U)                               /* CTS：可以发送 */
    {
      ctx->tx_block_left = data[1];               /* BS=0表示不限块 */
      uint8_t stmin = data[2];
      if ((stmin >= 0xF1U) && (stmin <= 0xF9U))   /* 100µs单位→按1ms处理 */
      {
        ctx->tx_stmin_ms = 1U;
      }
      else if (stmin <= 0x7FU)
      {
        ctx->tx_stmin_ms = stmin;
      }
      else
      {
        ctx->tx_stmin_ms = 7U;
      }
      ctx->tx_state   = ISOTP_TX_SENDING;
      ctx->tx_last_tick = HAL_GetTick();
    }
    else if (fs == 0x1U)                          /* Wait：刷新N_Bs继续等 */
    {
      ctx->tx_last_tick = HAL_GetTick();
    }
    else                                          /* OVFLW */
    {
      ctx->tx_state = ISOTP_TX_IDLE;
      if (ctx->err_cb != NULL)
      {
        ctx->err_cb(ISOTP_OVERFLOW);
      }
    }
  }
  else
  {
    /* 保留PCI，忽略 */
  }
}

/**
  * @brief  喂入一帧CAN报文（ID不匹配则忽略）
  */
uint8_t IsoTp_OnCanFrame(isotp_ctx_t *ctx, uint32_t id,
                         const uint8_t *data, uint8_t len)
{
  if ((ctx == NULL) || (data == NULL))
  {
    return 0U;
  }
  if ((id == ctx->rx_id) && (len >= 1U) && (len <= CAN_PAYLOAD_LEN))
  {
    IsoTp_RxFrame(ctx, data, len);
    return 1U;
  }
  return 0U;
}

/**
  * @brief  周期轮询：发送STmin节拍 + 各状态超时
  */
void IsoTp_Task(isotp_ctx_t *ctx)
{
  uint32_t now;

  if (ctx == NULL)
  {
    return;
  }
  now = HAL_GetTick();

  /* 发送方向 */
  if (ctx->tx_state == ISOTP_TX_WAIT_FC)
  {
    if ((now - ctx->tx_last_tick) > ISOTP_TIMEOUT_MS)  /* N_Bs超时 */
    {
      ctx->tx_state = ISOTP_TX_IDLE;
      if (ctx->err_cb != NULL)
      {
        ctx->err_cb(ISOTP_TIMEOUT);
      }
    }
  }
  else if (ctx->tx_state == ISOTP_TX_SENDING)
  {
    if ((now - ctx->tx_last_tick) >= ctx->tx_stmin_ms) /* STmin节拍到 */
    {
      uint8_t frame[CAN_PAYLOAD_LEN];
      uint8_t n = 0U;

      frame[n++] = (uint8_t)(0x20U | ctx->tx_next_sn);
      ctx->tx_next_sn = (uint8_t)((ctx->tx_next_sn + 1U) & 0x0FU);
      while ((n < CAN_PAYLOAD_LEN) && (ctx->tx_sent < ctx->tx_len))
      {
        frame[n++] = ctx->tx_buf[ctx->tx_sent++];
      }
      (void)memset(&frame[n], 0, (size_t)(CAN_PAYLOAD_LEN - n));

      if (CAN_Send(ctx->tx_id, frame, CAN_PAYLOAD_LEN) == HAL_OK)
      {
        ctx->tx_last_tick = now;

        if (ctx->tx_block_left > 0U)
        {
          ctx->tx_block_left--;
          if ((ctx->tx_block_left == 0U) && (ctx->tx_sent < ctx->tx_len))
          {
            ctx->tx_state = ISOTP_TX_WAIT_FC;      /* 块发完，等下一个FC */
            ctx->tx_last_tick = now;
          }
        }

        if (ctx->tx_sent >= ctx->tx_len)
        {
          ctx->tx_state = ISOTP_TX_IDLE;           /* 发送完成 */
        }
      }
      else
      {
        ctx->tx_last_tick = now;                   /* CAN忙：下个节拍重试该CF */
      }
    }
  }
  else
  {
    /* ISOTP_TX_IDLE */
  }

  /* 接收方向超时 N_Cr */
  if (ctx->rx_state == ISOTP_RX_COLLECT)
  {
    if ((now - ctx->rx_last_tick) > ISOTP_TIMEOUT_MS)
    {
      ctx->rx_state = ISOTP_RX_IDLE;
      if (ctx->err_cb != NULL)
      {
        ctx->err_cb(ISOTP_TIMEOUT);
      }
    }
  }
}
