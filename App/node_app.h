/**
  ******************************************************************************
  * @file    node_app.h
  * @brief   Node role entry (B board) - mb_task body + UI split for ui_task.
  *          v0.2: the former one-piece Slave_Main() is split three ways:
  *            Node_TaskMain()  - MB slave engine loop      (mb_task,  T2)
  *            Node_UiInit()    - LCD + key bring-up        (ui_task,  T6)
  *            Node_UiStep()    - key scan + refresh        (ui_task,  T6)
  ******************************************************************************
  */
#ifndef __NODE_APP_H
#define __NODE_APP_H

#include "main.h"

/** Role body: init Modbus slave + poll loop.  Never returns (task body). */
void Node_TaskMain(void);

/** UI bring-up, runs once at ui_task start (LCD + keys). */
void Node_UiInit(void);

/** One UI tick: poll keys, refresh LCD when due.  Runs in ui_task. */
void Node_UiStep(void);

#endif /* __NODE_APP_H */
