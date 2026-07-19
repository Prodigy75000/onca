# Atari Jaguar memory map

Big-endian, 24-bit external address bus. No separate I/O space: everything is
memory-mapped on the one bus shared by the 68000 and the Tom/Jerry RISC cores.
Addresses below come from the public Tom & Jerry Technical Reference Manual;
the "confirmed" notes mark what the boot ROM was actually observed touching in
`onca_trace bios/jagboot.rom` (see the POST walk-through at the bottom).

## Regions

| Range | Size | Contents |
|-------|------|----------|
| `$000000–$1FFFFF` | 2 MB | Main DRAM |
| `$800000–$DFFFFF` | ≤6 MB | Cartridge ROM (optional; open bus if absent) |
| `$E00000–$E1FFFF` | 128 KB | Boot ROM |
| `$F00000–$F0FFFF` | 64 KB | **Tom**: video / Object Processor / Blitter / GPU |
| `$F10000–$F1FFFF` | 64 KB | **Jerry**: DSP / timers / joystick / audio |

### Boot overlay

On reset the boot ROM is mirrored across low memory so the 68000's reset-vector
fetch (SSP at `$000000`, PC at `$000004`) returns ROM. The reset PC is
`$E00008` — an absolute high-ROM address — so the very first instruction fetch
lands in the real ROM alias; the core drops the mirror on that first access to
`$E00000+`, and low memory reads as DRAM thereafter. Writes always reach DRAM.

## Tom registers touched by POST (confirmed)

| Offset | Name | Notes |
|--------|------|-------|
| `$F00000` | MEMCON1 | first write, `$1861` |
| `$F00002` | MEMCON2 | `$35DD` |
| `$F00006` | VC | vertical count, **read** — synthesised free-running (half-lines) |
| `$F00028` | VMODE | video mode / enable, `$06C1` |
| `$F0002A/2C` | BORD1/2 | border colour |
| `$F0002E` | HP | horizontal period |
| `$F00030–$F0003E` | HBB/HBE/HS/HVS/HDB1/HDB2/HDE/VP | NTSC horizontal + vertical timing |
| `$F00040–$F0004C` | VBB/VBE/VS/VDB/VDE/VEB/VEE | vertical blank/sync/display window |
| `$F0004E` | VI | vertical interrupt line, `$0207` |
| `$F00020` | OLP | object list pointer, `$70000003` — the startup-logo display list |
| `$F000E0/E2` | INT1/INT2 | CPU interrupt enable/mask, `$1F01` |
| `$F02100–$F02114` | GPU control | G_FLAGS etc |
| `$F02200–$F0226C` | Blitter | A1_BASE/A1_FLAGS/A1_PIXEL/B_COUNT/… |
| `$F02238` | B_CMD | **blitter command/status**; read bit0 = idle. blits complete synchronously, so status reads idle |
| `$F03000–$F03FFF` | GPU RAM | 4 KB local RAM, zeroed by POST |

## Jerry registers touched by POST (confirmed)

| Offset | Name | Notes |
|--------|------|-------|
| `$F10012/14` | JPIT1/2 | programmable timer |
| `$F1A10C` | D_FLAGS neighbourhood | DSP control |
| `$F14000` | JOYSTICK | write column strobe (`$810E/810D/810B/8107`), read matrix |
| `$F14002` | JOYBUTS | buttons + board config; seeded idle-high (`$1F`) |

## What the boot ROM does (observed order)

1. Configure the memory controller (MEMCON1/2).
2. Read JOYBUTS for board/config detection.
3. Program the full NTSC video-timing register set.
4. Initialise Jerry timers and the GPU/DSP control registers.
5. Zero the 4 KB GPU RAM.
6. Copy its working routine into DRAM and run from there (`~$005000` region).
7. Kick blitter clears, spinning on B_CMD bit0 (idle) between each.
8. Build the Object Processor list and write OLP (`$F00020`) — the logo objects.
9. Set the vertical-interrupt line (VI) and enable video (VMODE) + CPU
   interrupts (INT1); interrupt mask drops to 0.
10. Enter the per-field main loop, scanning the joystick matrix and servicing
    the level-2 video interrupt.

The boot walk-through above was established during CPU bring-up: real boot code
demonstrably executes all the way to the running main loop before any video is
composited.

## Synthesised register surface (values produced rather than replayed)

- **VC / HC** — free-running from the CPU cycle counter so wait-for-scanline and
  VBLANK-timed loops make progress (same role the CLIO line counter played for
  the 3DO core).
- **B_CMD ($F02238) bit0** — always idle, so blitter-wait spins terminate
  without running an actual blit.
- **JOYBUTS ($F14002)** — idle-high buttons plus a seeded config byte.
- Everything else reads back its last written value; genuinely unmapped reads
  return 0 and are logged. None were hit on the boot path.
