# Graphics Options for PCs Without the HD Pack — Design

Date: 4 Nov 2026. Status: agreed in chat with the user ("está perfecto, crea el plan").

## Intent

Some players cannot use the HD texture pack because their PC is weak. They should still get a cleaner image,
with options that cost little. PPSSPP's texture scaling is the reference the user had in mind.

## What the user asked for (their words, 4 Nov 2026)

- "darle esa opción aparte a los jugadores que no puedan usar las texturas, escalado mejor, suavizado ... filtrado
  anisotrópico, etc"
- Why they can't use the textures: "por la pc débil".
- Agreed proposal: texture upscaling plus SMAA/FXAA as new launcher options; anisotropic filtering and MSAA left
  out ("está perfecto").

## Goals

1. **Texture upscaling (xBRZ, as in PPSSPP).**
   - Launcher option *Escalado de texturas*: Apagado / xBRZ 2x / 3x / 4x. Default Apagado.
   - It scales the game's own textures. Patapon's flat-colour silhouettes stay sharp instead of blurred.
   - Scaling runs on a worker thread and is cached, so playing never hitches.
   - The original texture is drawn until the scaled one is ready, as the HD pack does today.
2. **Screen anti-aliasing.**
   - Launcher option *Suavizado*: Apagado / FXAA / SMAA. Default Apagado.
   - One post-process at the internal resolution, before the existing scale-to-window blit.
   - FXAA is the lightest; SMAA looks better and costs a little more.
3. Each option's launcher description says in plain Spanish/English what it costs, so a weak-PC player can choose.

## Non-goals

- **Anisotropic filtering.** Patapon 3 draws almost everything as planes facing the camera, and the runtime uploads
  no mipmaps, so the change would be invisible.
- **MSAA.** It costs on integrated GPUs and does not smooth alpha-tested sprite edges, which are most of the screen.
- **Other PPSSPP modes** (Hybrid, Bicubic, Deposterize). Later only if asked.
- **An automatic choice per PC.** The launcher's auto render scale stays as it is.
- **Replacing the HD pack.** When the pack has a texture, the pack wins and that texture is not upscaled.

## Anti-goals

These would be failures even if the options technically work:

- Hitches or stutter when a new texture appears: no synchronous scaling on the render thread.
- Re-scaling textures whose content changes every few frames (palette animations): it burns CPU for nothing.
- A VRAM blow-up that crashes weak GPUs. Scaled textures count against the existing GL texture budget.
- Any change at all with both options off: same images, same speed.
- Breaking the ANGLE / OpenGL ES 3.0 path that weak PCs often use.

## Constraints

- Runtime C++17/20 + OpenGL 3.3 core, and GLSL ES 3.0 under ANGLE: shader headers come from `psp_glsl_header()`.
- **Licence.**
  - xBRZ is GPL-3.0. The runtime exe is already distributed under GPL-3.0 (README, kirk), so it fits. Vendor it
    under `runtime/third_party/xbrz/` with its licence header.
  - FXAA and SMAA must come from permissively licensed sources (SMAA by Jorge Jimenez et al. is MIT).
- Settings follow the existing contract:
  - a runtime core key in `psp_settings.cpp`;
  - `docs/contracts/settings-v1.md`;
  - the launcher's `src-tauri/core_options.json`, on the `graphics` tab.
- Never ship game art. Nothing here needs any.

## Decided

- **Option names, values and defaults:**
  - `display.texture_upscale` = `off|xbrz2|xbrz3|xbrz4` → `PSPRECOMP_TEX_UPSCALE`, default `off`;
  - `display.antialiasing` = `off|fxaa|smaa` → `PSPRECOMP_AA`, default `off`.
- **The HD pack wins** over upscaling, per texture.
- **Render targets** (RTT/framebuffer textures) are never upscaled.

## Left to the builder

- How "changes often" is detected. Suggested: a texture address whose content hash changed at least twice in the
  last 60 display lists is skipped.
- The exact FXAA variant and the SMAA quality preset. The preset should be cheap: LOW or MEDIUM.
- Whether the worker uses one thread or a small pool.
