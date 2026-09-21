/**
  ******************************************************************************
  * @file    ring_buffer.c
  * @brief   SPSC byte ring buffer (lock-free for one producer + one consumer).
  ******************************************************************************
  */
#include "ring_buffer.h"

void RING_Init(ring_buffer_t *rb, uint8_t *storage, uint16_t size)
{
    rb->buf = storage;
    rb->size = size;
    rb->head = 0U;
    rb->tail = 0U;
}

uint16_t RING_Count(const ring_buffer_t *rb)
{
    return (uint16_t)(rb->head - rb->tail);
}

uint8_t RING_Empty(const ring_buffer_t *rb)
{
    return (rb->head == rb->tail) ? 1U : 0U;
}

uint8_t RING_Full(const ring_buffer_t *rb)
{
    return ((uint16_t)(rb->head - rb->tail) == rb->size) ? 1U : 0U;
}

uint8_t RING_Push(ring_buffer_t *rb, uint8_t byte)
{
    if (RING_Full(rb))
    {
        return 0U;
    }
    rb->buf[rb->head % rb->size] = byte;
    rb->head = (uint16_t)(rb->head + 1U);
    return 1U;
}

uint8_t RING_Pop(ring_buffer_t *rb, uint8_t *byte)
{
    if (RING_Empty(rb))
    {
        return 0U;
    }
    *byte = rb->buf[rb->tail % rb->size];
    rb->tail = (uint16_t)(rb->tail + 1U);
    return 1U;
}

void RING_Reset(ring_buffer_t *rb)
{
    rb->head = 0U;
    rb->tail = 0U;
}
