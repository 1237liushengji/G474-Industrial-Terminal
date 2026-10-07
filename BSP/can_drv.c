/**
  ******************************************************************************
  * @file    can.c
  * @brief   Dual-node CAN driver implementation (classic CAN, RX FIFO0 + IT0)
  *          实例与引脚来自 app_board_cfg.h: FDCAN1 on PD0/PD1 (AF9)
  *
  * 设计说明（企业级要求）：
  *   1. 发送采用Tx FIFO（深度3），发送前查询剩余空间，不阻塞应用层；
  *      发送启用Tx Event FIFO（MessageMarker标记），中断里逐帧确认
  *      "已真正送达总线"并测量入队->确认延迟（txAckCount/txMaxLatencyMs）；
  *   2. 接收采用RX FIFO0 + FDCAN1中断线0，回调中排空FIFO写入16深度
  *      无锁SPSC环形缓冲（中断只写写指针、主循环只读写指针，Cortex-M
  *      单核下配合DMB屏障安全），应用层CAN_PollRx()轮询取走；
  *   3. bus-off采用指数退避自动恢复：1s起步翻倍、上限30s（CAN_Task调度），
  *      任一帧发送确认成功即复位退避——避免故障总线上的重连风暴；
  *   4. 上电自检CAN_SelfTest()：内部回环自发自收，验证软件链路完整性；
  *   5. 支持运行时重配置（CAN_Configure）：波特率切换、混杂模式切换，
  *      重配置流程 = Stop -> 重填参数 -> Init -> 过滤器 -> Start -> 重挂中断；
  *   6. 所有HAL返回值均检查并统计，异常不静默。
  *
  * 参考实现：STMicroelectronics/STM32CubeG4 官方示例
  *           Projects/STM32G474E-EVAL/Examples/FDCAN/FDCAN_Classic_Frame_Networking
  *           （本项目HAL库版本V1.2.3，与该示例API完全一致）
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics（驱动调用模式）
  *
  ******************************************************************************
  */
#include "can_drv.h"
#include "isotp.h"
#include "can_contract.h"
#include "FreeRTOS.h"
#include "queue.h"

/*--------------------------------------- 模块配置 --------------------------------------*/
/* CAN_RX_Q_LEN（FreeRTOS 队列深度，元素为 CAN_RxMsg）与 CAN_BACKOFF_INIT_MS /
 * CAN_BACKOFF_MAX_MS 均由 app_board_cfg.h 给出。
 * v0.2：接收缓冲由 16 深度 SPSC 环形缓冲改为 FreeRTOS 队列（docs/02 §5
 * 决策 1）。中断回调只做 xQueueSendFromISR + portYIELD_FROM_ISR，不做解析。
 * v0.4：硬件白名单滤波器由 2 条扩为 4 条（docs/03 §4.5），帧 ID 来自
 * Protocol/can_contract.h；v3.0 的 0x321/0x322 应用帧已废弃。 */

/*--------------------------------------- 硬件滤波器白名单 --------------------------------------*/
/* 掩码语义（FDCAN 经典帧）：SFMR1 中置 1 的位表示"必须相等"。
 * 默认全局过滤器 = REJECT，只有下表放行的 ID 段进入 RX FIFO0。 */
typedef struct
{
  uint32_t id;
  uint32_t mask;
} can_filter_entry_t;

#if ROLE_IS_GW
/* 网关：遥测/状态/计数/快照数据段(0x100-0x1FF)、事件/CMD_ACK(0x200-0x3FF)、
 *       节点心跳(0x500-0x5FF)、UDS 应答 0x7E8 */
static const can_filter_entry_t s_rxFilterTable[4] =
{
  { 0x100U,                   0x700U },
  { 0x200U,                   0x600U },
  { 0x500U,                   0x700U },
  { ISOTP_DIAG_RESP_ID,       0x7FFU },
};
#else
/* 节点：CMD(0x300-0x3FF)、快照请求+该方向FC 0x1A0(精确)、网关心跳(0x500-0x5FF)、
 *       UDS 请求 0x7E0 */
static const can_filter_entry_t s_rxFilterTable[4] =
{
  { 0x300U,                   0x700U },
  { CANC_ID_SNAPSHOT_REQ,     0x7FFU },
  { 0x500U,                   0x700U },
  { ISOTP_DIAG_REQ_ID,        0x7FFU },
};
#endif

