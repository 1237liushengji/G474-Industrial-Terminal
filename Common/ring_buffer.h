/**
  ******************************************************************************
  * @file    ring_buffer.h
  * @brief   Lock-free single-producer single-consumer byte ring buffer.
  * @note    Producer = UART DMA/IDLE ISR; consumer = protocol main loop.
  *          Pure C, no HAL dependency (PC-testable).
  ******************************************************************************
  */
#ifndef __RING_BUFFER_H
#define __RING_BUFFER_H

#include <stdint.h>

typedef struct {
    uint8_t  *buf;
    uint16_t size;      /* power of two recommended */
    volatile uint16_t head;   /* write index */
    volatile uint16_t tail;   /* read index  */
} ring_buffer_t;

/** Initialize with caller-provided storage. */
void RING_Init(ring_buffer_t *rb, uint8_t *storage, uint16_t size);

/** Number of bytes currently stored. */
uint16_t RING_Count(const ring_buffer_t *rb);

/** 1 when empty. */
uint8_t RING_Empty(const ring_buffer_t *rb);

/** 1 when full. */
uint8_t RING_Full(const ring_buffer_t *rb);

/** Push one byte. @retval 1 ok, 0 full (data lost). */
uint8_t RING_Push(ring_buffer_t *rb, uint8_t byte);

/** Pop one byte. @retval 1 ok, 0 empty. */
uint8_t RING_Pop(ring_buffer_t *rb, uint8_t *byte);

/** Discard all contents. */
void RING_Reset(ring_buffer_t *rb);

#endif /* __RING_BUFFER_H */
