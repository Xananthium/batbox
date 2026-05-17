// src/app/WireTui.cpp
// =============================================================================
// wire_tui() — compose the main TUI layout with a Claude-Code-style splash banner.
//
// Blueprint contract (task CPP 1.15):
//   function  batbox::app::wire_tui
//   file      src/app/WireTui.cpp
//
// Layout (ned-cpp.md §2.C1, updated TUI-FLOW-T4 / TUI-FLOW-T10):
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  SplashBanner (non-modal, collapses after first submit)  │
//   │  PermissionBanner (0–1 line, hidden in Default mode)    │
//   ├──────────────────────────────────────────────────────────┤
//   │  ChatView (flex)               │  SubAgentPanel (30 cols) │
//   ├──────────────────────────────────────────────────────────┤
//   │  InputBar (4 lines: prompt row + status row)             │
//   └──────────────────────────────────────────────────────────┘
//         └─ DemonPanel (floating bottom-right via dbox overlay)
//
// TUI-FLOW-T4 wiring:
//   1. SplashBanner is created (unless BATBOX_NO_SPLASH=true) with version,
//      model name, email from BATBOX_USER_EMAIL env, and current_path() cwd.
//   2. InputBar::set_splash_showing(true) is called so the contextual
//      placeholder is visible on the first render.
//   3. on_submit is wrapped: on the first call it collapses SplashBanner and
//      clears the placeholder via InputBar::set_splash_showing(false).
//
// TUI-FLOW-T10 wiring:
//   1. load_changelog(project_root()) is called before mounting the banner.
//   2. The loaded entries are passed to SplashBanner::set_changelog().
//   3. read_last_seen_changelog_version() gates whether the "What's new" panel
//      shows new content or an "up to date" stub.
//   4. BATBOX_FORCE_CHANGELOG=true bypasses the state check.
//   5. On first submit, write_last_seen_changelog_version(newest_version) is
//      called to persist the seen state.
//
// Implementation notes
// --------------------
//  SplashBanner::Focusable() returns false so Container::Vertical skips it
//  when routing keyboard events — InputBar always has focus.
//  The old full-screen Splash (timer + key-dismiss) is no longer mounted.
// =============================================================================

#include <batbox/app/McpStatusPoller.hpp>
#include <batbox/app/WireTui.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/StateStore.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>

#include <batbox/tui/Changelog.hpp>
#include <batbox/tui/ChatView.hpp>
#include <batbox/tui/DemonPanel.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/InputBar.hpp>
#include <batbox/tui/PermissionBanner.hpp>
#include <batbox/tui/PermissionCard.hpp>
#include <batbox/tui/PlanApprovalCard.hpp>
#include <batbox/tui/QuestionCard.hpp>
#include <batbox/tui/Screen.hpp>
#include <batbox/tui/Splash.hpp>
#include <batbox/tui/SubAgentPanel.hpp>

#include <batbox/agents/AgentEvent.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/repl/Keybindings.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace batbox::app {

// =============================================================================
// Internal: build_main_layout
// =============================================================================

