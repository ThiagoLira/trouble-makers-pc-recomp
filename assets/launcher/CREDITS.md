# Launcher artwork

`splash-background.jpg` is the active launcher artwork, generated locally with
ComfyUI 0.15.0 and the Z-Image Turbo model, then resized to the launcher's
2560 x 1600 production canvas. It shows the game's authentic Clancer block face
— two red angled oval eyes and a single black open oval mouth (not a smile) —
on a matte cobalt cube in the lower-left, with the right ~55% kept dark for the
menu.

Method: `comfy-generic-workflow.json` is the text-to-image base. Pure
text-to-image can't render a convincing Clancer (the model doesn't know the
character), so the shipped image is an **img2img** pass (denoise ~0.55)
seeded from a real in-game Clancer block to lock the face shape, colors, and
matte look. That seed frame is game imagery and is intentionally **not
committed** (same no-game-assets policy as the ROM); to reproduce, seed from
your own capture of an in-game Clancer.

`title-logo.png` uses the Luckiest Guy display font from Google Fonts, layered
into original Trouble Makers lettering for this launcher. The bundled font is
licensed under Apache License 2.0; see `fonts/LICENSE-LuckiestGuy.txt`.

- Font source: https://github.com/google/fonts/tree/main/apache/luckiestguy
- Specimen: https://fonts.google.com/specimen/Luckiest+Guy

## Replacement artwork spec

- Canvas: 2560 x 1600 pixels (16:10, twice the Steam Deck's native size)
- Format: sRGB JPEG, ideally below 2 MB
- Bleed: keep important details inside the center 2304 x 1440 area so 16:9
  displays can crop safely
- Composition: reserve the rightmost 40% for the menu and avoid putting faces
  or essential details beneath it; the launcher adds its own dark gradient
- Logo-safe area: the generated title occupies roughly the upper-left quarter
  and can be removed later if a transparent custom logo is supplied