/*--------------------------------------- 模块内部变量 --------------------------------------*/
FDCAN_HandleTypeDef hfdcan;                           /* 实例由 CAN_PERIPH 决定（ISR 引用） */

static QueueHandle_t s_rx_queue;                      /* RX FIFO0 -> can_task 交接 */

static uint32_t     s_txTick[256];                    /* MessageMarker -> 入队时刻（测延迟） */
static uint8_t      s_txMarker;                       /* 下一帧的MessageMarker */

static CAN_Stats    s_stats;                          /* 通信统计（调试器可观察） */

static bool         s_promisc;                        /* 混杂模式：接收所有ID */
static bool         s_loopbackSelfTest;               /* 运行时自检：内部回环 */
static uint32_t     s_bitrate = CAN_BITRATE;          /* 当前位速率 */

static volatile uint32_t s_busOffBackoffMs = CAN_BACKOFF_INIT_MS; /* 当前退避时长 */
static volatile uint32_t s_busOffDueTick;             /* 待执行的恢复时刻（0=无待恢复） */

/* 波特率-分频查找表：FDCAN内核时钟=PCLK1=150MHz，所有档位统一
 * 10tq/位、采样点80%、SJW=2：位速率 = 150MHz / (prescaler × 10)
 *   125k -> 120   250k -> 60   500k -> 30   1M -> 15                    */
static const uint32_t s_bitrateTable[][2] =
{
  { 125000U, 120U },
  { 250000U,  60U },
  { 500000U,  30U },
  {1000000U,  15U },
};

/**
  * @brief  查询位速率对应的分频值
  * @retval 分频值；0表示该速率不受支持
  */
static uint32_t CAN_BitrateToPrescaler(uint32_t bitrate)
{
  uint32_t i;

  for (i = 0U; i < (sizeof(s_bitrateTable) / sizeof(s_bitrateTable[0])); i++)
  {
    if (s_bitrateTable[i][0] == bitrate)
    {
      return s_bitrateTable[i][1];
    }
  }
  return 0U;
}

/**
  * @brief  FDCAN 完整配置流程（初始化与运行时重配置共用，实例由 CAN_PERIPH 决定）
  * @note   仅允许主循环上下文调用；重配置后环形缓冲复位（此时已停机，无RX中断竞争）
  * @retval HAL状态
  */
