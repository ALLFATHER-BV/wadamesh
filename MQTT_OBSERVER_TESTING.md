# MQTT Observer testing on ThinkNode M9

The Observer feature has 39 selectable profiles. Common starting points are:

- **Custom broker** publishes standard MeshCore Observer v1 JSON over plain MQTT.
- **NebraskaMesh** connects directly to `wss://mqtt.nebraskamesh.net:443/mqtt`
  using the M9 identity's Ed25519-signed JWT.
- **MichMesh** connects to the Let's Mesh Analyzer US endpoint at
   `wss://mqtt-us-v1.letsmesh.net:443/mqtt` with the same identity authentication.

The profile list also includes Analyzer US/EU, NZ Analyzer, MeshMapper,
MeshRank, WAEV, Meshomatic, CascadiaMesh, TennMesh, NashMesh, CTMesh, ChiMesh,
Meshat.se, East Idaho Mesh, ColoradoMesh, Dutch MeshCore 1/2, MeshCore Canada
1/2, MeshCore Finland, OkiMesh 1/2, INWMesh, BostonMesh, RFLab, IP Network UK,
FLMesh, CoreComms, MeshTexas, Mesh Chaun14, WCMesh, Atviras Tinklas, GoMesh,
IdahoMesh, NTXMesh and BSMesh.

Most built-in profiles need only a location/IATA code. Exceptions:

- **MeshRank** needs its account token in **Profile token** and does not use IATA.
- **INWMesh** needs the assigned broker username and password.
- **Mesh Chaun14** uses the device public key as username and needs its assigned password.
- TennMesh, NashMesh and CTMesh carry their published community credentials automatically.

Observer reporting is receive-only. It does not transmit packets over LoRa or
change normal WADAMESH messaging behavior.

Community profiles use public internet endpoints. The M9 can be on any Wi-Fi or
phone hotspot that permits outbound DNS, HTTPS/WSS on TCP port 443, and time
sync; it does not need to share a LAN with the Observer broker or analyzer.

## Open the MQTT settings

MQTT is hidden by default because it is experimental:

1. Open the app drawer, then **Store**.
2. Select **Built-in**.
3. Turn on **MQTT bridge**.
4. Open **Settings > MQTT bridge**.

Accept the privacy warning and enable the MQTT bridge. Leave **Publish direct
messages** off unless decoded-message forwarding is also under test.

## Test with a custom broker

Use a broker reachable from the M9's Wi-Fi. On another computer, install the
test subscriber once:

```bash
python3 -m pip install paho-mqtt
```

Start it before saving the M9 settings:

```bash
python3 scripts/mqtt/observer_verify.py --host 192.168.1.10 --iata TEST --count 3
```

Add `--username NAME --password VALUE` if the broker requires credentials.

On the M9 MQTT screen set:

1. **Broker host / IP** to the broker address and **Port** to `1883`.
2. **Observer profile** to **Custom broker**.
3. Turn on **Enable Observer reporting**.
4. Set **Observer name** to `M9_TEST`.
5. Set **Location / IATA code** to `TEST`.
6. Leave **Observer topic template** as `meshcore/{iata}/{device}`.
7. Select **Save**.

Generate traffic with another MeshCore node: send an advert, channel message,
or direct message over RF. The verifier passes after receiving retained online
status and three valid packet records. Each packet is checked for a full public
key, UTC timestamp, raw frame, packet type, route, RSSI, SNR, packet hash, and
path formatting.

Open the M9 **Terminal** and run:

```text
mqtt status
```

Expected: `mqtt: connected`, profile `custom`, Observer `on`, sent increasing,
queued returning to zero, and dropped remaining zero.

## Test persistence and queue limits

1. Reboot the M9 and confirm the verifier receives a new retained online status.
2. Generate another RF packet and confirm the sent counter increases.
3. Stop the broker or disconnect M9 Wi-Fi.
4. Generate more than eight RF packets.
5. Run `mqtt status`: queued must never exceed 8 and dropped should increase.
6. Restore the broker/Wi-Fi. The queue should drain to zero and new packets
   should continue publishing without freezing navigation.
7. Turn off **Enable Observer reporting**, save, and confirm no further packet
   messages are published.

## Test NebraskaMesh

Obtain a registered IATA-style location code from the Nebraska Mesh team first.
Then configure the M9:

1. Select **NebraskaMesh** as the Observer profile.
2. Turn on **Enable Observer reporting**.
3. Set the assigned **Location / IATA code** and an identifiable Observer name.
4. Leave the custom broker, credentials, topic template, and message encryption
   fields unchanged; this profile uses its built-in WSS endpoint and standard topic.
5. Save and run `mqtt status` in Terminal.

Expected progression: `waiting for Wi-Fi` or `waiting for valid UTC time`, then
`connecting`, then `connected`. Generate RF traffic and verify the sent counter
increases with no drops. Confirm the observer appears in the Nebraska Mesh
Analyzer under the assigned location code.

If it reaches `retrying`, record the complete `mqtt status` output. Error `4` or
`5` indicates broker authentication/authorization; a persistent TCP failure
indicates DNS, firewall, Wi-Fi, or TLS reachability rather than packet parsing.
The status output also includes `tls`, `stack`, and `socket` error numbers.

## Test MichMesh

Choose the Michigan location code that covers the observer site. MichMesh
currently documents these active zones:

- `DET` - greater Detroit
- `FNT` - Flint / Genesee County
- `GDW` - Gladwin and northern Michigan
- `AZO` - Kalamazoo
- `GRR` - Kent County / Grand Rapids
- `MBS` - Midland / Bay City / Saginaw

On the M9 MQTT screen:

1. Select **MichMesh** as the Observer profile.
2. Turn on **Enable Observer reporting**.
3. Enter the appropriate zone in **Location / IATA code**.
4. Enter an identifiable Observer name and save.
5. Run `mqtt status` in Terminal.

Expected broker: `mqtt-us-v1.letsmesh.net:443`, profile `MichMesh`, followed by
`connected`. Generate RF traffic and confirm the sent count rises while queued
returns to zero and dropped remains zero. The observer should then appear in the
[Let's Mesh Analyzer](https://analyzer.letsmesh.net/) under the selected zone.