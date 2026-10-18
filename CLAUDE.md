# CLAUDE.md - mgba-EncounterCounter

Daniel's fork of the libretro mGBA core / mGBA for the PokéCounter (Gen 3: Ruby, Sapphire, Emerald, FireRed,
LeafGreen, all languages). Branch **`master`**, CI workflow **"EncounterCounter Build"**
(`.github/workflows/encountercounter.yml`; other workflows manual only) builds the RetroArch core for Android arm64
(`mgba_libretro_android`, for RetroArch on his AYN Thor), the Windows core (`mgba_libretro_windows`) and a Windows
mGBA program (SDL, gamepads); release `encountercounter-latest`.

## What the fork adds

- `src/feature/encounter-counter.c/.h` - C tracker (`ecCreate/ecSetGame/ecFrame/ecStateLoaded`), watches
  gMain.inBattle, decodes gEnemyParty (Gen 3 PK3), sends t=enc; shiny auto-pause (`ecTakeShinyEvent`,
  hold blocks reset/state loading). RAM addresses from the pret decompilations and pokebot-gen3 symbol tables.
- Hooks: `src/platform/libretro/libretro.c`, `src/core/thread.c`, a field in `include/mgba/core/thread.h`,
  Qt glue in `src/platform/qt/CoreController.h`, `Window.cpp`; CMake additions.

## Status

- Older than the other senders: **no** team (t=mon), PC boxes, EV toast or auto-hunt yet - those exist in
  gen1recomp's PokeCounter mod (Gen 3) and melonDS (Gen 4). He mostly plays Gen 3 in gen1recomp now; mGBA work
  ("fanfare fast-forward" etc.) was put on hold.

## Owner and how to work with him

- Owner: Daniel (GitHub `remiind`). **Reply in German.** Network/security engineer, comfortable with technical detail.
- No upstream PRs (he declined). Work on the fork's branch named below and push there; CI builds the downloads.
- Session artifacts cannot be downloaded from a Claude session (GitHub blob storage answers 403): give him the
  Actions run link + artifact name and let him download. Logs of failed runs are also blocked; read the errors
  from the check-run annotations instead (`gh api repos/<repo>/check-runs/<id>/annotations`).
- Never kill processes with `pkill -f <pattern>` in the session shell (it matches and kills the shell itself);
  check `/proc/<pid>/cmdline` and kill by pid.
- His Windows PC (when the session is linked): `C:\Users\dschr\Desktop\pokecounter\` holds the display firmware
  project (`pokecounter\`), gen1recomp builds (`gen1recompPRJ\`) and melonDS (`C:\Users\dschr\Desktop\melonDS`).
  Large files go over the device bridge in parts of ≤25 MB into `gen1recompPRJ\_to_delete\`, then are joined
  with device_bash and checked with sha256.

## The PokéCounter system (shared by all four forks)

A desk display (Waveshare ESP32-S3-Touch-LCD-1.47, PlatformIO/Arduino, LVGL; web UI at http://pokecounter.local)
counts Pokémon encounters sent by the emulators/games over UDP.

- Firmware: **not on GitHub**, lives on the PC in `C:\Users\dschr\Desktop\pokecounter\pokecounter` (src/: main.cpp,
  net.cpp, store.cpp, hunts.cpp, boxes.cpp, sdlog.cpp, team.cpp, ui.cpp, web_index.h). Flash it from VS Code
  (PlatformIO "Upload", env `pokecounter`, USB) or via the web UI firmware upload (Verwaltung tab, password in
  `include/secrets.h`). **Never print or copy `secrets.h`** (Wi-Fi credentials). Builds made in a cloud session use a
  placeholder `secrets.h` - never hand those .bin files out (that once put the display off Wi-Fi).
- Transport: JSON datagrams to UDP port 4210 of the display; the display answers `{"ok":true,...}`. Senders find it
  via `pokecounter.local` (resolver, then a direct mDNS query) or broadcast, pin the first replying address,
  repeat a report until acknowledged (300 ms gap after an ack, 0.4 s → 2 s retries) and never drop reports.
- Packets:
  - `{"t":"enc","sp":<natdex>,"lv":..,"g":"M|F|N","shiny":bool,"pid":..,"eid":<random>,"ivs":<packed>,"ab":..,"it":..,
    "tsv":<TID^SID>,"src":"<4-char game code>"[,"egg":1][,"masuda":1]}` - one encounter. `eid` makes repeats
    harmless. IVs packed HP|Atk<<5|Def<<10|Spe<<15|SpA<<20|SpD<<25. `masuda`: egg from parents of different
    languages (Gen 4 rolls the PID 5x; the display counts it like 5 encounters for the odds).
  - `{"t":"mon","slot":1..6,"n":<party size>,...}` - party member for the Team tab (IVs, EVs, stats, moves, names).
  - `{"t":"box","b":1..18,"h":0|1,"nb":..,"cur":..,"name":"..","m":[[slot,sp,lv,fl,pid,ivs,it,ab,"nick",ball],..]}` -
    half a PC box per datagram (fits 1500 bytes); fl = shiny | gender<<1 | egg<<3.
  - `{"t":"ping"}` - find the display.
- Display features that depend on the senders: per-edition profiles (by `src`), target hunt ("Ziel-Jagd") with
  odds incl. Masuda, finished hunts tab, shinies, history on SD card, Team, Boxes, shiny melody.
- Shiny auto-pause in every sender: on a shiny the game pauses once the display confirmed it (max 1.5 s), and
  reset/state loading stay blocked until resumed by hand.
