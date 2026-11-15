# DevoJK manual

A short guide to what DevoJK adds and how to set it up. Everything here can be changed in the game under **Setup → DevoJK**, which has two pages: **Vulkan** (the renderer) and **DevoGI** (lighting, baking and sabers).

## What is DevoGI?

DevoGI gives every map new lighting. It works out how light bounces around the map (global illumination), with soft shadows, darker corners and colour picked up from the walls. All of it is worked out once per map, "baked", and saved next to the map as a `.devogi` file. After that, the map loads at normal speed with its new lighting.

Maps made for rend2, which bring their own modern lighting, are left alone.

## Performance mode

The most important setting, at the top of the DevoGI page.

| | Low spec | High spec |
|---|---|---|
| Cvar | `r_devoGI_live 0` | `r_devoGI_live 1` |
| Sun and lamps | baked | drawn live |
| Shadows from players | none | from the sun, and from lights close to you |
| Bounce light | baked | baked, plus soft sky light |
| Baked file | `maps/<map>.devogi` | `maps/<map>.live.devogi` |
| Cost | as fast as the original game | a few frames per second |

High spec also needs **Normal maps**, **Specular maps** and the **GPU dynamic light method**, all on by default.

### How high spec handles lamps

The work is split by distance, so far-away lamps cost nothing:

* **Far away:** the lamp's light and its soft shadows are baked.
* **In view:** the lamp is also drawn live for its shine, but it keeps its baked shadows.
* **Right by you:** up to 4 nearby lights (lamps, sabers, blaster bolts) also throw the shadows of anything that moves: players including yourself, weapons and doors. The level itself never has to be drawn for these shadows, since its own are baked. That keeps them cheap.

## Baking

The first time a map loads with a performance mode it hasn't been baked for, the game bakes it and shows a progress bar. How long that takes depends on the map, the PC and the **Lightmap detail** setting: at 12x, a map like ffa3 takes about 5 to 8 minutes and up to 9 GB of RAM.

Settings marked **(rebake)** in the menu change the baked file. A map bakes again the next time it loads after you change one.

### Bake in the game, and the prebake set

* **Bake in the game** (`r_devoGI_prebake`): On bakes missing maps as they load. Off never bakes in the game. A map without a baked file then keeps its original lighting, and you prepare files with the Precompile tool instead.
* **Prebake set** (`r_devoGI_prebakeSet`): what to bake when a map loads.
  * **Current mode** (default): only what your performance mode needs.
  * **High spec** or **Low spec**: that version too.
  * **Both**: both versions. Switching performance mode later never waits, but the first load takes twice as long.

### The Precompile tool

`Precompile Shaders.exe` sits in the game folder next to `mapcompiler.exe`. It bakes many maps in one go, outside the game, while you do something else.

1. Start it. It lists every map it finds: the original game's, and those in any map packs (pk3 files) in `base` and `EternalJK`.
2. Tick the maps you want (or press **All**).
3. Choose **High spec**, **Low spec** or **Both**.
4. Press **Prebake**. The maps bake one after another, and **Stop** ends the bake that's running.

It reads your settings from `EternalJK/eternaljk.cfg`, so the files it makes are exactly what the game expects. Change bake settings in the game first, then prebake.

## Recommended settings

These are the defaults, and what DevoJK was tuned with.

| Setting | Recommended | Notes |
|---|---|---|
| Performance mode | Low spec on older PCs, High spec on a decent graphics card | |
| Lightmap detail | 12x | Use 4x on a PC with less than 16 GB of RAM |
| Bounces | 16 (low spec), 24 (high spec) | |
| Rays per sample | 128 | |
| Shadow smoothing | 2 | |
| Ambient occlusion | 1.6, reach 256 | |
| Sky light | 0.5 (low spec), 6 (high spec) | |
| Sun shadow detail | 2048 | 4096 for very sharp sun shadows |
| Near-light shadows | 4, detail 512 | Fewer for more frames per second |
| Anti-aliasing | 8x | 4x is a good middle |
| Screen-space AO | Off | DevoGI already bakes its own |

## Sabers

On the DevoGI page:

* **Saber trail** `cg_saberTrail`: SFX (3) is the modern look. Its trail now only shows while you swing, like the classic one.
* **Saber trail length** `cg_saberTrailLength`: 1 is normal, 2 is twice as long.
* **SFX saber core / glow width / glow strength** `cg_shaderSaberCore`, `cg_shaderSaberGlow`, `cg_shaderSaberGlowAlpha`: lower the glow strength if the white core disappears against bright, sunlit walls.

## All DevoJK settings

For the console or an autoexec.

| Cvar | Default | What it does |
|---|---|---|
| `r_devoGI` | 1 | DevoGI on or off |
| `r_devoGI_live` | 0 | Performance mode: 0 low spec, 1 high spec |
| `r_devoGI_prebake` | 1 | Bake missing maps in the game |
| `r_devoGI_prebakeSet` | 0 | 0 current mode, 1 high spec too, 2 low spec too, 3 both |
| `r_devoGI_scale` | 12 | Lightmap detail, 1 to 16 |
| `r_devoGI_rays` | 128 | Rays per bounce-light sample |
| `r_devoGI_bounces` | 16 | Light bounces, low spec |
| `r_devoGI_liveBounces` | 24 | Light bounces, high spec |
| `r_devoGI_aa` | 2 | Shadow smoothing, 0.5 to 4 |
| `r_devoGI_ao` / `r_devoGI_aoDistance` | 1.6 / 256 | Corner darkening and its reach |
| `r_devoGI_sky` / `r_devoGI_liveSky` | 0.5 / 6 | Sky light, and its high spec multiplier |
| `r_devoGI_saturation` | 1.6 | Colour picked up by bounced light |
| `r_devoGI_sheen` | 0.12 | Faint glossy pick-up |
| `r_devoGI_intensity` / `r_devoGI_exposure` | 1 / 1 | Bounce strength, overall brightness |
| `r_devoGI_gpu` | 1 | Bake on the graphics card |
| `r_devoGI_progress` | 1 | Progress bar while baking |
| `r_devoGI_rebuild` | 0 | Set to 1 to bake the next map again |
| `r_devoGI_sunShadow` | 1 | Live sun shadows (high spec) |
| `r_devoGI_sunShadowSize` | 2048 | Sun shadow detail |
| `r_devoGI_sunShadowRange` / `...RangeFar` | 768 / 4096 | Reach of the sharp and the coarse sun shadows |
| `r_devoGI_lampShadows` | 4 | Lights close to you that throw moving shadows, 0 to 4 |
| `r_devoGI_lampShadowSize` | 512 | Their shadow detail |
| `r_devoGI_lampShadowRange` | 1024 | How close a light must be |
| `r_devoLights` | 0 | Preview the map's lights as plain dynamic lights (for mappers) |

## For mappers

DevoQ3Map2 ships the same baker: `mapcompiler -devogi [-live] [options] maps/mp/yourmap.bsp` writes the `.devogi` file. Ship it inside your pk3 next to the bsp, and players get the new lighting with no bake at all. Run `mapcompiler -help devogi` for every option.
