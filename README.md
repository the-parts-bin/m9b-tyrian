# Tyrian on the m9b SoC

OpenTyrian (the C port of Tyrian 2.1) as a program for the SoC in
`../core`. It runs from PSRAM, uses picolibc, and reads the game data from
the SD card through the core firmware's SD driver and FatFs.

**Status:** it runs at the game's full speed (about 35 fps, the rate
Tyrian's own timer sets), with some idle time left, with sound effects,
music (pre-rendered, see Music) and USB keyboard and mouse input. Menus,
the demo, saving and loading work.

Start it from the boot screen's browser (`TYRIAN.BIN` on the card), or
with `../core/tools/run.py build/tyrian.bin`.

**Input:** a USB keyboard and mouse on either of the core's USB ports, with
Tyrian's controls (arrows, Space fires, Enter changes the rear weapon mode,
Esc for the menu, P pauses); the terminal's keys work too (arrows, Enter,
Esc, printable characters).

**License:** GPL v2 (`opentyrian/COPYING`). The game data is Tyrian 2.1,
which its author released as freeware; it isn't in the repository
(`opentyrian/get_data.sh` fetches it).

## Sources

| Part | Upstream | Version | Here |
|---|---|---|---|
| The game | [opentyrian/opentyrian](https://github.com/opentyrian/opentyrian) (the C port of Tyrian 2.1) | `5a9d8da` | `opentyrian/`, with changes (Changes to OpenTyrian) |
| SDL stand-in | [SDL2](https://github.com/libsdl-org/SDL)'s API, for the parts the game uses | | `port/SDL*.h`, written for the port |
| Music renderer | OpenTyrian's own Loudness player and OPL emulator (DOSBox's), built for the PC | the same | `tools/musrender.c` |

## Layout

| Path | Contents |
|---|---|
| `opentyrian/` | OpenTyrian, from upstream commit `5a9d8da`, less its packaging, with small changes (below) |
| `port/SDL.h`, `port/SDL_types.h`, `port/SDL_endian.h` | an SDL2 stand-in: the types, constants and functions the game uses, scancodes as USB HID usages (as in SDL2) |
| `port/m9b_sdl.c` | start-up (traps, interrupts), time, events from the USB keyboards and mice and the UART, 8 bpp surfaces, the sound converter |
| `port/video_m9b.c` | in place of `video.c` and `video_scale.c`: the screen shown line-doubled from three display buffers, flipped at the vertical blank; screenshots over the UART |
| `port/loudness_m9b.c` | in place of `loudness.c` (same API): the effects mixed in the audio interrupt, the music streamed from WAV files |
| `tools/musrender.c`, `tools/make_music.sh` | render the music to those WAV files on a PC with OpenTyrian's own player and OPL emulator, and optionally copy them to the card |
| `tools/demoprof.sh`, `tools/newgame.sh` | load a build and start the demo, or a game, from the UART (tests) |
| `../core/runtime/syscalls.c`, `string.c`, `trap.c`, `trap_entry.S` | the runtime all the ports share (in the core): files through FatFs, the UART console, misaligned access emulation, the trap handler and PC-sampling profiler |

## Building and the SD card

`make` builds `build/tyrian.bin` (about 350 KB); `make B=build-prof
EXTRA=-DM9B_PROFILE` a build that reports frame rate, idle time and sound
time every 5 s, and the PC profile every 15 s, on the UART
(`../core/tools/prof.py ELF LOG` maps the samples to functions).

The game data goes in `data/` (`data/README.md` says what goes there).
The whole card, all the games with their data and music, is built in
m9b-system, which has this repository as a submodule:
`../sdcard/make_sdcard.sh` (see `../sdcard/README.md`) takes the data from
here. The scripts below add or replace files on a card in the board,
through the serial port.

The card needs:
- `TYRIAN/`: the game's data files (`opentyrian/get_data.sh data` fetches
  the freeware zip; everything but the DOS programs and documents, about
  10 MB). The configuration and saves go there too.
- `TYRIAN/MUSIC/SONG00.WAV` .. `SONG40.WAV`: the music (below).
- `TYRIAN.BIN` in the root, for the boot screen's browser.

`../core/tools/sdput/sdput.py put LOCAL REMOTE ...` copies files onto the
card through the board (about 80 KB/s).

## Video

The game draws into its 320 x 200 8 bpp surfaces. `JE_showVGA()` copies
the screen into one of three display buffers, cleans it from the data
cache and has the vertical-blank interrupt point the video engine's line
table at it (all 480 lines, each of the 200 on 2 or 3: DOS's 320 x 200
filled a 4:3 screen), loading the palette
(the game's `rgb_palette[]`, RGB565 through `SDL_MapRGB`) at the same time.

`../core/tools/m9bctl.py shot OUT.png` asks the running game for a
screenshot (a 0x1C byte on the UART; the game sends the size, the palette
and the pixels on screen) and `m9bctl.py keys ...` types keys, so the game
can be driven and checked without looking at the monitor.

## Sound

The audio engine plays 48 kHz 16-bit stereo from two 1 KB halves; the
audio interrupt mixes the next half at 24 kHz (each result held for 2
frames): the music, then the game's 8 effect channels (8-bit 11025 Hz
sounds, widened to 16 bits at load, stepped at 11025/24000).

## Music

Tyrian's songs are Loudness (LDS) tunes for an OPL2. OpenTyrian plays them
through DOSBox's OPL emulator, which computes the envelopes in floating
point for every sample: far too slow without an FPU. Instead,
`tools/make_music.sh DATADIR OUTDIR [--upload]` builds OpenTyrian's player
and emulator for the host and renders each of the 41 songs once through
(until the player loops or stops it) to `SONGnn.WAV`, 24 kHz 16-bit mono at
`loudness.c`'s 2x gain; 128 MB, about 27 minutes to copy through the board. The port streams
the current song from a 256 KB ring in PSRAM, filled from the main loop
(`SDL_PollEvent`, `SDL_Delay`), and loops it. Six songs stop instead of
looping (9, 10, 18, 24, 33, and 30, the secret level item's fanfare): the renderer marks them with an `m9b ` chunk in the WAV file, and the
port ends them, so `playing` goes false and the game goes back to the
level's song, as with the original player. `fade_song()` fades it out
over about 1.5 s. The jukebox uses the stream's `playing` and `songlooped`
state in place of the player's.

## Performance

The profile of the demo first showed two thirds of the time in
`blit_background_row`, which draws the three scrolling background layers a
byte at a time, checking each pixel against the surface's limits; rows
wholly on the surface now skip the checks (`M9B` in `backgrnd.c`), and the
game went from about 17 to 35 fps. What remains is the other blits and the
frame copy (`memcpy`, about 18%).

## Changes to OpenTyrian

Marked `M9B`:
- `src/file.c`: the configuration and saves in `TYRIAN/` (no home
  directory).
- `src/backgrnd.c`: the fast path for background rows (Performance).

Removed from the tree: the upstream `.git`, the Visual C, Linux and macOS
packaging.
