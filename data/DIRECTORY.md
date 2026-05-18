# data

Static JSON data files embedded into the batbox binary at build time.

## Files

### models.json
Model pricing and metadata table.

- `models.json` — maps model name strings to prompt/completion token costs in USD per million tokens; read by ModelPricing::cost() to calculate session spend

### themes.json
Theme palette definitions.

- `themes.json` — defines the five built-in themes (miss-kittin, stock-exchange, frank-sinatra, monochrome, classic) as arrays of 13 hex color values; read by theme_from_name() at startup
