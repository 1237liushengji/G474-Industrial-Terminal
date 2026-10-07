/**
  ******************************************************************************
  * @file    gw_ui.h
  * @brief   Gateway monitoring page on the TFT (v0.7).
  ******************************************************************************
  */
#ifndef __GW_UI_H
#define __GW_UI_H

/** Bring up keys + LCD and draw the static layer (ui_task start). */
void Gw_UiInit(void);

/** One UI tick (ui_task, 5 ms): keys + value refresh. */
void Gw_UiStep(void);

#endif /* __GW_UI_H */
