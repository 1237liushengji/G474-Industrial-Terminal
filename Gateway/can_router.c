/**
  ******************************************************************************
  * @file    can_router.c
  * @brief   Table-driven CAN frame routing (docs/03 §4.6).
  *
  *  Route tables are per role.  ISO-TP channels (UDS 0x7E0/0x7E8 and the
  *  snapshot channel 0x1A0/0x181) are rows in the same table: their handlers
  *  simply feed the respective isotp_ctx_t, which self-filters by rxId.
  ******************************************************************************
  */
#include "can_router.h"
#include "can_contract.h"
#include "isotp.h"
#include "app_board_cfg.h"
#include "uds.h"
#include "bridge.h"

/*====================================================================*/
/* ISO-TP feed handlers                                                */
/*====================================================================*/
static void Route_UdsFrame(const CAN_RxMsg *m)
{
    (void)Uds_FeedCanFrame(m->id, m->data, m->len);
}

#if ROLE_IS_GW
/*--------------------------------------------------------------------*/
/* Gateway: node -> gw telemetry/status/counters/event/ack/hb + snap  */
/*--------------------------------------------------------------------*/
static const can_route_t s_routes[] =
{
    { 0x7FFU, CANC_ID_TELEMETRY_FAST, 8U, Bridge_RxTelemetry   },
    { 0x7FFU, CANC_ID_STATUS,         8U, Bridge_RxStatus      },
    { 0x7FFU, CANC_ID_COUNTERS,       8U, Bridge_RxCounters    },
    { 0x7FFU, CANC_ID_EVENT,          8U, Bridge_RxEvent       },
    { 0x7FFU, CANC_ID_CMD_ACK,        8U, Bridge_RxCmdAck      },
    { 0x7FFU, CANC_ID_HB_NODE,        4U, Bridge_RxHeartbeat   },
    { 0x7FFU, CANC_ID_SNAPSHOT_DATA,  1U, Bridge_RxSnapshotFrame },
    { 0x7FFU, ISOTP_DIAG_RESP_ID,     1U, Route_UdsFrame       },
};
#else
/*--------------------------------------------------------------------*/
/* Node: gw CMD/hb + snapshot request(+FC) + UDS request              */
/*--------------------------------------------------------------------*/
static const can_route_t s_routes[] =
{
    { 0x7FFU, CANC_ID_CMD,            8U, Bridge_RxCmd         },
    { 0x7FFU, CANC_ID_HB_GW,          4U, Bridge_RxHeartbeat   },
    { 0x7FFU, CANC_ID_SNAPSHOT_REQ,   1U, Bridge_RxSnapshotFrame },
    { 0x7FFU, ISOTP_DIAG_REQ_ID,      1U, Route_UdsFrame       },
};
#endif

/*====================================================================*/
/* Dispatch                                                            */
/*====================================================================*/
static volatile uint32_t s_unmatched = 0U;

void CanRouter_Dispatch(const CAN_RxMsg *msg)
{
    uint32_t i;
    uint8_t  matched = 0U;

    if (msg == NULL)
    {
        return;
    }

    for (i = 0U; i < (uint32_t)(sizeof(s_routes) / sizeof(s_routes[0])); i++)
    {
        const can_route_t *r = &s_routes[i];

        if (((msg->id & r->id_mask) == r->id_value) && (msg->len >= r->dlc_min))
        {
            matched = 1U;
            r->handler(msg);
        }
    }

    if (matched == 0U)
    {
        s_unmatched++;
    }
}

uint32_t CanRouter_GetUnmatched(void)
{
    return s_unmatched;
}