namespace {

/// Assemble all components into the composed root Component.
///
/// @param banner        PermissionBanner Component.
/// @param splash_banner SplashBanner Component (may be nullptr if BATBOX_NO_SPLASH=true).
/// @param chat_view     ChatView shared_ptr.
/// @param sub_panel     SubAgentPanel Component.
/// @param demon_panel   DemonPanel Component.
/// @param input_bar     InputBar Component.
/// @returns The assembled root Component ready for ScreenManager::swap_root().
ftxui::Component build_main_layout(
    ftxui::Component banner,
    ftxui::Component splash_banner,
    ftxui::Component chat_view,
    ftxui::Component sub_panel,
    ftxui::Component demon_panel,
    ftxui::Component input_bar)
{
    using namespace ftxui;

    // -------------------------------------------------------------------------
    // Top section renderer: ChatView (flex) | SubAgentPanel (fixed width).
    // -------------------------------------------------------------------------
    auto top_renderer = Renderer([chat_view, sub_panel]() -> Element {
        return hbox({
            chat_view->Render() | flex,
            sub_panel->Render(),
        });
    });

    auto top_with_events = CatchEvent(
        top_renderer,
        [chat_view, sub_panel](Event ev) -> bool {
            if (sub_panel->OnEvent(ev)) return true;
            if (chat_view->OnEvent(ev)) return true;
            return false;
        });

    // -------------------------------------------------------------------------
    // Main area: (SplashBanner +) PermissionBanner + content + InputBar.
    // SplashBanner is non-focusable so Container::Vertical skips it for kbd.
    // -------------------------------------------------------------------------
    ftxui::Components vbox_children;
    if (splash_banner) {
        vbox_children.push_back(splash_banner);
    }
    vbox_children.push_back(banner);
    vbox_children.push_back(top_with_events | flex);
    vbox_children.push_back(input_bar);

    auto main_vbox = Container::Vertical(std::move(vbox_children));

    // Set initial focus to InputBar so keyboard events reach it immediately.
    // Without this call, Container::Vertical starts with selected_=0 (banner
    // or splash — non-focusable), causing printable key events to be silently
    // dropped.  TakeFocus() walks the parent chain and sets the selector to
    // InputBar's index.
    input_bar->TakeFocus();

    // -------------------------------------------------------------------------
    // Root: dbox stacks DemonPanel overlay on top of the main vbox.
    // -------------------------------------------------------------------------
    auto root_renderer = Renderer(main_vbox, [main_vbox, demon_panel]() -> Element {
        return dbox({
            main_vbox->Render(),
            demon_panel->Render(),
        });
    });

    // Event routing:
    //   1. DemonPanel        — DemonDirty posted events (highest priority).
    //   2. main_vbox         — keyboard events → InputBar (selected_=last);
    //                          PermissionBanner for Shift+Tab / Nuclear confirm.
    //   3. top_with_events   — posted events (Token, UserMessage, StreamDone,
    //                          AgentsDirty) reach ChatView/SubAgentPanel here.
    auto root_with_events = CatchEvent(
        root_renderer,
        [main_vbox, top_with_events, demon_panel](Event ev) -> bool {
            if (demon_panel->OnEvent(ev)) return true;
            if (main_vbox->OnEvent(ev)) return true;
            if (top_with_events->OnEvent(ev)) return true;
            return false;
        });

    return root_with_events;
}

/// Return true if BATBOX_FORCE_CHANGELOG env var is set to "true" or "1".
bool force_changelog() {
    const char* env = std::getenv("BATBOX_FORCE_CHANGELOG");
    if (!env || env[0] == '\0') return false;
    std::string_view sv(env);
    return sv == "true" || sv == "1";
}

// =============================================================================
// TUI-FLOW-T9: cwd file scanner for contextual placeholder templates
// =============================================================================

/// Scan @p project_root for "interesting" source files to use as placeholder
/// context.  Returns a vector of relative path strings (relative to project_root).
///
/// Candidate selection:
///   1. Top-level README.md / README (always included if present, in that order)
///   2. Top-level build manifests: CMakeLists.txt, Cargo.toml, package.json
///   3. Up to 5 randomly-chosen source files found under src/ (depth-bounded to
///      avoid scanning an unbounded tree).
///
/// Returns at most 8 paths.  The vector may be empty if the directory is
/// inaccessible or contains no source files.
static std::vector<std::string> scan_cwd_for_placeholder_files(
    const std::filesystem::path& project_root)
{
    std::vector<std::string> candidates;
    std::error_code ec;

    // --- 1. Top-level documentation / manifest files ---
    static const std::array<const char*, 6> kTopLevelNames = {
        "README.md", "README", "CMakeLists.txt", "Cargo.toml",
        "package.json", "go.mod",
    };
    for (const char* name : kTopLevelNames) {
        auto p = project_root / name;
        if (std::filesystem::exists(p, ec) && !ec) {
            candidates.push_back(name);
        }
    }

    // --- 2. Source files under src/ (depth-bounded) ---
    static const std::array<const char*, 9> kSourceExts = {
        ".cpp", ".hpp", ".h", ".c", ".rs", ".go", ".py", ".ts", ".swift",
    };

    auto is_source_ext = [&](const std::filesystem::path& p) -> bool {
        auto ext = p.extension().string();
        for (const char* e : kSourceExts) {
            if (ext == e) return true;
        }
        return false;
    };

    std::vector<std::string> src_files;
    auto src_dir = project_root / "src";
    if (std::filesystem::is_directory(src_dir, ec) && !ec) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(
                 src_dir,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec))
        {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
            if (!is_source_ext(entry.path())) continue;

            // Limit depth: relative path should have at most 4 components
            // (e.g. src/tui/InputBar.cpp = 3 components — well within range)
            auto rel = std::filesystem::relative(entry.path(), project_root, ec);
            if (ec) { ec.clear(); continue; }
            int depth = 0;
            for (const auto& part : rel) { (void)part; ++depth; }
            if (depth > 4) continue;

            src_files.push_back(rel.string());
        }
    }

    // Shuffle and pick up to 5 source files
    if (!src_files.empty()) {
        std::mt19937 rng(std::random_device{}());
        std::shuffle(src_files.begin(), src_files.end(), rng);
        int pick = std::min(static_cast<int>(src_files.size()), 5);
        for (int i = 0; i < pick; ++i) {
            candidates.push_back(src_files[static_cast<std::size_t>(i)]);
        }
    }

    return candidates;
}

