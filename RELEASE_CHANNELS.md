# WadaMesh release channels — Stable + Test

Goal: ship **test** builds to the community fast, let them shake out, then **promote**
a proven test build to **stable**. What ships to stable is exactly what was tested
(promote the binary, don't rebuild).

## Two concepts, kept separate

- **Branches** = where code lives.
- **Channels** = what a device / the flasher pulls (Stable vs Beta).

## Branches

- **`main`** — active line. All work lands here; every *test* release is cut from `main`. (Unchanged from today.)
- **`stable`** — promoted line. Moves only when a test build is blessed. Starts at the **beta_20** commit.
- Promote = `git branch -f stable <blessed beta commit>` + push, then run the stable-promotion publish.

## Versioning / tags

- **Test builds:** `beta_N` (immutable, incrementing), marked **GitHub pre-release**. Also mirrored to a rolling **`beta-latest`** pre-release (overwritten each time) for the Launcher beta sync.
- **Stable:** the blessed `beta_N`, marked **GitHub Latest**. Keep the `beta_N` id for now; friendly `vX.Y` names are an optional later polish (would need a rebuild-with-tag per promotion).
- First stable = **beta_20**.

## Distribution matrix

| Target | Stable | Test / Beta |
|---|---|---|
| GitHub release | blessed `beta_N`, marked **Latest** | every `beta_N`, **pre-release** + rolling `beta-latest` |
| VPS archive | `releases/TOUCH/<beta_N>/` (only blessed lands here) | `releases/BETA/<beta_N>/` |
| VPS channel feed | `stable/` (version.json + bins) | `beta/` (version.json + bins) |
| Flasher (flasher.wadamesh.com) | default | "Beta / testing" toggle |
| On-device update check | stable feed (legacy firmware reads `releases/TOUCH`) | beta feed (Phase 2 setting) |
| LauncherHub (bmorcelli, T-Deck) | existing entry (follows GitHub Latest) | new "WadaMesh (Beta)" entry (Phase 3) |
| Tanmatsu app-store | promote on stable only | (skipped for tests) |

## The linchpin: keeping field devices on stable

Legacy firmware's update check scans `releases/TOUCH/` for the highest `beta_N`. So:

- **Test builds archive under `releases/BETA/`** — invisible to legacy firmware.
- **Promotion copies the blessed build into `releases/TOUCH/`** — legacy devices then see it and show "update available," landing everyone on stable.

Net effect: existing devices quietly settle on **stable (beta_20)** and stop chasing
every beta; beta testers opt in via the flasher Beta toggle (or, later, the on-device
Beta channel).

## Two workflows

### Cut a TEST release (fast, frequent) — from `main`
1. `beta_N` = next number.
2. Build both touch envs with the tag embedded.
3. GitHub: create `beta_N` release **`--prerelease`** with the 7 assets; also refresh the rolling `beta-latest` pre-release.
4. VPS: rsync to `releases/BETA/<beta_N>/` + refresh the `beta/` feed.
5. Flasher Beta toggle now serves it. Skip LauncherHub / Tanmatsu / site. (~minutes)

### Promote to STABLE (deliberate, when proven)
1. `git branch -f stable <beta_N commit>` + push.
2. GitHub: mark that `beta_N` release **Latest** (clear pre-release).
3. VPS: copy `releases/BETA/<beta_N>/` → `releases/TOUCH/<beta_N>/`; refresh `stable/` + `latest/` (flasher stable) feeds.
4. Full fan-out: LauncherHub follows automatically; push the Tanmatsu store; bump the site "what's new."

## Phases

### Phase 1 — channel split, no firmware change (do now)
- [ ] Create + push `stable` branch at beta_20.
- [ ] VPS: stand up `stable/`, `beta/`, `releases/BETA/`; point `latest/` (flasher stable) at beta_20; freeze `releases/TOUCH/` at beta_20.
- [ ] GitHub: ensure beta_20 is **Latest**; future betas → `--prerelease`.
- [ ] Flasher: Stable (default) / Beta toggle + the beta manifest.
- [ ] `release.sh` / `gen-flasher-meta.py`: add `--channel beta|stable` producing the right feed/assets + the rolling `beta-latest`.
- [ ] Rewrite the `wadamesh-release` skill into the two-workflow form.

### Phase 2 — on-device channel selector (ships in a beta, then promoted)
- [ ] Settings → **Update channel: Stable / Beta**, default Stable (persisted).
- [ ] `verchkFetchLatest` reads the `stable/` or `beta/` feed per the setting.
- [ ] Beta devices get the new-beta banner automatically; stable devices never see betas.

### Phase 3 — LauncherHub beta listing (PR to bmorcelli, when wanted)
- [ ] Rolling `beta-latest` already produced in Phase 1.
- [ ] PR `bmorcelli/M5Stack-json-fw`: add a `wadamesh_beta.py` (mirror `bruce_beta.py`, track `beta-latest`) + a `t-deck.json` (Beta) entry.
- [ ] Result: T-Deck Launcher shows **WadaMesh** + **WadaMesh (Beta)**, both auto-synced.
- Until then: T-Deck Launcher beta testers side-load the beta `.bin` manually (it is a T-Deck-only gap; V4 uses the flasher).

## Where current work lands

- The uncommitted post-beta_20 work (auto-advert GUI, message-flash, docs, RAM pass)
  plus the hardened **MQTT-privacy** branch → commit to `main` → cut as **beta_21**,
  the first TEST release in this model. Community tests it; promote when proven.
- This is exactly the model that handles PR #71: land the reviewed/hardened pieces on
  `main`, ship as a beta, promote later.

## Decisions

- **Locked:** `main` + `stable`; keep `beta_N` ids with a Stable/Beta channel label; Phase 1 now, Phase 2 in beta_21, Phase 3 as a fast-follow.
- **Open (reversible):** whether stable later gets friendly `vX.Y` names (needs a rebuild-with-tag per promotion).
