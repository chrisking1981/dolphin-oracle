# Dolphin Oracle

A fork of the [Dolphin GameCube/Wii emulator](https://github.com/dolphin-emu/dolphin) that adds a
**TCP control protocol** for automated test harnesses, port debugging, and game-state oracles.

Inspired by [TwitchPlaysPokemon/dolphinWatch](https://github.com/TwitchPlaysPokemon/dolphinWatch)
(based on Dolphin 5.0, 2015), but rebased onto modern Dolphin master so you get **Vulkan**,
**RVZ ROM support**, and 9+ years of emulation-accuracy improvements.

## Why this exists

Standard Dolphin can run GameCube/Wii games great, but it has no programmatic API to:

- Save/load savestates by filename
- Read/write emulated PowerPC memory
- Get notified when memory changes (push events)
- Inject controller input deterministically (no SendKeys)
- Trigger screenshots from outside the emulator

This fork adds all of that over a simple newline-delimited TCP protocol on a configurable port.

## Use cases

- **Game porting**: compare your port's memory state to Dolphin's at the exact same moment
- **Speedrun research**: precise frame-N captures, deterministic input replays
- **Game reverse-engineering**: watch a memory address change in real-time
- **CI test harnesses**: drive Dolphin headlessly with full state inspection

## Protocol

TCP server, default port `6000`, newline-delimited commands. (Full set comes online as the fork
matures — see `CHANGELOG_ORACLE.md`.)

| Command | Action |
|---|---|
| `SAVE <abs_path>` | Save savestate to file |
| `LOAD <abs_path>` | Load savestate from file |
| `READ <8\|16\|32> <addr_hex>` | Read PowerPC memory |
| `WRITE <8\|16\|32> <addr_hex> <val_hex>` | Write PowerPC memory |
| `WRITE_MULTI <addr_hex> <hex_blob>` | Atomic batch write in 1 frame |
| `SUBSCRIBE <8\|16\|32> <addr_hex>` | Push event on memory change |
| `SUBSCRIBE_MULTI <count> <addr_hex>` | Watch a memory range |
| `UNSUBSCRIBE <addr_hex>` | Stop watching |
| `BUTTONSTATES_GC <pad> <mask> <sx> <sy> <csx> <csy>` | Set GC controller state |
| `PAUSE` / `RESUME` / `RESET` / `STOP` | Emulation control |
| `SPEED <factor>` | 0 = unlimited, 0.5 = slow-mo |
| `SCREENSHOT <abs_path>` | **New** — save current framebuffer (DolphinWatch lacked this) |

## Building

Identical to vanilla Dolphin on Windows. See [Building for Windows](Readme.md#building-for-windows).
For Visual Studio 2026 specifics, see [`chris-zo-bouwen.md`](chris-zo-bouwen.md).

After build, `Binary/x64/Dolphin.exe` is your oracle-enabled Dolphin.

Launch with `Dolphin.exe -w 6000 -e your-game.iso` to enable the oracle TCP server.

## Status

🚧 **Hello-world stage** — the build hook is in, TCP server lands in subsequent commits.

## License

Same as upstream Dolphin: GPL-2.0-or-later. See `COPYING`.

## Acknowledgements

- [Dolphin Emulator](https://github.com/dolphin-emu/dolphin) team for the underlying emulator
- [TwitchPlaysPokemon/dolphinWatch](https://github.com/TwitchPlaysPokemon/dolphinWatch) for the
  TCP-protocol design that inspired this fork
