# Storage-reduction decision ledger

No feature removals are approved yet. Checkboxes here are product-scope choices,
not implementation status.

## Profile choice

- [ ] Mesh Essentials: keep LoRa messaging, contacts/channels, GPS, SD storage,
      WadaMesh UI, USB companion; remove optional presentation/network extras.
- [ ] Offline Mesh: keep USB and BLE companion; remove Wi-Fi-dependent features.
- [ ] USB-only Minimal: keep only wired companion plus on-device mesh essentials.
- [ ] Custom: list exact retained/removed families below.

## First-batch candidates

- [ ] Replace the embedded default lock wallpaper with a lightweight fallback.
- [ ] Remove Snake.
- [ ] Remove MQTT.
- [ ] Use an English-only/smaller-font build.
- [ ] Remove or reduce the color emoji pack.
- [ ] Remove the on-device web reader.
- [ ] Remove online map tiles but retain coordinates/Friend Compass.
- [ ] Remove the complete Map/location/navigation experience.
- [ ] Remove rich charts/diagnostics.
- [ ] Remove the general file manager/media extras while preserving SD-backed data.
- [ ] Remove the browser terminal/mirror pages.
- [ ] Remove browser-upload OTA while retaining a recoverable update route.
- [ ] Remove BLE companion.
- [ ] Remove Wi-Fi/TCP/WebSocket companion.
- [ ] Remove QR sharing.
- [ ] Disable private-key import/export.
- [ ] Selectively remove FriendMesh development feature families: ____________

## Explicitly retained

- [ ] WadaMesh branding and mark.
- [ ] Core LoRa/MeshCore messaging.
- [ ] Contacts and channels.
- [ ] T-Deck display and physical inputs.
- [ ] `/meshcomod` SD-bound FriendMesh persistence and missing-card recovery.
- [ ] Existing A/B OTA partition layout.
- [ ] Other: ____________

## Approved profile

Record the user's final choice here before implementation:

```text
Approval date:
Target environment: LilyGo_TDeck_companion_radio_touch
Retain:
Remove:
Deferred:
Minimum acceptable app-slot headroom:
Approver notes:
```

## Measurement log

No implementation measurements yet.
