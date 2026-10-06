#!/usr/bin/env python3
"""Subscribe to a MeshCore Observer topic and validate WADAMESH payloads."""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import re
import sys
from threading import Event


HEX_RE = re.compile(r"^[0-9A-Fa-f]+$")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_common(payload: dict) -> None:
    require(isinstance(payload.get("origin"), str) and payload["origin"], "origin is missing")
    origin_id = payload.get("origin_id")
    require(isinstance(origin_id, str) and len(origin_id) == 64 and HEX_RE.fullmatch(origin_id),
            "origin_id must be a 64-character hexadecimal public key")
    timestamp = payload.get("timestamp")
    require(isinstance(timestamp, str), "timestamp is missing")
    datetime.fromisoformat(timestamp.replace("Z", "+00:00"))


def validate_status(payload: dict) -> None:
    validate_common(payload)
    require(payload.get("status") in {"online", "offline"}, "invalid status value")
    require(isinstance(payload.get("firmware_version"), str) and payload["firmware_version"],
        "firmware_version is missing")
    require(payload.get("client_version") == "wadamesh-observer/1",
        "unexpected client_version")
    radio = payload.get("radio")
    require(isinstance(radio, str) and len(radio.split(",")) == 4,
        "radio must contain frequency, bandwidth, spreading factor and coding rate")
    frequency, bandwidth, spreading_factor, coding_rate = radio.split(",")
    float(frequency)
    float(bandwidth)
    int(spreading_factor)
    int(coding_rate)


def validate_packet(payload: dict) -> None:
    validate_common(payload)
    require(payload.get("type") == "PACKET", "type must be PACKET")
    require(payload.get("direction") == "rx", "direction must be rx")
    require(payload.get("route") in {"F", "D", "T", "U"}, "invalid route")

    for field in ("len", "packet_type", "payload_len", "SNR", "RSSI"):
        require(isinstance(payload.get(field), str), f"{field} must be a JSON string")

    raw = payload.get("raw")
    require(isinstance(raw, str) and len(raw) >= 4 and len(raw) % 2 == 0 and HEX_RE.fullmatch(raw),
            "raw must be an even-length hexadecimal packet")
    wire_len = int(payload["len"])
    require(len(raw) == wire_len * 2, "raw length does not match len")
    raw_bytes = bytes.fromhex(raw)
    header = raw_bytes[0]
    route_type = header & 0x03
    packet_type = (header >> 2) & 0x0F
    require(int(payload["packet_type"]) == packet_type,
            "packet_type does not match the raw header")
    require(payload["route"] == ("D" if route_type in {2, 3} else "F"),
            "route does not match the raw header")

    path_offset = 5 if route_type in {0, 3} else 1
    require(path_offset < len(raw_bytes), "raw packet has no path-length byte")
    path_descriptor = raw_bytes[path_offset]
    path_count = path_descriptor & 0x3F
    path_hash_size = ((path_descriptor >> 6) & 0x03) + 1
    require(path_hash_size <= 3, "reserved path hash size")
    path_start = path_offset + 1
    payload_start = path_start + path_count * path_hash_size
    require(payload_start < len(raw_bytes), "raw packet has no payload")
    packet_payload = raw_bytes[payload_start:]
    require(int(payload["payload_len"]) == len(packet_payload),
            "payload_len does not match the raw packet")
    float(payload["SNR"])
    int(payload["RSSI"])

    packet_hash = payload.get("hash")
    require(isinstance(packet_hash, str) and len(packet_hash) == 16 and HEX_RE.fullmatch(packet_hash),
            "hash must be the 8-byte MeshCore packet hash in hexadecimal")
    hash_input = bytes([packet_type])
    if packet_type == 9:
        hash_input += bytes([path_descriptor, 0])
    expected_hash = hashlib.sha256(hash_input + packet_payload).hexdigest()[:16]
    require(packet_hash.lower() == expected_hash, "hash does not match the MeshCore packet hash")
    if "path" in payload:
        require(isinstance(payload["path"], list), "path must be an array")
        expected_path = [raw_bytes[path_start + index * path_hash_size:
                                   path_start + (index + 1) * path_hash_size].hex()
                         for index in range(path_count)]
        require(payload["path"] == expected_path, "path does not match the raw packet")
    elif route_type in {2, 3} and path_count:
        raise ValueError("direct packet path is missing")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="MQTT broker hostname or IP")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--iata", default="TEST", help="Location code configured on the device")
    parser.add_argument("--topic", help="Override the subscription topic")
    parser.add_argument("--count", type=int, default=3, help="Packet payloads required for PASS")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--websockets", action="store_true")
    parser.add_argument("--ws-path", default="/mqtt")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("Install the verifier dependency: python3 -m pip install paho-mqtt", file=sys.stderr)
        return 2

    topic = args.topic or f"meshcore/{args.iata.upper()}/+/+"
    complete = Event()
    result = {"online": False, "packets": 0, "errors": []}
    transport = "websockets" if args.websockets else "tcp"
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, transport=transport)
    except (AttributeError, TypeError):
        client = mqtt.Client(transport=transport)

    if args.username:
        client.username_pw_set(args.username, args.password)
    if args.tls:
        client.tls_set()
    if args.websockets:
        client.ws_set_options(path=args.ws_path)

    def on_connect(client, _userdata, _flags, reason_code, _properties=None):
        code = getattr(reason_code, "value", reason_code)
        if code != 0:
            result["errors"].append(f"broker rejected connection: {reason_code}")
            complete.set()
            return
        client.subscribe(topic)
        print(f"Subscribed to {topic}")

    def on_message(_client, _userdata, message):
        try:
            payload = json.loads(message.payload)
            if message.topic.endswith("/status"):
                validate_status(payload)
                if payload["status"] == "online":
                    result["online"] = True
                print(f"STATUS {payload['status']} {payload['origin']} {payload['origin_id'][:12]}")
            elif message.topic.endswith("/packets"):
                validate_packet(payload)
                result["packets"] += 1
                print(f"PACKET {result['packets']}/{args.count} type={payload['packet_type']} "
                      f"route={payload['route']} RSSI={payload['RSSI']} SNR={payload['SNR']}")
            if result["online"] and result["packets"] >= args.count:
                complete.set()
        except (ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
            result["errors"].append(f"{message.topic}: {exc}")
            complete.set()

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, 60)
    client.loop_start()
    complete.wait(args.timeout)
    client.loop_stop()
    client.disconnect()

    if result["errors"]:
        print(f"FAIL: {result['errors'][0]}", file=sys.stderr)
        return 1
    if not result["online"]:
        print("FAIL: no retained online status received", file=sys.stderr)
        return 1
    if result["packets"] < args.count:
        print(f"FAIL: received {result['packets']} of {args.count} required packets", file=sys.stderr)
        return 1
    print(f"PASS: valid online status and {result['packets']} Observer packets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())