static HAL_StatusTypeDef CAN_Configure(void)
{
  HAL_StatusTypeDef   status;
  FDCAN_FilterTypeDef filterConfig;
  uint32_t            prescaler = CAN_BitrateToPrescaler(s_bitrate);

  if (prescaler == 0U)
  {
    return HAL_ERROR;
  }

  /* 运行中重配置：先停机使状态机回到READY（HAL_FDCAN_Init要求） */
  if (hfdcan.State == HAL_FDCAN_STATE_BUSY)
  {
    if (HAL_FDCAN_Stop(&hfdcan) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  /* 位时序：PCLK1=150MHz，NominalPrescaler -> tq，10tq/位 -> 对应位速率，
   * 采样点80%。经典CAN模式不使用数据段位时序，按与仲裁段相同的合法值配置。 */
  hfdcan.Instance                            = CAN_PERIPH;
  hfdcan.Init.ClockDivider                   = FDCAN_CLOCK_DIV1;
  hfdcan.Init.FrameFormat                    = FDCAN_FRAME_CLASSIC;
#if (CAN_DEBUG_SELFTEST != 0)
  hfdcan.Init.Mode                           = FDCAN_MODE_EXTERNAL_LOOPBACK; /* 调试自测试：自发自收且驱动总线脚 */
#else
  if (s_loopbackSelfTest)
  {
    hfdcan.Init.Mode                         = FDCAN_MODE_INTERNAL_LOOPBACK; /* 上电自检：内部回环不驱动总线脚 */
  }
  else
  {
    hfdcan.Init.Mode                         = FDCAN_MODE_NORMAL;
  }
#endif
  hfdcan.Init.AutoRetransmission             = ENABLE;   /* 硬件自动重发，保证可靠送达 */
  hfdcan.Init.TransmitPause                  = DISABLE;
  hfdcan.Init.ProtocolException              = ENABLE;
  hfdcan.Init.NominalPrescaler               = prescaler;
  hfdcan.Init.NominalSyncJumpWidth           = 2U;
  hfdcan.Init.NominalTimeSeg1                = 7U;       /* 传播段+相位段1 */
  hfdcan.Init.NominalTimeSeg2                = 2U;
  hfdcan.Init.DataPrescaler                  = prescaler;
  hfdcan.Init.DataSyncJumpWidth              = 2U;
  hfdcan.Init.DataTimeSeg1                   = 7U;
  hfdcan.Init.DataTimeSeg2                   = 2U;
  /* 过滤器数量：混杂/自检模式1个(全收，多过滤器会重复存帧)；正常模式4个白名单 */
  hfdcan.Init.StdFiltersNbr                  = (s_promisc || s_loopbackSelfTest) ? 1U : 4U;
  hfdcan.Init.ExtFiltersNbr                  = 0U;
  hfdcan.Init.TxFifoQueueMode                = FDCAN_TX_FIFO_OPERATION;

  status = HAL_FDCAN_Init(&hfdcan);
  if (status != HAL_OK)
  {
    return status;
  }

  /* 接收过滤器：混杂/自检模式收全部；正常模式按角色白名单四连（docs/03 §4.5）。
   * 白名单显式放行 + 全局 REJECT 是抗广播风暴的正确默认。 */
  filterConfig.IdType       = FDCAN_STANDARD_ID;
  filterConfig.FilterIndex  = 0U;
  filterConfig.FilterType   = FDCAN_FILTER_MASK;
  filterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;

#if (CAN_DEBUG_SELFTEST != 0)
  filterConfig.FilterID1    = 0x000U;                 /* 调试自测试：掩码0，接收所有ID */
  filterConfig.FilterID2    = 0x000U;
  status = HAL_FDCAN_ConfigFilter(&hfdcan, &filterConfig);
  if (status != HAL_OK)
  {
    return status;
  }
#else
  if (s_promisc || s_loopbackSelfTest)
  {
    filterConfig.FilterID1  = 0x000U;                 /* 混杂/自检：掩码0，接收所有标准ID */
    filterConfig.FilterID2  = 0x000U;
    status = HAL_FDCAN_ConfigFilter(&hfdcan, &filterConfig);
    if (status != HAL_OK)
    {
      return status;
    }
  }
  else
  {
    uint32_t i;

    for (i = 0U; i < 4U; i++)
    {
      filterConfig.FilterIndex = i;
      filterConfig.FilterID1   = s_rxFilterTable[i].id;
      filterConfig.FilterID2   = s_rxFilterTable[i].mask;
      status = HAL_FDCAN_ConfigFilter(&hfdcan, &filterConfig);
      if (status != HAL_OK)
      {
        return status;
      }
    }
  }
#endif

  /* 全局过滤器：混杂/自检模式下非匹配标准/扩展帧一并收入RX FIFO0；正常模式全部拒绝 */
  if (s_promisc || s_loopbackSelfTest)
  {
    status = HAL_FDCAN_ConfigGlobalFilter(&hfdcan, FDCAN_ACCEPT_IN_RX_FIFO0,
                                          FDCAN_ACCEPT_IN_RX_FIFO0,
                                          FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE);
  }
  else
  {
    status = HAL_FDCAN_ConfigGlobalFilter(&hfdcan, FDCAN_REJECT, FDCAN_REJECT,
                                          FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE);
  }
  if (status != HAL_OK)
  {
    return status;
  }

  status = HAL_FDCAN_Start(&hfdcan);
  if (status != HAL_OK)
  {
    return status;
  }

  /* 接收新报文 + Tx事件确认 + 总线错误状态中断（V1.2.3中断线由ILS复位值决定，均走中断线0） */
  status = HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0U);
  if (status != HAL_OK)
  {
    return status;
  }
  /* Tx事件确认三件套：
   * 1) TEFNE：新事件写入通知（排空事件FIFO、对账MessageMarker）；
   * 2) TX_COMPLETE + BufferIndexes=0x07：使能3个Tx缓冲区的传输完成中断，
   *    V1.2.3的HAL据此置TXBTIE寄存器——G4精简FDCAN IP在TXBTIE未使能时
   *    事件FIFO写入处于未授权状态，会导致每次发送误报TEFL（元素丢失）；
   * 3) TEFL：真实丢失时的告警（正常应为0）。 */
  status = HAL_FDCAN_ActivateNotification(&hfdcan,
                                          FDCAN_IT_TX_EVT_FIFO_NEW_DATA | FDCAN_IT_TX_EVT_FIFO_ELT_LOST,
                                          0U);
  if (status != HAL_OK)
  {
    return status;
  }
  status = HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_TX_COMPLETE, 0x07U);
  if (status != HAL_OK)
  {
    return status;
  }
  status = HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_BUS_OFF | FDCAN_IT_ERROR_PASSIVE, 0U);
  if (status != HAL_OK)
  {
    return status;
  }

  /* 队列复位（此时已停机，且由 can_task 上下文调用，无RX中断竞争） */
  if (s_rx_queue != NULL)
  {
    (void)xQueueReset(s_rx_queue);
  }

  return HAL_OK;
}

