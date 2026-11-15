#!/usr/bin/env python3
"""
DevoJK settings menu generator.

The DevoJK menu has two pages - Vulkan and DevoGI - and lives in two files:

    assets/japro/ui/jamp/devojk.menu          (main menu: Setup -> DevoJK)
    assets/japro/ui/jamp/ingame_devojk.menu   (in game:   Setup -> DevoJK)

Writing those by hand means copying the same 25-line block a hundred times, so
they are generated from the two tables below instead. To add a setting, add a
line to a table and run:

    python tools/devojk_menu.py

Each line is:  (cvar, label, kind, values, help)
    kind "list":   values = [(label, value), ...]   - click to cycle through
    kind "slider": values = (default, min, max)     - drag a bar
The help text shows at the bottom of the screen while the mouse is over the line.
A * at the end of a label means the change only takes effect after "Apply".
"""

import os

ONOFF = [("Off", 0), ("On", 1)]

# ---------------------------------------------------------------------------
# Page 1: Vulkan - the renderer's own settings (what used to live in autoexec.cfg).
# Left out on purpose, because the Vulkan renderer doesn't read them at all:
# r_fbo, r_vbo, r_vbo_models (rend2 leftovers), r_ext_compress_textures,
# r_ext_compress_lightmaps, r_texturebitslm, r_gammaShaders,
# r_environmentMapping, r_dlightBacks.
# ---------------------------------------------------------------------------
VULKAN = [
    ("r_hdr", "HDR rendering *", "list", ONOFF, "Renders in high dynamic range, so bright light and glow don't clip."),
    ("r_presentBits", "Colour depth *", "list", [("8 bit", 24), ("10 bit", 30)], "Colour bits of the final picture. 10 bit needs a monitor that supports it."),
    ("r_ext_multisample", "Anti-aliasing *", "list", [("Off", 0), ("2x", 2), ("4x", 4), ("8x", 8)], "MSAA: smooths jagged edges. 8x looks best, 4x is a good middle."),
    ("r_ext_alpha_to_coverage", "Alpha to coverage *", "list", ONOFF, "Smooth edges on grass, fences and leaves too (with anti-aliasing on)."),
    ("r_ext_supersample", "Supersampling *", "list", ONOFF, "Renders every pixel several times. Very sharp, very slow."),
    ("r_renderScale", "Render scale mode *", "list", [("Off", 0), ("Nearest", 1), ("Nearest, bars", 2), ("Linear", 3), ("Linear, bars", 4)], "How a custom render resolution is scaled to the screen."),
    ("r_ext_max_anisotropy", "Max anisotropy *", "list", [("1", 1), ("2", 2), ("4", 4), ("8", 8), ("16", 16)], "Upper limit for anisotropic filtering."),
    ("r_ext_texture_filter_anisotropic", "Anisotropic filtering *", "list", [("Off", 0), ("2x", 2), ("4x", 4), ("8x", 8), ("16x", 16)], "Keeps floors and walls sharp at a glancing angle."),
    ("r_textureMode", "Texture filter", "strlist", [("Trilinear", "GL_LINEAR_MIPMAP_LINEAR"), ("Bilinear", "GL_LINEAR_MIPMAP_NEAREST"), ("Pixelated", "GL_NEAREST")], "How textures are smoothed between their pixels."),
    ("r_picmip", "Texture detail *", "list", [("Full", 0), ("Half", 1), ("Quarter", 2)], "Lower saves video memory on old cards."),
    ("r_nomip", "Lower map textures only *", "list", ONOFF, "Texture detail only lowers the map's textures, never players and weapons."),
    ("r_texturebits", "Texture quality *", "list", [("Default", 0), ("16 bit", 16), ("32 bit", 32)], "Colour bits per texture pixel."),
    ("r_simpleMipMaps", "Simple mipmaps *", "list", ONOFF, "Faster, slightly blurrier way to make the smaller texture levels."),
    ("r_roundImagesDown", "Round images down *", "list", ONOFF, "Odd-sized textures are rounded down instead of up."),
    ("r_detailtextures", "Detail textures *", "list", ONOFF, "Extra fine texture layers some maps use up close."),
    ("r_subdivisions", "Curve detail *", "list", [("Highest", 1), ("High", 2), ("Normal", 4), ("Low", 8), ("Lowest", 16)], "How finely curved walls and arches are cut into triangles."),
    ("r_lodCurveError", "Curve level of detail", "list", [("Normal", 250), ("High", 1000), ("Highest", 10000)], "Higher keeps distant curves round."),
    ("r_lodbias", "Model detail", "list", [("Highest", 0), ("High", 1), ("Normal", 2)], "Which level of detail models start at."),
    ("r_lodscale", "Model detail distance", "list", [("Normal", 5), ("Far", 10), ("Farthest", 20)], "How far away models stay detailed."),
    ("r_vertexLight", "Vertex lighting *", "list", ONOFF, "Old, flat lighting for very slow PCs. Turns lightmaps (and DevoGI) off."),
    ("r_normalMapping", "Normal maps *", "list", ONOFF, "Bumpy surface detail. Needed for live DevoGI lighting."),
    ("r_specularMapping", "Specular maps *", "list", ONOFF, "Shiny highlights. Needed for live DevoGI lighting."),
    ("r_genNormalMaps", "Generate normal maps *", "list", ONOFF, "Guesses bump detail for textures that have no normal map."),
    ("r_cubeMapping", "Reflections *", "list", ONOFF, "Reflections from cubemaps on shiny surfaces."),
    ("r_deluxeMapping", "Deluxe maps", "list", ONOFF, "Light direction from the map's deluxemaps, when it has them."),
    ("r_deluxeSpecular", "Deluxe specular", "slider", (1.0, 0.0, 1.0), "How strong highlights from deluxemaps are."),
    ("r_baseNormalX", "Bump strength X *", "slider", (1.0, 0.0, 2.0), "Sideways strength of normal-map bumps."),
    ("r_baseNormalY", "Bump strength Y *", "slider", (1.0, 0.0, 2.0), "Up-down strength of normal-map bumps."),
    ("r_baseParallax", "Parallax depth *", "slider", (0.05, 0.0, 0.2), "How deep parallax-mapped surfaces look."),
    ("r_baseSpecular", "Base shininess *", "slider", (0.04, 0.0, 0.2), "Shininess of surfaces that have no specular map."),
    ("r_ssao", "Screen-space AO *", "list", [("Off", 0), ("SSAO", 1), ("GTAO", 2)], "Darkens corners in real time. DevoGI already bakes its own, so Off is fine."),
    ("r_dynamiclight", "Dynamic lights", "list", ONOFF, "Light from sabers, blaster bolts and explosions."),
    ("r_dlightMethod", "Dynamic light method", "list", [("CPU", 0), ("GPU", 1)], "GPU is faster and needed for live DevoGI lighting."),
    ("r_dlightMode", "Dynamic light mode", "list", [("0", 0), ("1", 1), ("2", 2)], "2 lights models and the map alike."),
    ("r_DynamicGlow", "Glow *", "list", ONOFF, "Glowing sabers, light sources and screens."),
    ("r_DynamicGlowAllStages", "Glow on every layer *", "list", ONOFF, "Also glow the layers the original game skipped."),
    ("r_DynamicGlowIntensity", "Glow strength", "slider", (0.8, 0.0, 2.0), "How bright the glow is."),
    ("r_bloom", "Bloom *", "list", ONOFF, "Soft light bleeding around very bright spots."),
    ("r_flares", "Lens flares", "list", ONOFF, "Flares around lights."),
    ("r_drawSun", "Draw the sun", "list", ONOFF, "Draws the sun disc in the sky."),
    ("r_fastsky", "Fast sky", "list", ONOFF, "Plain colour instead of the sky. Faster, much uglier."),
    ("r_drawfog", "Fog", "list", [("Off", 0), ("Legacy", 1), ("Collapsed", 2)], "Collapsed is the faster way to draw the same fog."),
    ("r_surfaceSprites", "Grass and sprites *", "list", ONOFF, "Grass, snow and other little sprites some maps put on their ground."),
    ("r_patchStitching", "Patch stitching", "list", ONOFF, "Closes tiny cracks between curved surfaces."),
    ("r_marksOnTriangleMeshes", "Marks on models", "list", ONOFF, "Blaster marks and scorches on map models too."),
    ("r_dither", "Dithering", "list", ONOFF, "Hides colour banding in smooth gradients."),
]

