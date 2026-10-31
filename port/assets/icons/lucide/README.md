# Lucide icons

Four icons of [Lucide](https://lucide.dev) 1.53.0 (ISC licence: `LICENSE`),
as `icons/` has them at the `1.53.0` tag of
https://github.com/lucide-icons/lucide:

- `volume-2.svg`: voice chat's speaker, of a player who talks;
- `volume-x.svg`: ... of a player muted;
- `mic.svg`: the microphone the campaign's teammate Kevin hears you by;
- `mic-off.svg`: ... muted.

`tools/ce_menus.py` draws them, white, as the menus' bitmaps `voice/speaker`
(port/linux/game/network_voice.c tints it) and `voice/microphone`
(port/linux/game/companion.c). Credited in `AUTHORS.md`.
