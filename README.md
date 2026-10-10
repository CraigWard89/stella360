# Stella 360 (Atari 2600 for Xbox 360)

A native Atari 2600 video computer system emulator ported for the Xbox 360 (PowerPC Xenon architecture).

Based on the battle-tested Stella core (v3.9.3), **Stella 360** eliminates the dependency on legacy Original Xbox backwards compatibility (`.xbe`) and hidden `HDDX:` partition setups by targeting a standalone native `.xex` executable for modded Xbox 360 consoles (RGH, JTAG, and XeUnshackle/ABadAvatar).

---

## 🎯 Objectives
- **Native Xbox 360 Executable (`.xex`)**: Direct execution from USB (`Usb0:\`) or internal drive (`Hdd1:\`) without Partition 2 / `HDDX:` requirements.
- **Full DashLaunch & XeXMenu Compatibility**: Visible and launchable directly from DashLaunch file browsers and quick-boot button mappings.
- **PowerPC Big-Endian Optimized**: Native big-endian execution (`MSB_FIRST`) optimized for the Xbox 360 Xenon CPU (3 dual-threaded PowerPC cores @ 3.2 GHz).
- **Controller Support**: Full Xbox 360 gamepad mapping for Atari joystick, paddle, and console switches (Select, Reset, Color/BW, Difficulty).

---

## 🛠️ Build Targets
- **Xbox 360 XDK (MSVC 2010)**: `platform=xbox360_msvc2010`
- **Libxenon (GCC PPC)**: Standalone ELF/XEX compilation toolchain via Docker (`free60/libxenon`)

---

## 📜 Upstream Attribution
- Core emulator: [Stella](https://github.com/stella-emu/stella) by Bradford W. Mott, Stephen Anthony, and the Stella Team.
- Libretro implementation: [libretro/stella2014-libretro](https://github.com/libretro/stella2014-libretro).