# ---------------------------------------------------------------------------
# Page 2: DevoGI - DevoJK's lighting, and the saber looks.
# Settings marked "rebake" change the baked file: the map bakes again the next
# time it loads (or prebake it with the Precompile tool).
# ---------------------------------------------------------------------------
DEVOGI = [
    ("r_devoGI_live", "PERFORMANCE MODE *", "list", [("Low spec", 0), ("High spec", 1)], "Low spec: everything baked, fastest. High spec: live sun and light sources with real-time shadows."),
    ("r_devoGI", "DevoGI lighting *", "list", ONOFF, "New baked global illumination for every map. Off = the maps' original lighting."),
    ("r_devoGI_prebake", "Bake in the game", "list", ONOFF, "Bake a map's lighting when it loads without a baked file. Off: use the Precompile tool."),
    ("r_devoGI_prebakeSet", "Prebake set", "list", [("Current mode", 0), ("High spec", 1), ("Low spec", 2), ("Both", 3)], "Which versions to bake when a map loads. Both: switching mode never waits."),
    ("r_devoGI_scale", "Lightmap detail * (rebake)", "list", [("1x", 1), ("2x", 2), ("4x", 4), ("8x", 8), ("12x", 12), ("16x", 16)], "Sharper shadows and corners. 12x needs about 9 GB of RAM while baking."),
    ("r_devoGI_rays", "Rays per sample * (rebake)", "list", [("32", 32), ("64", 64), ("128", 128), ("256", 256), ("512", 512)], "More is smoother bounce light, slower bake."),
    ("r_devoGI_bounces", "Bounces * (rebake)", "list", [("2", 2), ("4", 4), ("8", 8), ("16", 16)], "How often light bounces in low spec mode."),
    ("r_devoGI_liveBounces", "High spec bounces * (rebake)", "list", [("8", 8), ("16", 16), ("24", 24), ("32", 32)], "How often light bounces in high spec mode."),
    ("r_devoGI_aa", "Shadow smoothing * (rebake)", "slider", (2.0, 0.5, 4.0), "0.5 = crisp shadow edges, 2 = smooth, 4 = very soft."),
    ("r_devoGI_ao", "Ambient occlusion * (rebake)", "slider", (1.6, 0.0, 2.0), "How dark corners and crevices get."),
    ("r_devoGI_aoDistance", "AO reach * (rebake)", "list", [("64", 64), ("128", 128), ("256", 256), ("512", 512)], "How far corner darkening reaches, in game units."),
    ("r_devoGI_sky", "Sky light * (rebake)", "slider", (0.5, 0.0, 2.0), "Soft light from the visible sky."),
    ("r_devoGI_liveSky", "High spec sky light * (rebake)", "slider", (6.0, 0.0, 10.0), "Sky light in high spec mode, where the sky lights all the shade."),
    ("r_devoGI_saturation", "Colour bleeding * (rebake)", "slider", (1.6, 0.5, 3.0), "How much colour bounced light picks up from surfaces."),
    ("r_devoGI_sheen", "Sheen * (rebake)", "slider", (0.12, 0.0, 0.5), "A faint glossy pick-up of whatever faces a surface."),
    ("r_devoGI_intensity", "Bounce strength * (rebake)", "slider", (1.0, 0.0, 2.0), "Strength of bounce light."),
    ("r_devoGI_exposure", "Exposure * (rebake)", "slider", (1.0, 0.5, 2.0), "Overall brightness. 1 matches the original map."),
    ("r_devoGI_gpu", "Bake on the graphics card", "list", ONOFF, "Much faster bakes, same result."),
    ("r_devoGI_progress", "Bake progress screen", "list", ONOFF, "Shows a progress bar while a map bakes."),
    ("r_devoGI_sunShadow", "Live sun shadows", "list", ONOFF, "High spec: real-time sun shadows from players and everything that moves."),
    ("r_devoGI_sunShadowSize", "Sun shadow detail *", "list", [("1024", 1024), ("2048", 2048), ("4096", 4096)], "Sharper sun shadows, slower."),
    ("r_devoGI_sunShadowRange", "Sharp sun shadow reach", "slider", (768, 256, 2048), "How far around you the sharp sun shadows reach."),
    ("r_devoGI_sunShadowRangeFar", "Far sun shadow reach", "slider", (4096, 1024, 8192), "How far the coarse sun shadows reach. Beyond: baked."),
    ("r_devoGI_lampShadows", "Near-light shadows", "list", [("Off", 0), ("1", 1), ("2", 2), ("3", 3), ("4", 4)], "How many lights close to you throw the shadows of moving things."),
    ("r_devoGI_lampShadowSize", "Near-light shadow detail *", "list", [("256", 256), ("512", 512), ("1024", 1024)], "Sharper near-light shadows, slower."),
    ("r_devoGI_lampShadowRange", "Near-light shadow reach", "slider", (1024, 256, 2048), "Lights closer than this may throw shadows."),
    ("cg_saberTrail", "Saber trail", "list", [("Off", 0), ("Normal", 1), ("Normal, smooth", 2), ("SFX", 3)], "The trail sabers leave while swinging."),
    ("cg_saberTrailLength", "Saber trail length", "slider", (1.0, 0.1, 4.0), "1 = normal, 2 = twice as long."),
    ("cg_shaderSaberCore", "SFX saber core", "slider", (0.625, 0.2, 2.0), "Thickness of the white core."),
    ("cg_shaderSaberGlow", "SFX saber glow width", "slider", (0.625, 0.2, 2.0), "Width of the coloured glow."),
    ("cg_shaderSaberGlowAlpha", "SFX saber glow strength", "slider", (1.0, 0.0, 2.0), "Lower keeps the core readable on bright ground."),
    ("cg_speedTrail", "Speed trail", "list", ONOFF, "Trail behind players using force speed."),
]

