# Launcher artwork

`splash-background.jpg` is the active launcher artwork. It was generated
locally with ComfyUI 0.15.0 and the Z-Image Turbo model, then resized to the
launcher's 2560 x 1600 production canvas. It uses the game's signature Clancer
block face — two oval eyes and a single open oval "oh" mouth (not a smile) —
on a beveled cobalt cube, with the right ~55% kept dark for the menu. Its
reproducible workflow (prompt + seed) is in `comfy-generic-workflow.json`.

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
