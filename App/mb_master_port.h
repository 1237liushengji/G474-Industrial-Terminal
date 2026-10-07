/**
  ******************************************************************************
  * @file    mb_master_port.h
  * @brief   Gateway role entry (A board) - runs as mb_task (T2) since v0.2.
  ******************************************************************************
  */
#ifndef __MB_MASTER_PORT_H
#define __MB_MASTER_PORT_H

#include "main.h"

/** Role body: init Modbus master + poll loop.  Never returns (task body). */
void Master_TaskMain(void);

#endif /* __MB_MASTER_PORT_H */
