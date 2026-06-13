# wadamesh.com infrastructure

Distribution stack for wadamesh: a **VPS nginx origin behind Cloudflare**.

```
tiles.wadamesh.com     →CF (HTTP, edge-cached) →  nginx → tile.openstreetmap.org
firmware.wadamesh.com  →CF (cache bins)        →  nginx → /srv/wadamesh/firmware
flasher.wadamesh.com   →CF (HTTPS)             →  web flasher        (TODO — see below)
wadamesh.com           →CF (HTTPS)             →  landing page       (later)
```

The firmware fetches **tiles + the update-check over plain HTTP** (on-device HTTPS
isn't viable — mbedTLS needs ~30 KB heap, only ~5 KB is free post-Wi-Fi), so the
tile + firmware hosts must stay reachable over HTTP. Cloudflare provides the edge
cache, HTTPS for the flasher, and hides the origin IP (so no IP lives in this repo
or the firmware).

## 1. VPS (origin)

```bash
sudo apt install nginx
sudo mkdir -p /srv/wadamesh/firmware/releases/TOUCH /var/cache/nginx/wadamesh-tiles
sudo cp deploy/nginx/tiles.wadamesh.com.conf    /etc/nginx/sites-available/
sudo cp deploy/nginx/firmware.wadamesh.com.conf /etc/nginx/sites-available/
sudo ln -s /etc/nginx/sites-available/tiles.wadamesh.com.conf    /etc/nginx/sites-enabled/
sudo ln -s /etc/nginx/sites-available/firmware.wadamesh.com.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

## 2. Cloudflare

- **DNS:** `A`/`AAAA` records for `tiles`, `firmware`, `flasher`, `@` → the VPS IP,
  all **Proxied** (orange cloud).
- **SSL/TLS:** mode **Flexible** (CF↔origin HTTP) is enough since the origin is
  HTTP-only. **Do NOT enable "Always Use HTTPS"** on `tiles.` or `firmware.` — the
  firmware needs plain HTTP there.
- **Cache Rules:**
  - `tiles.wadamesh.com/*` → Eligible for cache, Edge TTL ~14d.
  - `firmware.wadamesh.com/releases/*/*.bin` → cache, Edge TTL ~1d.
  - `firmware.wadamesh.com/releases/TOUCH` (the listing) → short TTL (~60s) or
    Bypass, so new releases appear promptly.

## 3. Publishing a release

From a wadamesh checkout (builds both boards, refreshes the listing, rsyncs up):

```bash
WADAMESH_VPS=user@your-vps scripts/release.sh beta_2
```

The on-device check GETs `http://firmware.wadamesh.com/releases/TOUCH`, finds the
highest `beta_<N>`, and (once OTA-over-Wi-Fi is re-enabled) pulls
`…/releases/TOUCH/beta_<N>/<board>.bin`.

## TODO before public launch

- **Web flasher** (`flasher.wadamesh.com`) — esptool-js / Web Serial install page
  with a board picker. Not yet built; the firmware already points "update manually
  at flasher.wadamesh.com" here.
- **Re-enable OTA-over-Wi-Fi** in the firmware (currently it version-checks then
  defers to manual flashing).
- **Landing page** at `wadamesh.com`.
- Decide tile-proxy sharing: dedicated `tiles.wadamesh.com` (this config) vs
  reusing the meshcomod proxy.

> Never commit the VPS IP, SSH keys, or `WADAMESH_VPS`. Cloudflare fronts the
> origin; the deploy target is supplied via the environment at publish time.
