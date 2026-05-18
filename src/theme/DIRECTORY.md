# src/theme

Theme loading implementation: palette definitions and runtime selection.

## Files

### Theme.cpp
`theme_from_name()` implementation: linear search over the five static Theme objects; returns miss-kittin on no match. `load_theme()` implementation: checks BATBOX_THEME env var first; falls back to settings.theme; calls theme_from_name() for both.

### themes.cpp
Static Theme object definitions for all five built-in palettes:

- `miss-kittin` — electroclash aesthetic: magenta/cyan on near-black
- `stock-exchange` — finance terminal: cyan/yellow on black
- `frank-sinatra` — smoky 1950s: sepia/cream on black
- `monochrome` — strict white-on-black with no accent colors
- `classic` — the original claude-code color set

### CMakeLists.txt
Build rules for the theme static library.
