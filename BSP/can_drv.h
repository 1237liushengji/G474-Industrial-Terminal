/**
  ******************************************************************************
  * @file    can.h
  * @brief   Dual-node CAN driver (classic CAN, RX FIFO0 + interrupt line 0)
  *
  * 硬件平台：STM32G474VET6 开发板 + SN65HVD230 收发器模块
  * 引脚：PB12 = FDCAN2_RX（AF9，接收发器 CTX/TXD）、
  *       PB13 = FDCAN2_TX（AF9，接收发器 CRX/RXD）
  *       （DS12288 Rev6 Table 13 Port D；实例与引脚均由 app_board_cfg.h
  *        的 CAN_* 宏给出）
  * 为什么不用 FDCAN2/PB12,PB13：那两个脚同时是板载 W25Q128 的 CS#/CLK，
  * 只能二选一；本系统选择保住板载 Flash。见 docs/07 §4 C1。
  *
  * 位时序（500 kbit/s，采样点80%，与ST官方G474E-EVAL示例结构一致）：
  *           FDCAN内核时钟 = PCLK1 = 150 MHz
  *           NominalPrescaler = 30  ->  位量子 tq = 200 ns
  *           1位 = 10 tq = 2 us     ->  500 kbit/s
  *           同步段(1) + Seg1(7) + Seg2(2)，采样点 = (1+7)/10 = 80%
  ******************************************************************************
  * @attention
  * 参考实现：STMicroelectronics/STM32CubeG4 官方示例
  *           Projects/STM32G474E-EVAL/Examples/FDCAN/FDCAN_Classic_Frame_Networking
  ******************************************************************************
  */
#ifndef __CAN_H
#define __CAN_H

#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include "app_board_cfg.h"

/*--------------------------------------- 节点角色配置 --------------------------------------*
 * 双机通信两个节点烧录不同固件：                                                          *
 *   板A（主节点）：周期发送命令帧，监视链路状态                                            *
 *   板B（从节点）：接收命令帧、翻转LED并应答                                              *
 * 使用方法：板A（网关）与板B（采集节点）烧同一份代码，仅 Keil Target 的                       *
 *           NODE_ROLE Define 不同（0=网关 / 1=节点）。                                       */
/* 节点角色改由 App/app_board_cfg.h 的 NODE_ROLE 统一给出（Keil Target 注入）。
 * 原 CAN_NODE_A / CAN_NODE_B / CAN_NODE_ROLE 已删除，避免第二处角色真相。 */

/* 单板自测试（硬件排错用）：置1后FDCAN进入外部回环模式——本板发出的帧会被
 * 自己收到，单板即可完成收发全流程。用于把固件问题与接线问题隔离开：
 * 自测试通过 => 固件正常，问题在外部总线/收发器/接线。
 * 测试完毕必须改回0恢复双机正常模式！ */
#define CAN_DEBUG_SELFTEST             0

/*--------------------------------------- 总线参数 ---------------------------------------*/
/* CAN_BITRATE / CAN_LINK_TIMEOUT_MS 由 app_board_cfg.h 统一给出
 * （本系统链路超时定为 3000 ms，以容纳 ISO-TP 快照传输）*/

/*--------------------------------------- 应用层协议 --------------------------------------*
 * v0.4：v3.0 的 0x321/0x322 请求-应答对已按契约废弃（docs/03 §4.2），
 * 职责分解为 0x500+N 心跳 / 0x300+N CMD / 0x100+N 遥测。
 * 帧 ID 与字节布局的唯一真相源：Protocol/can_contract.h（docs/03 §4）。   */

#define CAN_PAYLOAD_LEN                8U

/*--------------------------------------- 类型定义 ---------------------------------------*/
typedef struct
{
  uint32_t id;                              /* 报文标准ID */
  uint32_t timestamp;                       /* 接收时间戳计数（未启用时为0） */
  uint8_t  len;                             /* 有效数据长度（0~8） */
  uint8_t  data[CAN_PAYLOAD_LEN];           /* 数据域 */
} CAN_RxMsg;

typedef struct
{
  volatile uint32_t txCount;                /* 成功写入Tx FIFO的帧数 */
  volatile uint32_t txAckCount;             /* 经Tx Event FIFO确认送达总线的帧数 */
  volatile uint32_t txMaxLatencyMs;         /* 入队到总线确认的最大延迟（ms） */
  volatile uint32_t rxCount;                /* 成功接收的帧数 */
  volatile uint32_t txErrors;               /* 发送失败次数（FIFO满等） */
  volatile uint32_t rxOverruns;             /* 接收环形缓冲覆盖/报文丢失次数 */
  volatile uint32_t busOffCount;            /* 总线关闭（bus-off）发生次数 */
  volatile uint32_t errPassiveCount;        /* 进入/退出错误被动状态事件次数 */
  volatile uint32_t txEvtLost;              /* Tx事件FIFO元素丢失次数（正常应为0） */
  volatile uint32_t protocolErrors;         /* 协议类错误中断次数 */
} CAN_Stats;

/*--------------------------------------- 外部变量 ---------------------------------------*/
extern FDCAN_HandleTypeDef hfdcan;

/*--------------------------------------- 函数声明 ---------------------------------------*/
HAL_StatusTypeDef CAN_Init(void);
HAL_StatusTypeDef CAN_Send(uint32_t stdId, const uint8_t *data, uint8_t len);
bool              CAN_PollRx(CAN_RxMsg *msg);

/**
  * @brief  阻塞等待一帧（can_task 上下文；队列为空时最多等 timeout_ms 毫秒）。
  * @retval 1 取到一帧；0 超时
  * @note   v0.2：中断投递队列后，等待由队列直接唤醒（即"来自 ISR 的通知"）。
  */
uint8_t           CAN_WaitRx(CAN_RxMsg *msg, uint32_t timeout_ms);

const CAN_Stats  *CAN_GetStats(void);

/* 运行时重配置（控制台调用）：
 * 波特率仅支持125k/250k/500k/1M（内部为预计算时序表，保证所有档位
 * 统一10tq/位、采样点80%）；两块板必须同步切换，否则无法互通。
 * 混杂模式（总线监视sniff）：接收总线上所有ID的报文。 */
HAL_StatusTypeDef CAN_SetBitrate(uint32_t bitrate);
HAL_StatusTypeDef CAN_SetPromiscuous(bool enable);
uint32_t          CAN_GetBitrate(void);

/* 驱动工程化（阶段1）：
 * CAN_Task   —— 主循环周期调用：调度bus-off指数退避恢复（1s起步翻倍，
 *               上限30s；任一帧发送确认成功即复位退避），避免错误状态下
 *               立即重连风暴；
 * CAN_SelfTest —— 上电自检：临时切入内部回环自发自收一帧测试报文，
 *               验证"FDCAN内核+消息RAM+中断+收发路径"软件链路完整，
 *               不驱动总线引脚（内部回环含总线监测模式）。 */
void CAN_Task(void);
bool CAN_SelfTest(void);

#endif /* __CAN_H */