# ---------------------------------------------------------------------------
# Layout: a 640x480 screen, two columns per page.
# ---------------------------------------------------------------------------
COL_X = (24, 326)
COL_W = 290
TOP = 104
ROW = 13
VALUE_AT = 175      # where the value starts inside a row (labels end here)
GOLD = "1 .682 0 1"


def q(text):
    return '"' + text.replace('"', "'") + '"'


def item(page, idx, rows_per_col, entry, visible):
    cvar, label, kind, values, help_text = entry
    col = idx // rows_per_col
    row = idx % rows_per_col
    x = COL_X[col]
    y = TOP + row * ROW
    # "type" has to come first: the game sets up an item's storage as soon as it reads
    # "font" (or a cvar), sized for the type the item has at that moment. A list that
    # got its type later would write its options past the end of that storage.
    item_type = "ITEM_TYPE_SLIDER" if kind == "slider" else "ITEM_TYPE_MULTI"
    lines = [
        "\t\titemDef",
        "\t\t{",
        f"\t\t\tname\t\t\t\tdvj_{cvar}",
        f"\t\t\ttype\t\t\t\t{item_type}",
        f"\t\t\tgroup\t\t\t\t{page}",
        f"\t\t\ttext\t\t\t\t{q(label)}",
        f"\t\t\trect\t\t\t\t{x} {y} {COL_W} {ROW - 1}",
        "\t\t\ttextalign\t\t\tITEM_ALIGN_RIGHT",
        f"\t\t\ttextalignx\t\t\t{VALUE_AT - 8}",
        "\t\t\ttextaligny\t\t\t-3",
        "\t\t\tfont\t\t\t\t2",
        "\t\t\ttextscale\t\t\t0.55",
        f"\t\t\tforecolor\t\t\t{GOLD}",
        f"\t\t\tdescText\t\t\t{q(help_text)}",
        f"\t\t\tvisible\t\t\t\t{1 if visible else 0}",
    ]
    if kind == "slider":
        default, lo, hi = values
        lines += [
            f"\t\t\tcvarfloat\t\t\t{q(cvar)} {default} {lo} {hi}",
        ]
    else:
        lines += [f"\t\t\tcvar\t\t\t\t{q(cvar)}"]
        if kind == "strlist":
            lines.append("\t\t\tcvarStrList { " + " ".join(f"{q(l)} {q(v)}" for l, v in values) + " }")
        else:
            lines.append("\t\t\tcvarFloatList { " + " ".join(f"{q(l)} {v}" for l, v in values) + " }")
    lines += [
        "\t\t\taction { play \"sound/interface/button1.wav\" }",
        "\t\t}",
        "",
    ]
    return "\n".join(lines)


