// src/theme/Theme.cpp
// ---------------------------------------------------------------------------
// batbox::theme — Theme struct implementation.
//
// theme_from_name() and load_theme() are defined in themes.cpp (same library).
// This file exists as the designated landing point for any future Theme-level
// utilities that do not belong with the palette definitions — e.g. JSON
// serialisation helpers, theme diff/comparison, or dynamic field mutation.
//
// Currently empty of runtime logic; the real work is in themes.cpp.
// ---------------------------------------------------------------------------
#include <batbox/theme/Theme.hpp>

// theme_from_name() and load_theme() are defined in themes.cpp.
// No additional symbols need to be defined here at this stage.
