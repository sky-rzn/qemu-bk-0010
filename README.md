# BK-0010 emulator on QEMU

A QEMU fork that emulates the K1801VM1 CPU and the BK-0010 home
computer: display, keyboard, system timer and speaker. The monitor and
BASIC ROMs are included (`pc-bios/bk0010-*.rom`).

The upstream QEMU readme is in [README_QEMU.rst](README_QEMU.rst).

## Building

You need a C compiler, `ninja`, `meson`, Python 3, `pkg-config`, glib2
and pixman, plus SDL2 or GTK3 for the window. On Debian/Ubuntu:

```sh
sudo apt install build-essential ninja-build python3-venv pkg-config \
    libglib2.0-dev libpixman-1-dev libsdl2-dev libpulse-dev
```

Build only the target you need:

```sh
mkdir build && cd build
../configure --target-list=k1801vm1-softmmu --disable-werror
make -j$(nproc)
```

## Running

```sh
build/qemu-system-k1801vm1
```

The `bk0010` machine is the default, and the ROMs are picked up from
the build tree. Sound goes to the default audio backend; to choose one
explicitly:

```sh
build/qemu-system-k1801vm1 -audiodev pa,id=snd -machine bk0010,audiodev=snd
```

Use `-bios file.rom` to load a different monitor ROM.

## Keyboard

| PC key                        | BK key            |
|-------------------------------|-------------------|
| F1                            | LAT               |
| F2                            | RUS               |
| Enter                         | VVOD (Enter)      |
| Backspace                     | ZABOY (Backspace) |
| Esc                           | SBR (Clear)       |
| Arrows, keypad 4/8/6/2        | Arrows            |
