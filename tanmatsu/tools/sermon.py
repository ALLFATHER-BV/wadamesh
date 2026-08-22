#!/usr/bin/env python3
# Resilient non-resetting serial monitor for the Tanmatsu P4 USB-serial-JTAG console.
# Auto-reconnects across USB re-enumeration (device reset when an app is launched).
import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1301"
logpath = sys.argv[2] if len(sys.argv) > 2 else "/tmp/wada_serial.log"

def open_port():
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 1
    s.dsrdtr = False
    s.rtscts = False
    s.open()
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    return s

with open(logpath, "ab") as out:
    while True:
        try:
            s = open_port()
            out.write(b"\n==== monitor attached ====\n"); out.flush()
            while True:
                d = s.read(4096)
                if d:
                    out.write(d); out.flush()
        except Exception as e:
            try:
                out.write(("\n==== port dropped: %s — reconnecting ====\n" % e).encode()); out.flush()
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.5)
