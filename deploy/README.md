# wadamesh.com infrastructure

Distribution stack for wadamesh: a **VPS nginx origin behind Cloudflare**.

```
tiles.wadamesh.com     →CF (HTTP, edge-cached) →  nginx → OpenStreetMap / OpenTopoMap
firmware.wadamesh.com  →CF (cache bins)        →  nginx → /srv/wadamesh/firmware
flasher.wadamesh.com   →CF (HTTPS)             →  301 → wadamesh.com
wadamesh.com           →CF (HTTPS)             →  nginx → /srv/wadamesh/site
```

**Map tile styles.** The default `/{z}/{x}/{y}.jpg` route serves **OpenStreetMap**
(the firmware default). An opt-in **OpenTopoMap** topographic style is served from
`/opentopo/{z}/{x}/{y}.jpg` (explicit OSM alias at `/osm/...`); the device requests
it only when the user enables *Map → Options → Topographic map*. Legal: OpenTopoMap
map tiles are **© OpenTopoMap (CC-BY-SA)** over **© OpenStreetMap contributors
(ODbL) + SRTM** — the touch UI shows that attribution when topo is active, and the
14-day disk cache keeps each tile hitting OpenTopoMap at most once per fortnight
(their tile-usage policy asks for a contactable UA + caching, both of which the
transcode service provides). Deploying the topo routes = update
`tiles.wadamesh.com.conf` + `tile-transcode.py`, then
`systemctl restart wadamesh-tile-transcode && nginx -t && systemctl reload nginx`
and purge the Cloudflare cache for `tiles.wadamesh.com/opentopo/*`.

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

## Done

- **Web flasher** ✅ — the guided install page at `wadamesh.com`, served from
  `deploy/site/` and published by `scripts/deploy-site.sh` (esp-web-tools / Web
  Serial, per-board install buttons + .bin downloads, manifests generated per
  release by `scripts/build/gen-flasher-meta.py` into the `/latest/` and
  `/latest-beta/` feeds that `release.sh` refreshes each publish).
- **`flasher.wadamesh.com` 301-redirects to the apex** (see
  `deploy/nginx/flasher.wadamesh.com.conf`) — it is an alias, not its own page.
- **`deploy/flasher/`** is the original standalone flasher page. **No deploy
  script publishes it** — `deploy-site.sh` ships `deploy/site/` only — so it is
  effectively an offline/local copy kept in parity by hand. Retire it or wire it
  into a deploy target; until then, treat `deploy/site/index.html` as the only
  install page users can reach.

## TODO before public launch

- **Re-enable OTA-over-Wi-Fi** in the firmware (currently it version-checks then
  defers to manual flashing).
- **Flip `wadamesh` repo public** = launch.
- Decide tile-proxy sharing: dedicated `tiles.wadamesh.com` (this config) vs
  reusing the meshcomod proxy.

> Never commit the VPS IP, SSH keys, or `WADAMESH_VPS`. Cloudflare fronts the
> origin; the deploy target is supplied via the environment at publish time.
