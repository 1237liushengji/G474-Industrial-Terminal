# 08 Storage 设计：24C02 参数区与 W25Q128 日志

## 1. 24C02 参数区布局（Storage/config）

24C02 = 256 B，I2C 地址 0xA0。布局：

| 偏移 | 长度 | 内容                    |
|------|------|-------------------------|
| 0x00 | 2    | 魔数 MAGIC = 0x4D42 ("MB") |
| 0x02 | 1    | 参数版本 PARAM_VER = 1  |
| 0x03 | 2    | SlaveID(1B) + 保留(1B)  |
| 0x05 | 2    | Baudrate 索引           |
| 0x07 | 2    | TempLimit (0.1℃)       |
| 0x09 | 2    | VoltageLimit (0.01V)   |
| 0x0B | 2    | SamplePeriod (ms)       |
| 0x0D | 2    | DeviceMode              |
| 0x0F | 2    | 校验 CRC16(对 0x00~0x0E) |
| 0x11 | —    | 保留                    |

- 读取：启动时整页读入 → 校验 MAGIC+CRC → 通过则装载，否则用默认值并触发一次保存。
- 写入：更新 RAM → 重算 CRC → 写 24C02（页写 ≤8B 拆两次）。
- 写保护策略：非易失参数仅在"修改完成/掉电前"写，避免频繁擦写（24C02 寿命 1M 次，无磨损问题，但写期间 I2C 占用注意临界区）。
- 对外接口：

```c
typedef struct {
    uint8_t  slave_id;
    uint8_t  baud_index;      /* 见寄存器表波特率码表 */
    uint16_t temp_limit;
    uint16_t volt_limit;
    uint16_t sample_period;
    uint8_t  device_mode;
} config_param_t;

bool CONFIG_Load(config_param_t *p);   /* 失败→填默认 */
bool CONFIG_Save(const config_param_t *p);
```

## 2. W25Q128 环形日志区（Storage/log）

W25Q128 = 16 MB；日志区取尾部 1 MB（其余保留给用户/后续 FFS）。

- 日志记录定长 64 B（方便环形管理 + 按记录索引）：
  - 4 B：序号(seq，单调递增，掉电后按魔数恢复)
  - 8 B：时间戳(自启动 tick 或 RTC BCD)
  - 4 B：事件类型 + 附加参数
  - 46 B：文本/扩展
- 头记录（每扇区首条或日志区固定偏移）保存：写指针、魔数、CRC，防掉电错乱。
- 环形策略：满 → 擦除最旧扇区 → 继续写（扇区级磨损均衡：写指针循环推进）。
- 事件类型（`LOG_*`，与规格书一致）：

```c
LOG_BOOT / LOG_RX_OK / LOG_TX_OK / LOG_TIMEOUT / LOG_CRC_ERROR /
LOG_EXCEPTION / LOG_CONFIG_CHANGE / LOG_SYSTEM_ERROR
```

- 接口：

```c
bool LOG_Init(void);
bool LOG_Write(log_event_t ev, uint16_t param);
uint32_t LOG_GetCount(void);
bool LOG_Read(uint32_t index, log_record_t *rec);   /* 供 LCD/Python dump */
```

- 写日志对波特率敏感操作的影响：Flash 页编程/擦除为 ms 级，需要调度配合
  （V1.0：仅在帧间空闲/从站非响应期执行；保证不影响 Modbus 时序）。

## 3. 电源/掉电

- CR1220 供 RTC：日志时间戳可用 RTC（需在 v1.1 前确认 RTC 是否已初始化配置）。
- 掉电保存采用"修改即存"策略（规格书要求修改→掉电→上电恢复），无需掉电检测。
