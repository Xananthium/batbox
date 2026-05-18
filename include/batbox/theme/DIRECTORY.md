# include/batbox/theme

Theme types and loaders for the FTXUI colour system.

## Files

### Theme.hpp
Color-role struct carrying 13 ftxui::Color values and named palette loaders.

- `theme_from_name(name) -> Theme` — returns the named theme (miss-kittin, stock-exchange, frank-sinatra, monochrome, classic); returns miss-kittin for unknown or empty name
- `load_theme(settings) -> Theme` — resolves active theme: BATBOX_THEME env var > settings.theme > miss-kittin default; unknown names at any level fall back to miss-kittin