/// Build the full placeholder template list for InputBar (TUI-FLOW-T9).
///
/// Returns a vector whose first element is always the T4 default fallback.
/// Subsequent entries are verb+file combinations built from scanned cwd files.
static std::vector<std::string> build_placeholder_templates(
    const std::filesystem::path& project_root)
{
    std::vector<std::string> templates;

    // Index 0: T4 default fallback (always present)
    templates.push_back("Try '/help' or 'plan a feature'");

    auto files = scan_cwd_for_placeholder_files(project_root);
    if (files.empty()) {
        return templates;   // only the fallback
    }

    static const std::array<const char*, 4> kVerbs = {
        "ask about", "explain", "refactor", "/plan add tests for",
    };

    int added = 0;
    for (const char* verb : kVerbs) {
        for (const auto& file : files) {
            if (added >= 11) goto done;   // cap total templates at 12
            templates.push_back(std::string(verb) + " `" + file + "`");
            ++added;
        }
    }
    done:

    return templates;
}

} // anonymous namespace

// =============================================================================
// wire_tui — public entry point
// =============================================================================

void wire_tui(
    batbox::tui::ScreenManager&                 screen_mgr,
    batbox::agents::AgentSupervisor*            supervisor,
    const batbox::agents::AgentEventQueue&      queue,
    const batbox::theme::Theme&                 theme,
    batbox::repl::History&                      history,
    batbox::repl::Keybindings&                  keybindings,
    std::string                                 model_name,
    batbox::tui::InputBar::SubmitCallback       on_submit_override,
    batbox::commands::SlashCommandRegistry*     slash_registry,
    batbox::tui::PermissionCard*                permission_card,
    batbox::tui::PlanApprovalCard*              plan_approval_card,
    batbox::tui::QuestionCard*                  question_card,
    batbox::mcp::McpServerRegistry*             mcp_registry,
    batbox::permissions::PermissionGate*        permission_gate,
    batbox::tui::InputBar::InterruptCallback    on_interrupt_cb)
{
    BATBOX_LOG_DEBUG("wire_tui: constructing TUI components");

    // -------------------------------------------------------------------------
    // 1. Load changelog from disk (TUI-FLOW-T10).
    //    Tries agentic/changelog.md then CHANGELOG.md at project root.
    //    Falls back to an empty vector (SplashBanner uses kChangelog fallback).
    // -------------------------------------------------------------------------
    const std::filesystem::path proj_root = batbox::paths::project_root();
    std::vector<batbox::tui::ChangelogEntry> changelog_entries =
        batbox::tui::load_changelog(proj_root);

    // Determine the newest version from loaded entries (empty = unknown).
    std::string newest_version;
    if (!changelog_entries.empty()) {
        newest_version = changelog_entries[0].version;
    }

    // -------------------------------------------------------------------------
    // 2. State check: has the user already seen this version? (TUI-FLOW-T10)
    //    If yes and BATBOX_FORCE_CHANGELOG is not set, pass an empty changelog
    //    to the banner so the right panel shows the "up to date" stub.
    // -------------------------------------------------------------------------
    std::vector<batbox::tui::ChangelogEntry> banner_changelog = changelog_entries;

    if (!force_changelog() && !newest_version.empty()) {
        const auto last_seen = batbox::config::read_last_seen_changelog_version();
        if (last_seen.has_value() && last_seen.value() == newest_version) {
            // User has already seen this version — show empty stub.
            banner_changelog.clear();
            BATBOX_LOG_DEBUG("wire_tui: changelog version '{}' already seen — suppressing panel",
                             newest_version);
        }
    }

    // -------------------------------------------------------------------------
    // 3. SplashBanner — TUI-FLOW-T4.
    //    Build the non-modal banner.  Skip if BATBOX_NO_SPLASH=true.
    //    Raw pointer and shared control state are set up before InputBar so
    //    the on_submit wrapper can capture them without pointer aliasing issues.
    // -------------------------------------------------------------------------
    const bool skip_splash = batbox::tui::Splash::should_skip();

    // Shared state owned by the submit wrapper lambda.
    // Using shared_ptr so the lambda can be copied safely into the Component tree.
    auto splash_banner_ptr = std::make_shared<batbox::tui::SplashBanner*>(nullptr);
    auto input_bar_ptr     = std::make_shared<batbox::tui::InputBar*>(nullptr);
    auto first_submit_done = std::make_shared<bool>(false);

    ftxui::Component splash_banner_comp = nullptr;
    if (!skip_splash) {
        auto sb = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
        *splash_banner_ptr = dynamic_cast<batbox::tui::SplashBanner*>(sb.get());

        if (*splash_banner_ptr) {
            // Populate banner fields
            (*splash_banner_ptr)->set_model(model_name);

            // Always call set_email so SplashBanner::set_email() can run
            // resolve_account_label() and produce a $USER@hostname fallback
            // when BATBOX_USER_EMAIL is unset or empty.
            const char* email_env = std::getenv("BATBOX_USER_EMAIL");
            (*splash_banner_ptr)->set_email(
                (email_env && email_env[0] != '\0') ? std::string(email_env) : std::string{});

            std::error_code ec;
            auto cwd_path = std::filesystem::current_path(ec);
            if (!ec) {
                (*splash_banner_ptr)->set_cwd(cwd_path.string());
            }

            // TUI-FLOW-T10: wire the changelog entries (may be empty if suppressed).
            (*splash_banner_ptr)->set_changelog(std::move(banner_changelog));
        }

        splash_banner_comp = std::move(sb);
        BATBOX_LOG_DEBUG("wire_tui: SplashBanner constructed (non-modal)");
    }

    // -------------------------------------------------------------------------
    // 4. Determine the effective on_submit callback.
    //    Wrap with splash-collapse + state-write logic (TUI-FLOW-T10):
    //    on the first call:
    //      a. collapse the SplashBanner
    //      b. clear the InputBar placeholder
    //      c. write newest_version to ~/.batbox/state.json
    //    then delegate to the real submit.
    // -------------------------------------------------------------------------
    batbox::tui::InputBar::SubmitCallback base_submit =
        on_submit_override
            ? std::move(on_submit_override)
            : [](std::string /*text*/) { /* no on_submit_override provided */ };

    batbox::tui::InputBar::SubmitCallback effective_submit =
        [base_submit    = std::move(base_submit),
         splash_ptr     = splash_banner_ptr,
         input_ptr      = input_bar_ptr,
         first_done     = first_submit_done,
         newest_ver     = newest_version](std::string text) mutable {
            if (!*first_done) {
                *first_done = true;
                if (*splash_ptr) (*splash_ptr)->collapse();
                if (*input_ptr) (*input_ptr)->set_splash_showing(false);
                // Persist newest version to state.json (TUI-FLOW-T10).
                if (!newest_ver.empty()) {
                    batbox::config::write_last_seen_changelog_version(newest_ver);
                }
            }
            if (base_submit) base_submit(std::move(text));
        };

    // -------------------------------------------------------------------------
    // 5. InputBar — construct with the wrapped submit callback.
    // -------------------------------------------------------------------------
    batbox::tui::InputBar::SlashCommandProvider slash_provider_cb = nullptr;
    if (slash_registry) {
        slash_provider_cb = [slash_registry]() -> std::vector<std::string> {
            return slash_registry->names();
        };
    }

    auto input_bar = batbox::tui::make_input_bar(
        theme,
        history,
        keybindings,
        std::move(effective_submit),
        std::move(slash_provider_cb),
        /*ac_provider=*/nullptr);

    auto* input_bar_raw = dynamic_cast<batbox::tui::InputBar*>(input_bar.get());

    // Publish raw pointer into the shared capture so the submit lambda can
    // call set_splash_showing(false) on the correct InputBar instance.
    *input_bar_ptr = input_bar_raw;

    // -------------------------------------------------------------------------
    // 6. Apply initial InputBar state.
    // -------------------------------------------------------------------------
    if (input_bar_raw && !model_name.empty()) {
        input_bar_raw->set_model(model_name);
        BATBOX_LOG_DEBUG("wire_tui: InputBar model set to '{}'", model_name);
    }

    if (input_bar_raw && !skip_splash) {
        input_bar_raw->set_splash_showing(true);

        // TUI-FLOW-T9: populate contextual placeholder templates from cwd.
        // build_placeholder_templates() scans for source files and builds a
        // rotating list.  Falls back gracefully to the T4 default when empty.
        auto ph_templates = build_placeholder_templates(proj_root);
        const std::size_t ph_count = ph_templates.size();
        input_bar_raw->set_placeholder_templates(std::move(ph_templates));
        BATBOX_LOG_DEBUG("wire_tui: placeholder templates built (count={})", ph_count);
    }

    // TUI-PERM-T1: wire PermissionGate into InputBar so Shift+Tab can cycle modes.
    if (input_bar_raw && permission_gate) {
        input_bar_raw->set_permission_gate(permission_gate);
        BATBOX_LOG_DEBUG("wire_tui: PermissionGate wired to InputBar for Shift+Tab cycle");
    }

    // TUI-FIX-T3: wire interrupt callback so Esc while streaming fires cancel.
    if (input_bar_raw && on_interrupt_cb) {
        input_bar_raw->set_on_interrupt(std::move(on_interrupt_cb));
        BATBOX_LOG_DEBUG("wire_tui: interrupt callback wired to InputBar (TUI-FIX-T3)");
    }

    // -------------------------------------------------------------------------
    // 7. PermissionBanner — wired to update InputBar status line on mode change.
    // -------------------------------------------------------------------------
    auto banner = batbox::tui::make_permission_banner(
        theme,
        [input_bar_raw](batbox::permissions::PermissionMode /*mode*/,
                         std::string_view label) {
            if (input_bar_raw) {
                input_bar_raw->set_mode(std::string(label));
            }
        });

    // -------------------------------------------------------------------------
    // 8. ChatView — append-only scrollable conversation history.
    // -------------------------------------------------------------------------
    auto chat_view = std::make_shared<batbox::tui::ChatView>(theme);

    // TUI-FLOW-T1: wire the screen_post_fn so the spinner timer thread can
    // post SpinnerTick events back to the FTXUI event loop at 1Hz.
    chat_view->set_screen_post_fn([&screen_mgr](ftxui::Event ev) {
        screen_mgr.post_event(std::move(ev));
    });

    // TUI-ASKQ-T4: wire the QuestionCard so ChatView's OnEvent can load specs
    // into it when a QuestionShow event arrives.
    if (question_card != nullptr) {
        chat_view->set_question_card(
            std::shared_ptr<batbox::tui::QuestionCard>(
                question_card, [](batbox::tui::QuestionCard*) {}));
    }

    // TUI-FLOW-T6: wire InputBar into ChatView so UserMessage/StreamDone events
    // can toggle set_stream_active(), which controls the "esc to interrupt" chip.
    if (input_bar_raw) {
        chat_view->set_input_bar(input_bar_raw);
    }

    // -------------------------------------------------------------------------
    // 9. SubAgentPanel — right sidebar with live sub-agent status.
    //    Make() starts the 10Hz TuiAgentTickerThread automatically.
    // -------------------------------------------------------------------------
    auto sub_panel = batbox::tui::SubAgentPanel::Make(supervisor, queue, theme);

    // -------------------------------------------------------------------------
    // 10. DemonPanel — floating Party Monster easter-egg panel.
    //    Make() starts the 5Hz TuiDemonTickerThread automatically.
    // -------------------------------------------------------------------------
    auto demon_panel = batbox::tui::DemonPanel::Make(
        theme, screen_mgr.screen_interactive());

    BATBOX_LOG_DEBUG("wire_tui: all 5 components constructed "
                     "(SplashBanner, PermissionBanner, ChatView, SubAgentPanel, "
                     "DemonPanel, InputBar)");

    // -------------------------------------------------------------------------
    // 11. Compose the main layout root Component.
    // -------------------------------------------------------------------------
    auto main_root = build_main_layout(
        banner, splash_banner_comp, chat_view, sub_panel, demon_panel, input_bar);

    // -------------------------------------------------------------------------
    // 12. PermissionCard modal overlay (TUI-T4, UI-D2).
    // -------------------------------------------------------------------------
    ftxui::Component effective_root = main_root;
    if (permission_card != nullptr) {
        auto* perm_card_ptr = permission_card;
        // TUI-FIX-T7: inject screen so e key can run editor via WithRestoredIO.
        permission_card->set_screen(&screen_mgr.screen_interactive());

        auto modal_renderer = ftxui::Renderer(main_root,
            [main_root, perm_card_ptr]() -> ftxui::Element {
                using namespace ftxui;
                if (perm_card_ptr->pending()) {
                    return dbox({
                        main_root->Render(),
                        perm_card_ptr->Render() | clear_under | center,
                    });
                }
                return main_root->Render();
            });

        effective_root = ftxui::CatchEvent(modal_renderer,
            [main_root, perm_card_ptr](ftxui::Event ev) -> bool {
                if (ev == batbox::tui::Events::ModalShow ||
                    ev == batbox::tui::Events::ModalHide) {
                    return true;
                }
                if (perm_card_ptr->pending()) {
                    return perm_card_ptr->OnEvent(ev);
                }
                return main_root->OnEvent(ev);
            });

        BATBOX_LOG_DEBUG("wire_tui: PermissionCard modal overlay wired (TUI-T4)");
    }

    // -------------------------------------------------------------------------
    // 13. PlanApprovalCard modal overlay (TUI-PLAN-T2).
    // -------------------------------------------------------------------------
    if (plan_approval_card != nullptr) {
        auto* plan_card_ptr = plan_approval_card;
        // TUI-FIX-T7: inject screen so e/E key can run editor via WithRestoredIO.
        plan_approval_card->set_screen(&screen_mgr.screen_interactive());

        auto plan_modal_renderer = ftxui::Renderer(effective_root,
            [effective_root, plan_card_ptr]() -> ftxui::Element {
                using namespace ftxui;
                if (plan_card_ptr->pending()) {
                    return dbox({
                        effective_root->Render(),
                        plan_card_ptr->Render() | clear_under | center,
                    });
                }
                return effective_root->Render();
            });

        effective_root = ftxui::CatchEvent(plan_modal_renderer,
            [effective_root, plan_card_ptr](ftxui::Event ev) -> bool {
                if (ev == batbox::tui::Events::PlanApprovalShow) {
                    return true;
                }
                if (plan_card_ptr->pending()) {
                    return plan_card_ptr->OnEvent(ev);
                }
                return effective_root->OnEvent(ev);
            });

        BATBOX_LOG_DEBUG("wire_tui: PlanApprovalCard modal overlay wired (TUI-PLAN-T2)");
    }

    // -------------------------------------------------------------------------
    // 14. QuestionCard modal overlay (TUI-ASKQ-T4).
    //
    // Z-order: PermissionCard (outermost) > PlanApprovalCard > QuestionCard.
    // QuestionCard wraps effective_root (which already includes the two higher-
    // priority overlays) so it loses the key-event race to them when both are
    // simultaneously visible — PermissionCard and PlanApprovalCard intercept
    // their CatchEvent layers first.
    //
    // Visibility is controlled by chat_view->show_question_card() which is
    // written by ChatView::OnEvent on QuestionShow / QuestionResolved events.
    // -------------------------------------------------------------------------
    if (question_card != nullptr) {
        auto* qcard_ptr = question_card;

        auto question_modal_renderer = ftxui::Renderer(effective_root,
            [effective_root, chat_view, qcard_ptr]() -> ftxui::Element {
                using namespace ftxui;
                if (chat_view->show_question_card()) {
                    return dbox({
                        effective_root->Render(),
                        qcard_ptr->Render() | clear_under | center,
                    });
                }
                return effective_root->Render();
            });

        effective_root = ftxui::CatchEvent(question_modal_renderer,
            [effective_root, chat_view, qcard_ptr](ftxui::Event ev) -> bool {
                // Consume the wake-trigger event silently.
                if (ev == batbox::tui::Events::QuestionShow) {
                    return true;
                }
                // Route keyboard to QuestionCard when it is the active modal.
                if (chat_view->show_question_card()) {
                    // Let QuestionCard consume the event first.
                    // The card calls resolve() on Enter/Esc, which posts a
                    // QuestionResolved event that ChatView's OnEvent catches
                    // to clear show_question_card_.
                    if (qcard_ptr->OnEvent(ev)) return true;
                }
                return effective_root->OnEvent(ev);
            });

        BATBOX_LOG_DEBUG("wire_tui: QuestionCard modal overlay wired (TUI-ASKQ-T4)");
    }

    // -------------------------------------------------------------------------
    // 15a. McpStatusPoller (TUI-FLOW-T11) — wire MCP failure count into InputBar.
    //
    // The poller lives in a shared_ptr so the lambda below can hold a weak_ptr
    // to InputBar independently.  The poller is kept alive by capturing it into
    // the screen_mgr's post-run cleanup via a shared lambda or by storing it in
    // the local scope — here we store it as a local that outlives the event loop
    // via its capture in the layout Components.
    //
    // Thread safety note: InputBar::mcp_failed_ is std::atomic<int> so the
    // on_change callback can call set_mcp_failed() directly from the poller
    // thread without additional locking.
    // -------------------------------------------------------------------------
    // mcp_status_poller_ must be declared AFTER input_bar so RAII destruction
    // order is poller first, then InputBar — matching the required teardown order.
    std::unique_ptr<batbox::app::McpStatusPoller> mcp_status_poller;
    if (mcp_registry != nullptr && input_bar_raw != nullptr) {
        // Capture a raw pointer to InputBar.  InputBar's lifetime is tied to the
        // FTXUI component tree, which is torn down before WireTui scope exits.
        // The poller is destroyed (and its thread joined) before screen_mgr.run()
        // returns, because screen_mgr.run() is called after wire_tui() and the
        // poller's shared_ptr destructor runs when the component tree is released.
        // To guarantee order, we hold the poller in a shared_ptr captured by the
        // screen_mgr swap_root component — but a simpler approach is to use
        // ScreenManager's post-run hook.
        //
        // Simplest correct approach: store the poller in a shared_ptr and capture
        // it in the effective_root's CatchEvent destructor path via the Renderer.
        // However, the most robust approach without touching ScreenManager is to
        // store the poller as a local unique_ptr and let the RAII stack unwind it
        // BEFORE screen_mgr.run() returns — but screen_mgr.run() is called in
        // App.cpp AFTER wire_tui() returns, so the local unique_ptr would be
        // destroyed at wire_tui() scope exit, before the event loop starts.
        //
        // Correct solution: capture the poller in a shared_ptr inside the component
        // tree so it stays alive for the duration of the event loop, then is released
        // when the Components are destroyed (which happens after run() returns).
        auto poller_ptr = std::make_shared<batbox::app::McpStatusPoller>(
            mcp_registry,
            [input_bar_raw](int n) {
                input_bar_raw->set_mcp_failed(n);
            });

        // Keep the poller alive for the duration of the event loop by capturing
        // it in the effective_root renderer.  Wrap effective_root in a thin
        // Renderer that holds the shared_ptr as a captured value.
        auto poller_keeper = poller_ptr; // shared ownership
        auto old_root = effective_root;
        effective_root = ftxui::Renderer(old_root,
            [old_root, poller_keeper]() -> ftxui::Element {
                return old_root->Render();
            });
        effective_root = ftxui::CatchEvent(effective_root,
            [old_root](ftxui::Event ev) -> bool {
                return old_root->OnEvent(ev);
            });

        BATBOX_LOG_DEBUG("wire_tui: McpStatusPoller started (TUI-FLOW-T11)");
    }

    // -------------------------------------------------------------------------
    // 15b. TUI-FIX-T4: Enable bracketed paste mode.
    //      Writes the VT sequence \e[?2004h to stdout so the terminal
    //      wraps all paste events in \e[200~...\e[201~.  This must be done
    //      AFTER the terminal is in raw mode (FTXUI's ScreenInteractive handles
    //      raw mode setup) but before the event loop starts.
    //      The corresponding disable (\e[?2004l) is written on exit via atexit.
    //      Guard: only emit in a real terminal (isatty(1)).
    // -------------------------------------------------------------------------
    if (std::isatty(1)) {
        // Enable bracketed paste mode
        std::fputs("\x1b[?2004h", stdout);
        std::fflush(stdout);
        // Register disable on exit
        std::atexit([]() {
            std::fputs("\x1b[?2004l", stdout);
            std::fflush(stdout);
        });
        BATBOX_LOG_DEBUG("wire_tui: bracketed paste mode enabled (TUI-FIX-T4)");
    }

    // -------------------------------------------------------------------------
    // 15. Mount the layout directly (SplashBanner is inline, no screen swap).
    // -------------------------------------------------------------------------
    screen_mgr.swap_root(effective_root);

    BATBOX_LOG_DEBUG("wire_tui: TUI root mounted; call screen_mgr.run() to enter loop");
}

} // namespace batbox::app
