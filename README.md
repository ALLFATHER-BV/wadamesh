<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/wadamesh-readme-dark.svg">
    <img alt="WADAMESH" src="assets/wadamesh-readme-light.svg" width="440">
  </picture>
</p>

<p align="center"><b>Une véritable interface utilisateur tactile pour votre radio mesh.</b> &middot; open source &middot; GPL-3.0</p>

Firmware radio compagnon Touch-UI [MeshCore](https://github.com/meshcore-dev/MeshCore)
pour les **LilyGo T-Deck / T-Deck Plus** et **Heltec V4 / Heltec V4 Expansion Kit**
(ESP32-S3).

L'interface utilisateur tactile LVGL — carte, chat, contacts, canaux et reglages —
est derivee de
[meshcomod](https://github.com/ALLFATHER-BV/meshcomod). L'application s'appuie
sur une version derivee de MeshCore via `lib_deps` dans PlatformIO.

## Cartes

- LilyGo T-Deck / T-Deck Plus — env `LilyGo_TDeck_companion_radio_touch`
- Heltec V4 / Heltec V4 Expansion Kit (Heltec V4 + TFT + tactile CHSC6x) — env `heltec_v4_tft_companion_radio_usb_tcp_touch`

## Architecture

Ce depot contient uniquement l'**application** : l'interface `companion_radio`,
l'UI LVGL `ui-touch`, les adaptations/variantes des deux cartes, et
`platformio.ini`. Le **coeur MeshCore n'est pas integre ici** : il est recupere
comme bibliotheque via `lib_deps` depuis le monorepo
[`ALLFATHER-BV/meshcomod`](https://github.com/ALLFATHER-BV/meshcomod) (le meme
depot que le firmware non tactile), epingle par un tag Git `core-*` allege ne
contenant que les sources. Les fichiers de l'application tactile maintenus dans
ce depot (`TouchPrefsStore`, `WifiRuntimeStore`, les transports, etc.) sont
retires de la bibliotheque via `-DMC_VENDORED_TOUCH_APP` afin d'eviter une double
compilation. Le build obtenu est identique octet par octet au firmware meshcomod
original compile dans le depot source.

## Construire

[PlatformIO](https://platformio.org/) recupere automatiquement le fork du coeur
et toutes les bibliotheques :

```bash
pio run -e heltec_v4_tft_companion_radio_usb_tcp_touch   # Heltec V4 / Heltec V4 Expansion Kit
pio run -e LilyGo_TDeck_companion_radio_touch            # LilyGo T-Deck
# ou simplement `pio run` pour compiler les deux
```

Flashez avec la chaine a 4 composants preservant le NVS (bootloader /
partitions / boot_app0 / firmware a `0x0 / 0x8000 / 0xe000 / 0x10000`) afin de
conserver les identifiants Wi-Fi enregistres. N'utilisez pas une image fusionnee,
qui remplit avec `0xFF` et efface le NVS.

## Contribuer

Les contributions sont les bienvenues — voir [CONTRIBUTING.md](CONTRIBUTING.md).
Un seul sujet par pull request ; les contributions entrantes sont acceptees sous
la licence GPL-3.0 du projet.

## Licence

**GPL-3.0-or-later** — voir [LICENSE](LICENSE). wadamesh est sous licence
copyleft : toute personne qui distribue un build ou un fork doit aussi rendre son
code source disponible sous GPL. Cela permet de garder l'interface utilisateur
ouverte et de concentrer les efforts de la communaute au lieu de les fragmenter
en forks fermes.

wadamesh integre et depend de
[MeshCore](https://github.com/meshcore-dev/MeshCore) (MIT, © Scott Powell /
rippleradios.com) ainsi que d'autres composants tiers — voir [NOTICE](NOTICE)
pour la liste complete et leurs licences. Les fichiers derives de MeshCore
conservent leurs mentions MIT ; l'ensemble distribue reste sous GPL
(la licence MIT est compatible GPL). Le fork MeshCore utilise par wadamesh reste
volontairement sous licence **MIT** afin que ses integrations Wi-Fi/BLE puissent
rester compatibles avec MeshCore en amont.
