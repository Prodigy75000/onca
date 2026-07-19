# Onca

> An open-source Atari Jaguar emulator core.

> **Onca is an early work in progress.**
> It is not yet feature complete and the majority of games won't boot correctly.

## About

Onca is a clean-room implementation of an Atari Jaguar emulator core written entirely from scratch.

No existing emulator source code (Virtual Jaguar, MAME, BigPEmu) was consulted at any point. The implementation was built from publicly circulated Atari hardware documentation (the Tom & Jerry Technical Reference Manual) and the officially released Jaguar Doom source code.

The project aims to provide a portable, maintainable, and accurate implementation. The long-term goal is a standalone emulator core that can be used directly or integrated into frontends such as Libretro.

Current compatibility headline: Jaguar Doom boots to gameplay.

## Features

Current status:

- ✅ Clean-room implementation
- ✅ Portable C codebase
- ✅ Libretro core
- ✅ Motorola 68000 CPU
- ✅ Object Processor (video compositor)
- ✅ BIOS support (cartridge boot ROM; the Jaguar CD boot ROM is not supported)
- 🚧 Jaguar GPU (Tom RISC)
- 🚧 Blitter
- 🚧 Memory subsystem
- 🚧 Audio (DSP)
- 🚧 Compatibility improvements

## Usage

Onca requires a user-supplied Jaguar boot ROM: place `jagboot.rom` (the 128 KB cartridge boot ROM) in your frontend's system directory.

Supported content:

- `.j64` / `.rom` / `.bin` cartridge images
- `.jag` dev-kit executables (loaded directly into RAM, no BIOS required)

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
-  `onca_libretro_so` (Linux)

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

See the LICENSE notice and the full license text in COPYING.