def button(name, text, x, y, w, actions, help_text, group="none", visible=True, scale=0.8):
    return "\n".join([
        "\t\titemDef",
        "\t\t{",
        f"\t\t\tname\t\t\t\t{name}",
        "\t\t\ttype\t\t\t\tITEM_TYPE_BUTTON",
        f"\t\t\tgroup\t\t\t\t{group}",
        f"\t\t\ttext\t\t\t\t{q(text)}",
        "\t\t\tstyle\t\t\t\tWINDOW_STYLE_EMPTY",
        f"\t\t\trect\t\t\t\t{x} {y} {w} 20",
        "\t\t\ttextalign\t\t\tITEM_ALIGN_CENTER",
        f"\t\t\ttextalignx\t\t\t{w // 2}",
        "\t\t\ttextaligny\t\t\t0",
        "\t\t\tfont\t\t\t\t3",
        f"\t\t\ttextscale\t\t\t{scale}",
        f"\t\t\tforecolor\t\t\t{GOLD}",
        f"\t\t\tvisible\t\t\t\t{1 if visible else 0}",
        f"\t\t\tdescText\t\t\t{q(help_text)}",
        "\t\t\taction",
        "\t\t\t{",
        "\t\t\t\tplay\t\t\t\"sound/interface/button1.wav\" ;",
        *[f"\t\t\t\t{a} ;" for a in actions],
        "\t\t\t}",
        "\t\t}",
        "",
    ])


