# Onca

> An open-source Atari Jaguar emulator core.

> **Onca is an early work in progress.**
> It is not yet feature complete and the majority of games won't boot correctly.

## About

Onca is a clean-room implementation of an Atari Jaguar emulator core written entirely from scratch

No existing emulator source code (Virtual Jaguar, MAME, BigPEmu) was consulted at any point. The implementation was built from publicly circulated Atari hardware documentation (the Tom & Jerry Technical Reference Manual) and the officially released Jaguar Doom source code.

The project aims to provide a portable, maintainable, and accurate implementation. The long-term goal is a standalone emulator core that can be used directly or integrated into frontends such as Libretro.

Current compatibility is sparse: a handful of games run well, but the rest are incompatible to say the least. I will try my best to improve it when I find the time. In the meantime, the libretro fork of Virtual Jaguar is already well maintained, so I refer you to them if you run into issues.

## Features

Current status:

- ✅ Clean-room implementation
- ✅ Portable C codebase
- ✅ Libretro core
- ✅ Motorola 68000 CPU
- ✅ Object Processor (video compositor)
- ✅ BIOS support (cartridge boot ROM; the Jaguar CD boot ROM is not supported)
- 🚧 Jaguar GPU (Tom RISC)
- 🚧 Blitter - Most of the work is done but I suspect there are lingering issues
- 🚧 Memory subsystem
- 🚧 Audio (DSP) - Sound FX works on most games but Music still WIP
- 🚧 Compatibility improvements - honest caveat intermittent freezes observed on Doom

## Usage

Onca requires a user-supplied Jaguar boot ROM: place `jagboot.rom` (the 128 KB cartridge boot ROM) in your frontend's system directory.

Supported content:

- `.j64` / `.rom` / `.bin` cartridge images
- `.jag` dev-kit executables (loaded directly into RAM, no BIOS required)

## Known issues:

Music timbre: synthesized music plays notes at the right times but sounds garbled (under investigation; sound effects are correct)
Cartridge EEPROM works in-session but isn't saved to disk yet, so game settings don't survive a restart
No save states (retro_serialize not implemented)
NTSC only, single controller only, no Team Tap
JagLink/ComLynx networking isn't emulated (co-op connect attempts can be aborted with Option)
No Jaguar CD support

## Compatibility:

Doom: fully playable start to finish, with sound
Alien vs Predator: launches but renders a black screen
Dev-kit .jag executables: several homebrew demos run; some homebrew cartridge images don't boot yet

## Building

```bash
cmake -B build
cmake --build build
```

Compiled libraries will be produced in the build output.

## Releases

Every tagged release includes prebuilt binaries for supported platforms.

Assets currently include:

- `onca_libretro.dll` (Windows)
- `onca_libretro.so` (Android arm64)
- `onca_libretro_so` (Linux)

## Project Structure

```
docs/       Documentation
src/        Emulator source
tests/      Unit tests
tools/      Development utilities
```

## Design Goals

- Clean-room implementation
- Portable C
- Easy to understand
- Well documented
- Unit tested where practical
- Suitable for long-term maintenance

## Legal

Onca is an independent open-source project and is **not affiliated with, endorsed by, or sponsored by Atari.**

Users must provide their own legally obtained BIOS files and game software where required.

## License

Licensed under GPL-3.0-or-later.

See the NOTICE file and the full license text in COPYING.
