#!/usr/bin/env python3
"""Verify the float64 reference WMM against NOAA's official WMM2025_TestValues.txt."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wmm import load_cof, wmm
d = os.path.dirname(os.path.abspath(__file__))
m = load_cof(os.path.join(d, "WMM.COF"))
worst = {k: 0.0 for k in ("D","I","H","X","Y","Z","F")}
n = 0
for line in open(os.path.join(d, "WMM2025_TestValues.txt")):
    if line.startswith("#") or not line.strip(): continue
    t = line.split(); n += 1
    got = wmm(m, float(t[2]), float(t[3]), float(t[1]), float(t[0]))
    exp = dict(D=float(t[4]), I=float(t[5]), H=float(t[6]), X=float(t[7]),
               Y=float(t[8]), Z=float(t[9]), F=float(t[10]))
    for k in worst: worst[k] = max(worst[k], abs(got[k] - exp[k]))
print(f"{n} official NOAA test values; max abs error:")
for k, v in worst.items(): print(f"  {k}: {v:.3g}")
print("PASS" if worst["D"] < 0.006 else "FAIL")