def page_items(page, entries, visible):
    rows = (len(entries) + 1) // 2
    return "".join(item(page, i, rows, e, visible) for i, e in enumerate(entries))


def menu(name, back_menu, ingame):
    out = []
    out.append("// DevoJK settings: generated by tools/devojk_menu.py - edit the tables there, not this file\n{\n")
    out.append("\tmenuDef\n\t{\n")
    out.append(f"\t\tname\t\t\t\t\t{q(name)}\n")
    out.append("\t\tfullScreen\t\t\t\t" + ("0" if ingame else "1") + "\n")
    out.append("\t\trect\t\t\t\t\t0 0 640 480\n")
    out.append("\t\tvisible\t\t\t\t\t0\n")
    out.append("\t\tfocusColor\t\t\t\t1 1 1 1\n")
    out.append("\t\tdescX\t\t\t\t\t320\n\t\tdescY\t\t\t\t\t436\n\t\tdescScale\t\t\t\t0.7\n")
    out.append("\t\tdescColor\t\t\t\t1 .682 0 .8\n\t\tdescAlignment\t\t\tITEM_ALIGN_CENTER\n\n")
    out.append("\t\tonOpen\n\t\t{\n\t\t\tshow\t\tpage_vulkan\n\t\t\thide\t\tpage_devogi\n"
               "\t\t\tsetitemcolor\ttabs\tforecolor\t1 .682 0 1\n\t\t\tsetitemcolor\ttab_vulkan\tforecolor\t1 1 1 1\n\t\t}\n\n")
    out.append(f"\t\tonESC\n\t\t{{\n\t\t\tplay\t\t\"sound/interface/button1.wav\"\n\t\t\tclose\t\t{name}\n\t\t\topen\t\t{back_menu}\n\t\t}}\n\n")
    # a dark backdrop with a thin frame, title and tabs
    out.append("\t\titemDef\n\t\t{\n\t\t\tname\t\t\t\tbackdrop\n\t\t\tstyle\t\t\t\tWINDOW_STYLE_FILLED\n"
               "\t\t\trect\t\t\t\t12 40 616 420\n\t\t\tbackcolor\t\t\t0 0 .12 .92\n\t\t\tborder\t\t\t\t1\n"
               "\t\t\tbordercolor\t\t\t.2 .3 .8 1\n\t\t\tvisible\t\t\t\t1\n\t\t\tdecoration\n\t\t}\n\n")
    out.append("\t\titemDef\n\t\t{\n\t\t\tname\t\t\t\ttitle\n\t\t\ttext\t\t\t\t\"DEVOJK SETTINGS\"\n"
               "\t\t\trect\t\t\t\t24 46 300 20\n\t\t\tfont\t\t\t\t3\n\t\t\ttextscale\t\t\t1\n"
               "\t\t\tforecolor\t\t\t1 1 1 1\n\t\t\tvisible\t\t\t\t1\n\t\t\tdecoration\n\t\t}\n\n")
    out.append(button("tab_vulkan", "VULKAN", 326, 46, 140,
                      ["show page_vulkan", "hide page_devogi", "setitemcolor tabs forecolor 1 .682 0 1",
                       "setitemcolor tab_vulkan forecolor 1 1 1 1"],
                      "The renderer's own settings.", group="tabs"))
    out.append(button("tab_devogi", "DEVOGI", 470, 46, 140,
                      ["show page_devogi", "hide page_vulkan", "setitemcolor tabs forecolor 1 .682 0 1",
                       "setitemcolor tab_devogi forecolor 1 1 1 1"],
                      "DevoJK lighting, baking and sabers.", group="tabs"))
    out.append("\t\titemDef\n\t\t{\n\t\t\tname\t\t\t\tnote\n\t\t\ttext\t\t\t\t\"* takes effect after Apply\"\n"
               "\t\t\trect\t\t\t\t24 76 300 14\n\t\t\tfont\t\t\t\t2\n\t\t\ttextscale\t\t\t0.5\n"
               "\t\t\tforecolor\t\t\t.7 .7 .7 1\n\t\t\tvisible\t\t\t\t1\n\t\t\tdecoration\n\t\t}\n\n")
    out.append(page_items("page_vulkan", VULKAN, True))
    out.append(page_items("page_devogi", DEVOGI, False))
    # bottom row
    out.append(button("apply", "APPLY", 60, 410, 150, ["exec \"vid_restart\""],
                      "Restarts the renderer so the settings marked * take effect."))
    out.append(button("rebake", "REBAKE THIS MAP", 245, 410, 150, ["exec \"r_devoGI_rebuild 1; vid_restart\""],
                      "Bakes the current map's lighting again, with the settings above."))
    out.append(button("back", "BACK", 430, 410, 150, [f"close {name}", f"open {back_menu}"],
                      "Back to the setup menu."))
    out.append("\t}\n}\n")
    return "".join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ui = os.path.join(here, "..", "assets", "japro", "ui", "jamp")
    for file, name, back, ingame in (("devojk.menu", "devojk_menu", "setup_menu", False),
                                     ("ingame_devojk.menu", "ingame_devojk", "ingame_setup", True)):
        with open(os.path.join(ui, file), "w", newline="\r\n") as f:
            f.write(menu(name, back, ingame))
        print("wrote", file)


if __name__ == "__main__":
    main()
