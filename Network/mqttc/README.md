# MQTT-C (vendored, pruned) + platform port

## Provenance

| Item | Value |
|---|---|
| Upstream | https://github.com/LiamBindle/MQTT-C |
| Tag | **v1.1.6** (release tarball) |
| License | MIT — `LICENSE` kept verbatim |
| Imported | v0.6 (docs/05), 2026-10-07 |

Kept files:

```
include/mqtt.h     client implementation header (verbatim)
src/mqtt.c         client state machine (verbatim)
include/mqtt_pal.h OUR platform port (replaces the upstream file, as the
                   upstream porting guide prescribes: "to port MQTT-C to a
                   new platform the following types/macros/functions must
                   be defined in mqtt_pal.h")
```

NOT vendored: upstream `src/mqtt_pal.c` (POSIX/wolfSSL/... transports).  Our
transport lives in `Network/mqtt_client.c`:

- `mqtt_pal_sendall()`  -> ESP-AT pass-through (Network/esp_at.c), blocking
  UART write only;
- `mqtt_pal_recvall()`  -> non-blocking ring pull (0 = nothing yet);
- `MQTT_PAL_TIME()`     -> HAL_GetTick()/1000 (keepalive bookkeeping);
- mutex macros are no-ops: the whole client is owned by uplink_task (single
  task confinement, documented in the PAL header).

## Integration points

- `Network/mqtt_client.c` — session management: connect with
  clean_session=false + LWT (`{PFX}/n0/lwt`), 100 ms `mqtt_sync` pump,
  reconnect with the shared 1->2->...->30 s backoff.
- `Gateway/uplink_port.c` — the JSON payload builders that feed
  `mqtt_publish` (identical bytes to the M1 frame path).
- `Network/backfill.c` — offline replay from the pending records in the
  W25Q log (Storage/log.h layout).

Do not patch the vendored files in-tree; take fixes from a newer upstream tag
and record the reason here.
