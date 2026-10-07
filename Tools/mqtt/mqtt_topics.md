# MQTT 主题表与上行帧协议（冻结 · v0.5）

> 本文件是 M1 影子网关（v0.5）与设备侧 MQTT（v0.6）共用的**冻结接口**。
> 契约来源：docs/03 §5.1（主题/payload/QoS）；本文件补充 **USART1 线协议** 与
> PC 侧工具用法。任何改动必须先改 docs/03，再改这里，最后改代码。

## 1. USART1 线协议（0xAA55 帧，v0.5 冻结）

```
AA 55 | type(1) | seq(2 BE) | len(2 BE) | payload[len] | crc16(2 LE)
```

- `crc16` = MODBUS_CRC16（init 0xFFFF / poly 反射 0xA001），覆盖 `type..payload`
  （即 5+len 字节），低字节在前。
- `payload` = docs/03 §5.1 的 JSON，**逐字节原样**发布（M2 时同一份字节交给
  MQTT-C，构建器唯一：`Gateway/uplink_port.c`）。
- **与 console 文本共线**：console 行以可打印 ASCII 0x20–0x7E 开头（docs/02 决策 4），
  首字节 0xAA ⇒ 二进制帧，其余 ⇒ 文本透传。发送侧由 muxtex 保证不交错
  （`BSP_TxLock/Unlock`）。
- `len ≤ 1536`（meta 实测 1405 B —— docs/03 §5.1 的 640 B 估算是旧值）。

| type | 名称 | 周期 | 主题 | QoS / Retain |
|---|---|---|---|---|
| 1 | telemetry | 1 s | `{PFX}/n1/telemetry` | 0 / 否 |
| 2 | status | 5 s | `{PFX}/n1/status` | 1 / **是** |
| 3 | counters | 10 s | `{PFX}/n1/counters` | 0 / 否 |
| 4 | meta | 上电 + 变更 | `{PFX}/meta` | 1 / **是** |
| 5 | event | 即时 | `{PFX}/n1/event` | 1 / 否 |
| 6 | snapshot | 30 s | `{PFX}/n1/snapshot` | 1 / 否 |

`{PFX}` 默认 `gac/line1/gwA`（配置点 `MQTT_TOPIC_PREFIX`）。

## 2. payload 一览（docs/03 §5.1，字段顺序即固件输出顺序）

```
telemetry {"seq":123,"ts":45678,"temp":28.6,"humi":63.2,"volt":12.08,"curr":1.25,"adc1":2048,"mcu_t":41.2,"vref":1020}
status    {"st":1,"er":0,"lk":13,"ch":"A91F","cv":1,"ts":45678,"heap":3120}
counters  {"rx":1234,"tx":1230,"crc":2,"can_ack":4510,"can_ovr":0,"can_bo":1,"can_lat":3,"mb_to":3,"mb_rt":5,"mb_exc":0}
snapshot  {"cv":1,"ch":"A91F","regs":{"40001":286,...,"40056":0}}      # 56 槽：PDU 0x00..0x37
meta      {"fw":"1.0.0","cv":1,"ch":"A91F","role":"GW","form":"M1","can":"500k","mb_baud":115200,"mb_sid":1,"dp":[["temp",0,1,"RO"],...]}
event     {"ev":9,"p":[1,0,0],"seq":88,"ts":45670}
```

- `ch` = 契约哈希（4 位大写 hex；本版 = `E71B`），A-05 的作用就是比它：
  寄存器 0x18 == `meta."ch"` == 快照 regs[0x18]（`Tools/python/contract_check.py --port`）。
- `seq`：telemetry = 节点发布序号低字节（0x111 B7 / 0x501）；event = 网关帧序号低字节（去重用）。
- `lk` = 网关 0x17 链路位图（bit0 485 / bit1 CAN / bit2 WiFi / bit3 MQTT）。
- 数值刻度一律 `fmt_fixed`（禁 %f）；比对用**整数原值**，不用浮点容差。

## 3. PC 工具

```
# 影子网关（板 USART1 的 USB-TTL → broker）
python Tools/python/mqtt_shadow_gw.py --port COM7 --broker 127.0.0.1

# 离线回放 / CI（不需要串口与 broker）
python Tools/python/mqtt_shadow_gw.py --stdin --dry-run < capture.bin

# A-06：RS485 直读（PC 主站）与 MQTT 收到的同一组测量量逐点整数比对
python Tools/python/mqtt_verify.py --port COM7 --broker 127.0.0.1 --count 1000

# 人工观察
mosquitto_sub -h 127.0.0.1 -v -t 'gac/#'
```

## 4. broker bring-up（本机现状）

`E:\Edge\mosquitto-2.1.2` 是**源码树，尚无二进制**。二选一：

1. 官方 Windows 安装包（推荐，一条命令即可跑 `mosquitto.exe -c Tools/mqtt/mosquitto.conf -v`）；
2. 自行编译源码（msbuild/cmake，需 VS 工具链）。

配置模板已随仓库提供：`Tools/mqtt/mosquitto.conf`（`persistence true` 是 QoS1
离线消息的前提，docs/03 §5.3）。
