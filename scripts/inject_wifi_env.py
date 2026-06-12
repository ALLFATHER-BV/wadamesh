#!/usr/bin/env python3
"""Inject WIFI_SSID and WIFI_PWD from environment into build. Never put real credentials in the repo."""

import os

Import("env")

ssid = os.environ.get("WIFI_SSID", "YourSSID").replace("\\", "\\\\").replace('"', '\\"')
pwd = os.environ.get("WIFI_PWD", "YourPassword").replace("\\", "\\\\").replace('"', '\\"')

# Write a generated header so we avoid -D quote escaping; force-include it.
out_dir = os.path.join(env.get("PROJECT_DIR", "."), "scripts", "build")
os.makedirs(out_dir, exist_ok=True)
header_path = os.path.join(out_dir, "wifi_secrets.h")
with open(header_path, "w") as f:
    f.write("/* Generated at build from WIFI_SSID/WIFI_PWD env - do not commit */\n")
    f.write("#ifndef WIFI_SECRETS_H\n#define WIFI_SECRETS_H\n")
    f.write('#define WIFI_SSID "%s"\n' % ssid)
    f.write('#define WIFI_PWD "%s"\n' % pwd)
    f.write("#endif\n")

env.Append(CPPPATH=[out_dir])
env.Append(CCFLAGS=["-include", "wifi_secrets.h"])
