# DevoJK
DevoJK is a Jedi Knight: Jedi Academy multiplayer client by **Devoted**. It's built on [JKSunny's EternalJK](https://github.com/JKSunny/EternalJK) with the Vulkan PBR renderer, which itself builds on [EternalJK](https://github.com/eternalcodes/EternalJK), [jaPRO](https://github.com/videoP/jaPRO) and [OpenJK](https://github.com/JACoders/OpenJK).

## Goals
* **Better JA++ compatibility:** work well on servers running the [JA++](https://github.com/Razish/japp) mod.
* **Global illumination:** more natural, bounced lighting on top of the Vulkan PBR renderer.
* **Improved mapping tools:** better compiled lighting and shader support through [DevoQ3Map2](https://github.com/Dadunito/DevoQ3Map2), so maps can take full advantage of the renderer.

> **Status:** early, work in progress. Changes so far are listed under [What's new](#whats-new).

## What's new
* **SFX sabers in the menus:** with Saber Trail set to **SFX** (`cg_saberTrail 3`), the saber preview in the profile and saber menus now uses the SFX blade, matching what you see in game. The blade glow and core size follow `cg_shaderSaberGlow` and `cg_shaderSaberCore`. Ported from [JKEnhanced](https://github.com/JKEnhanced/OpenJK) by redsaurus.
* **Black SFX blades fixed:** black sabers no longer get a random colour with SFX sabers on.

## Credits
DevoJK only exists thanks to the people who brought Vulkan and incredible graphics to Jedi Knight, and everyone who kept the game alive before them. Thank you to:
* **[JKSunny](https://github.com/JKSunny)**, for the Vulkan renderer, PBR and many graphics improvements in EternalJK. DevoJK is built directly on this work.
* **[ec-](https://github.com/ec-) and the [Quake3e](https://github.com/ec-/Quake3e) contributors**, whose Vulkan renderer the EternalJK one is based on, along with [kennyalive's Quake-III-Arena-Kenny-Edition](https://github.com/kennyalive/Quake-III-Arena-Kenny-Edition) and [suijingfeng's vkQuake3](https://github.com/suijingfeng/vkQuake3), which started it.
* **[eternal](https://github.com/eternalcodes)**, for creating and maintaining EternalJK, and **[bucky](https://github.com/Bucky21659)** for contributing to it.
* **[loda](https://github.com/videoP)**, for jaPRO and the client-side features EternalJK is built around.
* **[Razish](https://github.com/Razish)** and the JA++ contributors, for the JA++ mod.
* **The [OpenJK](https://github.com/JACoders/OpenJK) team** (JACoders), including ensiform, Razish, xycaleth, eezstreet, mrwonko and many more, for years of engine fixes and improvements.
* **redsaurus and [JKEnhanced](https://github.com/JKEnhanced/OpenJK)**, for SFX sabers.
* **Raven Software and Activision**, for making Jedi Academy and releasing its source code.

## For players
1. Build DevoJK (see below), or use a release once one is available.
2. Copy the files into your Jedi Academy `GameData` folder. For Steam, that's `<Steam Folder>/steamapps/common/Jedi Academy/GameData/`.
3. Run `eternaljk.x86_64.exe`. The Vulkan renderer is selected with `cl_renderer rd-vulkan`.

## For developers
### Building
DevoJK uses CMake. On Windows with Visual Studio:
```
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix <install folder>
```
The install folder gets the client, both renderers (`rd-vulkan` and `rd-eternaljk`) and the `EternalJK` folder with the game modules and assets.

### Contributing
Bug reports, suggestions and pull requests are welcome under [issues](https://github.com/Dadunito/DevoJK/issues).

Fixes that also apply upstream are worth sending to [JKSunny/EternalJK](https://github.com/JKSunny/EternalJK) as well.

### Vulkan support
From the EternalJK README by JKSunny: Vulkan support was first started by porting [Quake-III-Arena-Kenny-Edition](https://github.com/kennyalive/Quake-III-Arena-Kenny-Edition), then took its file structure from [vkQuake3](https://github.com/suijingfeng/vkQuake3/tree/master/code), and is now based on [Quake3e](https://github.com/ec-/Quake3e), which is actively maintained and has many additions. A list of those additions is [here](https://github.com/ec-/Quake3e#user-content-vulkan-renderer).

## License
[![License](https://img.shields.io/github/license/Dadunito/DevoJK.svg)](LICENSE.txt)

DevoJK, like OpenJK, is licensed under GPLv2 as free software. You are free to use, modify and redistribute it following the terms in [LICENSE.txt](LICENSE.txt). See [rv-readme.txt](rv-readme.txt) for Raven's original source release notes.

## Author
* **Devoted**
