"""
run_acceptance.py - one-click acceptance runner (docs/05 v0.8).

Stage 1 (always, no hardware): the offline regression battery
    - A-01  single-config-point check        (Tools/ci/check_config_points.sh)
    - run_tests.py                           (protocol regression + 11 golden vectors)
    - contract_check.py                      (C<->JSON cross-check, hash E71B)

Stage 2 (only when hardware ports are given): live checks
    --485-port COMx   USB-RS485 on the bus
        * contract_check.py --port   -> A-05 register leg (firmware 0x18 == E71B)
        * mqtt_verify.py --port      -> A-06 dual-path comparison (needs broker)
    --ttl-port COMx   USB-TTL on the gateway USART1
        * prints the exact command for the shadow gateway + broker tools

Everything not automatable (TFT, fault injection, 24 h runs) is printed as a
guided checklist with the acceptance IDs; archive results under
Tools/python/results/ (gitignored).

Exit code 0 = every executed stage passed.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable


def run(title, args, cwd):
    print("=" * 72)
    print(">> %s" % title)
    print("   $ %s" % " ".join(args))
    r = subprocess.run(args, cwd=cwd)
    ok = (r.returncode == 0)
    print("<< %s: %s" % (title, "PASS" if ok else "FAIL"))
    return ok


HARDWARE_CHECKLIST = """
--- Hardware acceptance checklist (not automatable offline) ---------------
A-02  Dual time base  : power both boards, run 10 min, console 'stats' shows
                        tick sources alive; no HAL timeout spins.
A-03  CAN accounting  : 1 s telemetry 30 min; A's can_rx vs 0x501 hb_seq;
                        ovr == 0; txAck == tx (console 'stats' = u32 values).
A-04  bus-off recovery: short CANH-GND 3 s; backoff 1->2->4 s; recover <= 30 s.
A-05  contract hash   : this script --485-port (register leg); shadow
                        gateway log shows meta.ch == E71B (TTL leg).
A-06  dual path       : mqtt_verify.py --port <485> --broker <ip> --count 1000
A-07  broker outage   : stop broker 60 s; backfill <= 24 s, no seq hole;
                        LOG_BACKFILL carries the loss count (console 'stats').
A-08  e2e latency     : telemetry ts vs PC receive time; P99 <= 200 ms.
A-09  pin assertion   : boot log shows "A-09: pin ownership OK".
A-10  stack watermarks: task_ovr == 0 over 24 h ('stats').
A-11  dual-path cfg   : RS485 FC06 vs MQTT cmd write of 0x09; 24C02 equal.
A-12  UI vs protocol  : browse TFT pages while polling; mb_timeout/mb_retry
                        deltas == 0.
A-13  power-cycle cfg : 100x write + power cut + reboot; value survives.
A-14  log ring        : 3000 records; seq monotonic; power-up scan correct.
Fault injection set   : docs/05 section 4 (11 cases).
Archive evidence      : Tools/python/results/*.jsonl  (gitignored)
---------------------------------------------------------------------------
"""


def main():
    ap = argparse.ArgumentParser(description="v1.0 acceptance runner")
    ap.add_argument("--485-port", dest="port_485",
                    help="USB-RS485 COM port (A-05/A-06 legs)")
    ap.add_argument("--ttl-port", help="USART1 USB-TTL COM port (shadow gateway)")
    ap.add_argument("--broker", default=None, help="broker host for A-06")
    ap.add_argument("--count", type=int, default=1000,
                    help="A-06 comparison sample count")
    args = ap.parse_args()

    tools = os.path.abspath(os.path.join(HERE, "..", "python"))
    root = os.path.abspath(os.path.join(HERE, "..", ".."))

    ok = True

    # ---- stage 1: offline ----
    ok &= run("A-01 config points",
              [PY, os.path.join(HERE, "check_config_points.py")], root)
    ok &= run("offline regression",
              [PY, os.path.join(tools, "run_tests.py")], root)
    ok &= run("contract check (offline legs)",
              [PY, os.path.join(tools, "contract_check.py")], root)

    # ---- stage 2: hardware legs (optional) ----
    if args.port_485:
        ok &= run("A-05 register leg (firmware 0x18)",
                  [PY, os.path.join(tools, "contract_check.py"),
                   "--port", args.port_485], root)
        if args.broker:
            ok &= run("A-06 dual path (RS485 vs MQTT)",
                      [PY, os.path.join(tools, "mqtt_verify.py"),
                       "--port", args.port_485, "--broker", args.broker,
                       "--count", str(args.count)], root)
        else:
            print("--  A-06 skipped: pass --broker <host> to enable it")
    else:
        print("--  hardware legs skipped: pass --485-port COMx to run A-05/A-06")

    if args.ttl_port:
        print("=" * 72)
        print(">> shadow gateway on %s - run in a second terminal:" % args.ttl_port)
        print("   python Tools/python/mqtt_shadow_gw.py --port %s%s"
              % (args.ttl_port,
                 (" --broker %s" % args.broker) if args.broker else ""))
        print("   mosquitto_sub -h %s -v -t 'gac/#'    # observe" %
              (args.broker or "127.0.0.1"))

    print(HARDWARE_CHECKLIST)
    print("RESULT: %s" % ("PASS (executed stages)" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
