// tests/fixtures/TestTheme.hpp
//
// Shared make_test_theme() fixture — PEXT3 3.1 DRY extraction.
//
// Previously each of the 12 test files below defined its own local
// make_test_theme() returning some variant of the miss-kittin palette.
// All copies converged on the same theme; the differences were cosmetic
// (hand-coded RGB vs theme_from_name, slightly different shade values,
// different namespace qualification).
//
// Design directives (Karla K3 / user brief):
//   - No class hierarchy — single inline free function in namespace.
//   - Header-only; inline so no ODR violation across TUs.
//   - No extra validation / Err guards.
//   - Pure C++; only dep is batbox/theme/Theme.hpp.
//
// Usage:
//   #include "fixtures/TestTheme.hpp"
//   // (requires ${PROJECT_SOURCE_DIR}/tests on the include path)
//   using batbox::test_fixtures::make_test_theme;
//
//   auto theme = make_test_theme();
//
// Migrated in PEXT3 3.1:
//   tests/unit/test_diff_card.cpp
//   tests/unit/test_streaming_message_view.cpp
//   tests/unit/test_syntax_highlight_new_langs.cpp
//   tests/unit/test_demon_panel.cpp
//   tests/unit/test_markdown_render.cpp
//   tests/unit/test_question_card.cpp
//   tests/unit/test_syntax_highlight.cpp
//   tests/integration/test_plan_approval_card.cpp
//   tests/integration/test_permission_card.cpp
//   tests/integration/test_nuclear_mode.cpp
//   tests/integration/test_tui_layout.cpp
//   tests/integration/test_subagent_panel.cpp

#pragma once

#include <batbox/theme/Theme.hpp>

namespace batbox::test_fixtures {

// Returns the canonical miss-kittin theme.
// All 12 migrated copies returned miss-kittin; theme_from_name is the
// authoritative definition so we delegate there instead of duplicating RGB.
inline batbox::theme::Theme make_test_theme() {
    return batbox::theme::theme_from_name("miss-kittin");
}

} // namespace batbox::test_fixtures
