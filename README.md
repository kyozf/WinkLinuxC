# WinkLinuxC

WinkLinuxC is a lightweight Linux distribution built around a custom C-based init system and shell

## Features

- Custom WinkShell
- Custom init system
- WinkFetch system information utility
- Alpine Linux userspace
- Linux kernel
- Limine bootloader
- x86_64 bootable ISO

## Current version

**0.2** — Initial public release

## Building

The root `CMakeLists.txt` builds the C programs:

- `init`
- `sh`
- `winkfetch`

Full ISO creation currently uses the project's separate build setup

## Booting

Download the WinkLinuxC ISO from the [GitHub Releases](https://github.com/kyozf/WinkLinuxC/releases) page

The current release targets x86_64 systems

## Project status

WinkLinuxC is an experimental project and is under active development
