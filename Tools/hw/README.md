# Tools/hw — 硬件台架工具（v1.0 验收期沉淀）

这些工具在 v1.0 实机验收期（2026-10-12 ~ 10-14）实际使用并验证，统一遵循项目铁律
**"串口静默打开"**（open 前预设 `dtr=False; rts=False`）——实机实锤：默认方式打开
会在端口开启期间把板**按住复位**（全程 0 字节，关闭瞬时才重启，见 docs/迁移说明 §19.3）。

| 工具 | 用途 |
|---|---|
| `silent_probe.py` | 探活 / 控制台交互：静默读流、可选发一条命令行（收 ack）、原始字节留存 |
| `raw_capture.py` | 纯只读原始捕获（USART1 的 0xAA55 帧流 + 控制台文本 → .bin） |
| `parse_capture.py` | 用 `Tools/python/uplink_frames.py` 解码捕获（帧类型/seq/负载 + 文本行） |
| `cmd_burst.py` | 连续 console `cmd <reg> <val>` 写入（逐条等 ack；A-11/A-13 的 100× 写入用它） |

示例（依赖 `pyserial`，系统 Python 3.14 已装）：

```bash
python Tools/hw/silent_probe.py COM3 2 stats        # 读 2s → 发 "stats" → 等应答
python Tools/hw/raw_capture.py COM3 15 cap.bin      # 静默捕获 15 秒
python Tools/hw/parse_capture.py cap.bin            # 解码查看
python Tools/hw/cmd_burst.py COM3 9 100 100 10 345  # 向寄存器 9 循环写 100 次、终值 345
```
