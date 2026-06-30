#!/usr/bin/env python3
"""
Reference subscriber + decryptor for the wadamesh MQTT bridge.

The bridge publishes received mesh messages to:
    wadamesh/{node_hex}/msg/dm    direct / signed messages  (only if DM publish is ON)
    wadamesh/{node_hex}/msg/ch    channel messages
    wadamesh/{node_hex}/status    "online" / "offline" (retained LWT)

Payload:
    - No encryption key set on the device -> payload is plain UTF-8 JSON.
    - Encryption key (PSK) set            -> payload is
          base64( nonce[12] || ciphertext || tag[16] ),  AES-256-GCM,
          key = SHA-256(psk).
      Set PSK below to the same passphrase you typed on the device.

Requires:  pip install paho-mqtt cryptography
"""
import base64, hashlib, json
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

BROKER = "homeassistant.local"
PORT   = 1883
PSK    = ""          # same passphrase as Settings -> MQTT -> Encryption key; "" = plaintext


def decode(payload: bytes) -> dict:
    """Return the message dict, decrypting if a PSK is configured."""
    if not PSK:
        return json.loads(payload)
    key  = hashlib.sha256(PSK.encode()).digest()      # 32-byte AES-256 key
    blob = base64.b64decode(payload)
    nonce, ct_and_tag = blob[:12], blob[12:]           # GCM tag is the trailing 16 bytes
    plain = AESGCM(key).decrypt(nonce, ct_and_tag, None)
    return json.loads(plain)


def main():
    import paho.mqtt.client as mqtt

    def on_connect(c, *_):
        c.subscribe("wadamesh/+/msg/#")
        print("subscribed to wadamesh/+/msg/#")

    def on_message(_c, _u, msg):
        try:
            data = decode(msg.payload)
            print(f"{msg.topic}: {data}")
        except Exception as e:
            print(f"{msg.topic}: <undecodable: {e}>")

    c = mqtt.Client()
    c.on_connect, c.on_message = on_connect, on_message
    c.connect(BROKER, PORT, 60)
    c.loop_forever()


if __name__ == "__main__":
    main()