/**
  * @brief  初始化 FDCAN：接收队列、位时序、过滤器、中断、启动
  * @retval HAL状态
  */
HAL_StatusTypeDef CAN_Init(void)
{
  if (s_rx_queue == NULL)
  {
    s_rx_queue = xQueueCreate(CAN_RX_Q_LEN, sizeof(CAN_RxMsg));
    if (s_rx_queue == NULL)
    {
      return HAL_ERROR;                            /* heap exhausted */
    }
  }

  s_bitrate         = CAN_BITRATE;
  s_promisc         = false;
  s_loopbackSelfTest = false;
  s_busOffBackoffMs = CAN_BACKOFF_INIT_MS;
  s_busOffDueTick   = 0U;
  return CAN_Configure();
}

/**
  * @brief  上电自检：内部回环自发自收一帧（不驱动总线引脚）
  * @retval true=软件链路完整；false=自检失败
  */
bool CAN_SelfTest(void)
{
  const uint8_t testPayload[CAN_PAYLOAD_LEN] = {0x5AU, 0xA5U, 0x3CU, 0xC3U, 0, 0, 0, 0};
  bool          ok = false;
  CAN_RxMsg     msg;
  uint32_t      start;

  s_loopbackSelfTest = true;
  if (CAN_Configure() != HAL_OK)
  {
    s_loopbackSelfTest = false;
    (void)CAN_Configure();                           /* 尽力恢复正常模式 */
    return false;
  }

  if (CAN_Send(0x1F5U, testPayload, 4U) == HAL_OK)
  {
    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 100U)           /* 自检超时100ms */
    {
      if (CAN_PollRx(&msg))
      {
        ok = (msg.id == 0x1F5U) && (msg.len == 4U) &&
             (msg.data[0] == 0x5AU) && (msg.data[1] == 0xA5U);
        break;
      }
    }
  }

  s_loopbackSelfTest = false;
  (void)CAN_Configure();                             /* 恢复正常模式 */
  return ok;
}

/**
  * @brief  主循环周期任务：调度bus-off指数退避恢复
  * @note   恢复动作（Stop->Start）放在主循环而非中断，避免中断里做耗时操作
  */
void CAN_Task(void)
{
  if ((s_busOffDueTick != 0U) && ((int32_t)(HAL_GetTick() - s_busOffDueTick) >= 0))
  {
    s_busOffDueTick = 0U;

    if (HAL_FDCAN_Stop(&hfdcan) == HAL_OK)
    {
      (void)HAL_FDCAN_Start(&hfdcan);
      (void)HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0U);
      (void)HAL_FDCAN_ActivateNotification(&hfdcan,
                                          FDCAN_IT_TX_EVT_FIFO_NEW_DATA | FDCAN_IT_TX_EVT_FIFO_ELT_LOST,
                                          0U);
      (void)HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_TX_COMPLETE, 0x07U);
      (void)HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_BUS_OFF | FDCAN_IT_ERROR_PASSIVE, 0U);
    }
    else
    {
      s_busOffDueTick = HAL_GetTick() + 100U;        /* Stop失败：100ms后重试 */
    }
  }
}

/**
  * @brief  运行时切换总线波特率（125k/250k/500k/1M）
  * @note   重配置期间收发短暂中断；两块板必须切换到同一速率，否则无法互通
  * @retval HAL状态
  */
HAL_StatusTypeDef CAN_SetBitrate(uint32_t bitrate)
{
  if (CAN_BitrateToPrescaler(bitrate) == 0U)
  {
    return HAL_ERROR;                                  /* 不支持的速率 */
  }

  s_bitrate = bitrate;
  return CAN_Configure();
}

/**
  * @brief  开/关混杂模式（总线监视sniff：接收所有ID）
  * @retval HAL状态
  */
HAL_StatusTypeDef CAN_SetPromiscuous(bool enable)
{
  s_promisc = enable;
  return CAN_Configure();
}

/**
  * @brief  读取当前位速率
  */
