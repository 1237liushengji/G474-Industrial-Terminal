# FreeRTOS-Kernel (vendored, pruned)

## Provenance

| Item | Value |
|---|---|
| Upstream | https://github.com/FreeRTOS/FreeRTOS-Kernel |
| Tag | **V10.5.1** (release tarball, SHA of the tag as published 2022-11-16) |
| License | MIT — see `LICENSE.md` (kept verbatim) |
| Imported | v0.2 (docs/05), 2026-10-07 |

Kept files (everything else from the tarball was pruned):

```
include/                          kernel headers (complete)
tasks.c list.c queue.c            the three kernel translation units we build
portable/GCC/ARM_CM4F/port.c      Cortex-M4F port
portable/GCC/ARM_CM4F/portmacro.h
portable/MemMang/heap_4.c         heap manager (coalescing free list)
```

## Why V10.5.1

- Long-term-stable release line; the `GCC/ARM_CM4F` port is the combination
  ARM/Keil themselves document for **ARM Compiler 6 (armclang)** — the RVDS
  ports were deprecated in V10.4 and removed afterwards.  This project builds
  with ARMCLANG V6.14, so the GCC port is the supported choice.
- Covers everything the v0.2 task model uses (queues, task notifications,
  mutexes, heap_4) without the V11 API/naming churn.

## Pruning rules (so a future upgrade is mechanical)

- `event_groups.c`, `stream_buffer.c`, `timers.c` are **not** compiled and not
  vendored.  If a later milestone needs them (software timers, event groups),
  copy them from the same tag and add to the Keil group — no `.c` edits needed.
- Do not patch kernel files in-tree.  If a fix is needed, take it from a
  newer upstream tag and record the reason here.

## Integration points (in this repo)

- `Core/Inc/FreeRTOSConfig.h` — the single kernel configuration point
  (tick 1000 Hz, MaxSyscall priority 5, heap size per role).
- `Core/Src/stm32g4xx_it.c` no longer defines SVC/PendSV/SysTick handlers;
  the port owns them via `#define vPortSVCHandler SVC_Handler` etc.
- HAL time base moved to TIM6 (`Core/Src/stm32g4xx_hal_timebase_tim.c`),
  free of `HAL_TIM`, register-level, priority (8,0) — see docs/02 decision 3.
