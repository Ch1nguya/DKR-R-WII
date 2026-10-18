# Credits

## Wii Port
- **Ch1nguya** — author of the Wii adaptation of DKR-R: GX renderer, texture/TMEM emulation, native audio mixer, software launcher, input/platform adaptation, debug tooling, testing on hardware.
  Developed with the assistance of Claude (Anthropic).

## Upstream and Dependencies
- **DKR-R** (Diddy Kong Racing Recompiled) by ThatGuyMcd and contributors — MIT licence, copyright "DKR-R contributors". The files listed under "Modified upstream files" below keep that licence.
- **N64Recomp** and **N64ModernRuntime** (`ultramodern`, `librecomp`) — https://github.com/N64Recomp. `runtime-patches/` contains our modifications to them.
- **libogc / devkitPPC** — https://devkitpro.org.
- **Diddy Kong Racing decompilation** by DavidSM64 and contributors — https://github.com/DavidSM64/Diddy-Kong-Racing — used as a reference for the game's behaviour (no code copied).

## Wii-Original Files (MIT Licence, see LICENSE)
`runtime-recomp/src/game/`: `native_audio_mixer.cpp/.hpp`, `wii_atomics.cpp`, `wii_funcprof.cpp`, `wii_launcher.cpp`, `wii_launcher_font.inc`, `wii_netlog.cpp`, `wii_netplay_stub.cpp`,
`wii_renderer_cfg.inc`, `wii_renderer_debug.inc`, `wii_renderer_dlparse.inc`, `wii_renderer_geometry.inc`, `wii_renderer_profile.inc`, `wii_renderer_rdp_state.inc`, `wii_renderer_state.inc`, `wii_textures.inc`.

## Modified Upstream Files (MIT Licence, see LICENSE, changes in UPSTREAM-DIFF.patch)
`runtime-recomp/CMakeLists.txt`, `scripts/apply-dependency-patches.sh`, and in `runtime-recomp/src/game/`: `game_main.cpp`, `game_payload.cpp`, `game_payload_v77.cpp`, `game_registration.cpp`, `null_renderer.cpp/.hpp`
(the renderer was rewritten for GX), `presentation_identity.cpp`, `runtime_enhancements.cpp`, `runtime_platform.cpp`, `runtime_stubs.cpp`, `runtime_support.cpp`, `netplay/datagram_socket.cpp`, `netplay/network_interfaces.cpp`.