uint32_t CAN_GetBitrate(void)
{
  return s_bitrate;
}

/**
  * @brief  发送一帧经典CAN数据帧（标准ID）
  * @param  stdId 标准ID（0x000~0x7FF）
  * @param  data  数据指针
  * @param  len   数据长度（<=8）
  * @retval HAL状态（HAL_BUSY表示Tx FIFO暂满）
  */
HAL_StatusTypeDef CAN_Send(uint32_t stdId, const uint8_t *data, uint8_t len)
{
  FDCAN_TxHeaderTypeDef txHeader;
  uint8_t               txData[CAN_PAYLOAD_LEN] = {0U};  /* 零填充暂存区：驱动按字(4字节)拷贝，
                                                           防止len<8时读越界 */
  uint8_t               marker;

  if ((data == NULL) || (len > CAN_PAYLOAD_LEN) ||
      (stdId > 0x7FFU))
  {
    s_stats.txErrors++;
    return HAL_ERROR;
  }

  if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan) == 0U)
  {
    s_stats.txErrors++;                        /* Tx FIFO满：放弃本帧并统计 */
    return HAL_BUSY;
  }

  for (uint8_t i = 0U; i < len; i++)
  {
    txData[i] = data[i];
  }

  marker = ++s_txMarker;                        /* 1~255循环标记（Tx Event回读对账） */
  s_txTick[marker] = HAL_GetTick();             /* 入队时刻，用于确认延迟测量 */

  txHeader.Identifier          = stdId;
  txHeader.IdType              = FDCAN_STANDARD_ID;
  txHeader.TxFrameType         = FDCAN_DATA_FRAME;
  txHeader.DataLength          = len;                     /* V1.2.3语义：原始DLC(0~15)，
                                                            驱动内部完成<<16编码 */
  txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  txHeader.BitRateSwitch       = FDCAN_BRS_OFF;
  txHeader.FDFormat            = FDCAN_CLASSIC_CAN;        /* 经典CAN帧 */
  txHeader.TxEventFifoControl  = FDCAN_STORE_TX_EVENTS;    /* 存Tx事件供发送确认 */
  txHeader.MessageMarker       = marker;

  if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan, &txHeader, txData) != HAL_OK)
  {
    s_stats.txErrors++;
    return HAL_ERROR;
  }

  s_stats.txCount++;
  return HAL_OK;
}

/**
  * @brief  取走一帧接收报文（can_task 上下文，非阻塞）
  * @param  msg 输出报文
  * @retval true取到一帧；false队列为空
  * @note   中断侧只投递队列，所有协议处理都发生在任务上下文（docs/02 §5）。
  */
bool CAN_PollRx(CAN_RxMsg *msg)
{
  if ((msg == NULL) || (s_rx_queue == NULL))
  {
    return false;
  }
  return (xQueueReceive(s_rx_queue, msg, 0U) == pdTRUE);
}

/**
  * @brief  阻塞等待一帧接收报文（can_task 上下文）
  * @param  msg        输出报文
  * @param  timeout_ms 队列为空时的最长等待（ms）
  * @retval 1 取到一帧；0 超时
  * @note   队列由 ISR 投递，任务在队列上阻塞 = "ISR 通知"语义（docs/02 T1）。
  */
uint8_t CAN_WaitRx(CAN_RxMsg *msg, uint32_t timeout_ms)
{
  if ((msg == NULL) || (s_rx_queue == NULL))
  {
    return 0U;
  }
  return (xQueueReceive(s_rx_queue, msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) ? 1U : 0U;
}

/**
  * @brief  获取通信统计指针（调试器实时观察用）
  */
const CAN_Stats *CAN_GetStats(void)
{
  return &s_stats;
}

/**
  * @brief  RX FIFO0接收回调（中断上下文）：逐帧投递到FreeRTOS队列
  * @note   ISR 只做 xQueueSendFromISR + yield；队列满时丢弃并计入
  *         rxOverruns（等价于旧环形缓冲满的语义）。
  */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
  FDCAN_RxHeaderTypeDef rxHeader;
  uint8_t               rxData[CAN_PAYLOAD_LEN];
  BaseType_t            woken = pdFALSE;

  if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != 0U)
  {
    /* 循环排空FIFO，避免高负载下滞留 */
    while (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK)
    {
      CAN_RxMsg msg;

      msg.id        = rxHeader.Identifier;
      msg.timestamp = rxHeader.RxTimestamp;
      /* V1.2.3的DataLength为原始DLC：经典帧DLC>8时有效字节仍为8 */
      msg.len       = (rxHeader.DataLength > 8U) ? 8U : (uint8_t)rxHeader.DataLength;
      for (uint8_t i = 0U; i < msg.len; i++)
      {
        msg.data[i] = rxData[i];
      }

      if (xQueueSendFromISR(s_rx_queue, &msg, &woken) != pdTRUE)
      {
        s_stats.rxOverruns++;                   /* 队列满：本帧丢弃并统计 */
      }
      s_stats.rxCount++;
    }

    /* 排空后最后一次GetRxMessage必然返回FIFO_EMPTY并粘滞置位ErrorCode；
     * V1.2.3的IRQHandler在每次中断结尾检查ErrorCode!=0即误调ErrorCallback，
     * 必须在此主动清除（否则协议错误计数按中断频率虚增） */
    hfdcan->ErrorCode = HAL_FDCAN_ERROR_NONE;

    portYIELD_FROM_ISR(woken);
  }

  if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) != 0U)
  {
    s_stats.rxOverruns++;                       /* FIFO满导致报文丢失 */
  }
}

