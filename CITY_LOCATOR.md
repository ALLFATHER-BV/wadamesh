# City locator

Shows which city or town you are in, from the GPS, with no internet and no mesh traffic.
Boundary files can be built for the US, Quebec and Ontario.

- With a GPS fix the radio checks every 10 seconds which place (city, town,
  municipality or census-designated place) contains your position.
- Crossing into a new place shows a pop-up, "Now in Roseville". With **Settings, GPS,
  City on Home** switched on (it is off by default), the Home screen's top-left corner
  shows the place name instead of the node name; outside every place it goes back to
  the node name.
- With the GPS switched off the place clears at once, and after 10 minutes without a fix.
- A new place must be seen on two checks in a row (about 20 s) before it is shown, so
  GPS wobble on a boundary does not make the name flicker.
- Nothing is transmitted. Your position stays on the device.

Built into every board with an SD card, a GPS and PSRAM (`CAP_CITY_LOCATOR` in
`src/ui-touch/device_caps.h`), except the T-LoRa Pager, whose flash is full. It does
nothing until a `cities.bin` is on the card.

## The boundary file

The firmware reads `cities.bin` from the SD card's top folder, or from `/transfer`
(where the Wi-Fi **Transfer** app puts uploads, for boards with an internal card).
It is read once into PSRAM at the first GPS fix after boot, a few KB per pass of the main
loop so the screen and the mesh keep running; restart after replacing it.

Build one from US Census TIGER/Line place boundaries (public domain):

```
pip install pyshp shapely
# download tl_2024_SS_place.zip for your state (06 = California) from
# https://www2.census.gov/geo/tiger/TIGER2024/PLACE/ and unzip it
python3 scripts/build/make_cities.py tl_2024_06_place cities.bin                       # Sacramento-Foothills area (default box)
python3 scripts/build/make_cities.py tl_2024_06_place cities.bin -125 32 -114 42.1    # all of California (about 2.1 MB)
```

To build a whole state (any of the 50, plus DC = `11` and Puerto Rico = `72`):

```
python3 scripts/build/make_cities.py --state tl_2024_48_place cities-texas
```

That writes `cities-texas.bin`, or, if it would be over 4,000,000 bytes, splits the state
along its longer direction into `cities-texas-north.bin` / `-south.bin` (or `-east` /
`-west`), and again if needed (`-northeast` ...). A place crossing a dividing line goes
in both parts.
With the 2024 data every state fits in one file; the biggest are Texas and California
(about 2.1 MB each).

Boundaries are simplified to about 5 m. Census-designated places are statistical
areas, not legal city limits. Files up to 4 MB are accepted.

## Canada (Quebec, Ontario)

`scripts/build/make_cities_ca.py` builds the same file from Canadian municipal boundaries,
optionally mixed with US places so one file can cross the border. It uses
`make_cities.py` for the encoding and the 4 MB split.

- **Quebec:** MRNF "Découpages administratifs" 1:20 000, the `munic_s` layer of
  [`SDA.shp.zip`](https://www.donneesquebec.ca/recherche/dataset/decoupages-administratifs)
  (CC-BY 4.0). Unorganized territories (TNO) are stored like census-designated places,
  after the municipalities.
- **Ontario:** Land Information Ontario "Municipal Boundary - Lower and Single Tier"
  (Open Government Licence - Ontario). It is only offered as a web service, so
  `fetch-ontario` downloads the municipalities inside a box first, 20 per request, one
  request every 5 s (`--delay`). Names are title-cased (`MCNAB/BRAESIDE` becomes
  `McNab/Braeside`).
- **US:** TIGER/Line places as above; `--us` can be given once per state.

A place goes in when it touches `--box`, or the band `--buffer-km` wide on each side of a
`--route` of `lon,lat` points. `--us-box` adds a second area for US places only (a strip
along the border, say).

```
cd work   # wherever the downloads are
# Quebec: download SDA.shp.zip from the link above and unzip it into SDA/
python3 path/to/scripts/build/make_cities_ca.py fetch-ontario on -76.5 44.5 -74.3 46.0
python3 path/to/scripts/build/make_cities_ca.py build cities --qc SDA/munic_s --on on \
    --route="-73.57,45.50;-75.70,45.42" --buffer-km 50                 # Montreal to Ottawa
```

Give `--route` with `=` so the leading minus of the first longitude is not read as an
option. Larger areas split into `-north` / `-south` ... files like `--state` above. All
of Quebec does not fit: its large northern territories keep one part over 4 MB after
the splits, so pick a box or route.

**Credit:** CC-BY 4.0 and the Ontario licence ask for a credit line wherever a built file
is shared, for example "Contains data from the Gouvernement du Québec (MRNF, CC-BY 4.0)
and information licensed under the Open Government Licence - Ontario."

## Code

- `src/helpers/CityLocator.{h,cpp}`: file parser and point-in-polygon lookup (plain C++,
  host-testable).
- `src/ui-touch/UITask.cpp`: `cityLocatorTick()` (sliced load, lookup, debounce, pop-up),
  the Home status-bar name and the City on Home switch on the GPS page.
- `test/test_city_locator.cpp`: host test for the parser and lookup
  (`scripts/run-host-tests.sh` runs it).
