# Known Issues

## Remaining widescreen boss backgrounds

CERBERUS (3-12) has a brighter 4:3 center against its expanded background.
PHOENIX has separated background strips and black gaps outside the native
view. Original 4:3 mode avoids these expansion defects. These remain open
after the [whole-game background audit](docs/widescreen-background-audit.md).

## Animated sprite artifacts with frame interpolation

At frame rates above the game's native 60 FPS, some animated characters can
look blurred or appear to have body parts offset from one another. These
characters are assembled from multiple independently rendered pieces, which
RT64 may not match consistently between two animation frames.

Workaround: select **Native 60 FPS** in the launcher. SSAA can still be used at
the native frame rate.

## Seams in multipart sprites and terrain strips

Some characters have visible joins between their component sprites, and some
terrain artwork has several horizontal strip boundaries. MSAA and SSAA can
smooth the edge of each individual piece, but they do not align independently
rasterized pieces or remove boundaries already present in layered artwork.

Higher resolutions and supersampling can make these joins easier to notice.
Fixing them safely will require targeted pixel-grid alignment or sprite/terrain
composition changes; no global overlap or texture expansion is currently
applied because it could introduce texture bleeding in other scenes.