/**
  * @brief  Tx事件回调（中断上下文）：逐帧确认送达并测量延迟
  * @note   任一帧确认成功即认为总线恢复健康，bus-off退避时长复位
  */
void HAL_FDCAN_TxEventFifoCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t TxEventFifoITs)
{
  FDCAN_TxEventFifoTypeDef txEvent;

  if ((TxEventFifoITs & FDCAN_IT_TX_EVT_FIFO_NEW_DATA) != 0U)
  {
    while (HAL_FDCAN_GetTxEvent(hfdcan, &txEvent) == HAL_OK)
    {
      uint32_t latency = HAL_GetTick() - s_txTick[txEvent.MessageMarker];

      s_stats.txAckCount++;
      if (latency > s_stats.txMaxLatencyMs)
      {
        s_stats.txMaxLatencyMs = latency;       /* 入队->总线确认最大延迟 */
      }

      s_busOffBackoffMs = CAN_BACKOFF_INIT_MS;  /* 总线健康：复位退避 */
    }

    /* 同Rx排空：清除GetTxEvent排空后粘滞的FIFO_EMPTY错误码，防ErrorCallback误报 */
    hfdcan->ErrorCode = HAL_FDCAN_ERROR_NONE;
  }

  if ((TxEventFifoITs & FDCAN_IT_TX_EVT_FIFO_ELT_LOST) != 0U)
  {
    s_stats.txEvtLost++;                        /* 事件FIFO溢出丢失（真实丢失，正常应为0） */
  }
}

/**
  * @brief  总线错误状态回调（中断上下文）：计数 + 登记退避恢复
  * @note   M_CAN进入bus-off时硬件自动置位INIT离线；真正的Stop->Start恢复
  *         由主循环CAN_Task在退避时间到后执行（不在中断里做）。
  */
void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
  UNUSED(hfdcan);

  if ((ErrorStatusITs & FDCAN_IT_ERROR_PASSIVE) != 0U)
  {
    s_stats.errPassiveCount++;                 /* 进入/退出错误被动状态（总线异常的早期信号） */
  }

  if ((ErrorStatusITs & FDCAN_IT_BUS_OFF) != 0U)
  {
    s_stats.busOffCount++;

    /* 指数退避：1s->2s->4s->...->30s封顶 */
    s_busOffDueTick = HAL_GetTick() + s_busOffBackoffMs;
    s_busOffBackoffMs *= 2U;
    if (s_busOffBackoffMs > CAN_BACKOFF_MAX_MS)
    {
      s_busOffBackoffMs = CAN_BACKOFF_MAX_MS;
    }
  }
}

/**
  * @brief  协议错误回调（中断上下文）：仅计数，供调试观察
  */
void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef *hfdcan)
{
  UNUSED(hfdcan);
  s_stats.protocolErrors++;
}

/*====================================================================*/
/* ISR symbol wired into the startup vector table                      */
/*   Same convention as BSP/bsp_uart.c: the BSP owns its vector, the   */
/*   name comes from the config point so switching FDCAN1/FDCAN2 does   */
/*   not touch Core/Src/stm32g4xx_it.c.                                 */
/*====================================================================*/
void CAN_IRQ_HANDLER(void)
{
    HAL_FDCAN_IRQHandler(&hfdcan);
}
