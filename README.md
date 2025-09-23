# InfinityEngine-Enhancer

Engine graphical enhancement mod for Baldur's Gate Enhanced Edition.

## Features

### Tile Rendering

First feature of the mod, it allow "upscaled" tiles to be used in engine and detecting it through the TIS header. By
itself it won't do any changes as you'll need a mod that upscale the content.

The big upside is that all coordinate like ARE/WED are untouched and it's virtually compatible with everything (apart
from TIS/PVRZ if you use the upscale mod).

## Requirements

- **Game**: Baldur's Gate Enhanced Edition
- **Platform**: Windows only
- **Graphics**: OpenGL compatible GPU
- **Loader**: [EEex](https://github.com/Bubb13/EEex) framework

## Installation

1. Install [EEex](https://github.com/Bubb13/EEex) first
2. Download `InfinityEngine-Enhancer.dll` from [Releases](../../releases)
3. Place the DLL in your game's root directory
4. Place the [loading script](tools/M_IEEE.lua) in your `override`folder

A `.ini` and `.log` file will be created if it's installed correctly.

## Development

Built with C++20, CMake, and modern tooling:

- **MinHook** - Function hooking
- **spdlog** - Logging
- **Pattern scanning** - Dynamic address resolution

## Thanks

- **[Bubb13](https://github.com/Bubb13)** - For creating the [EEex](https://github.com/Bubb13/EEex) framework that makes
  this possible
- **[Gibberlings3](https://gibberlings3.github.io)** - For documenting Infinity Engine file formats
- **[Claude Code](https://claude.ai/code)** - For collaborative development assistance

## License

See LICENSE file for details.