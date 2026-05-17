// src/main.cpp
// =============================================================================
// batbox entry point.
//
// Responsibilities:
//   - Parse argv with CLI11.
//   - Populate AppArgs from parsed flags / subcommands.
//   - Delegate to batbox::App::run(args).
//   - Catch all unhandled exceptions and return exit code 2.
//
// Recognised flags (see pmdraft.md env-var schema + Decision of Record #6):
//   --print                  headless mode, no TUI
//   --print-format plain|json|markdown  output format for --print (CPP A.1)
//   --prompt "text"          prompt text for headless mode (also positional)
//   --resume <session-id>    resume a specific session by UUID
//   --resume-latest          resume the most-recent session
//   --model <name>           model override
//   --env <path>             alternative .env file path
//   --verbose                enable trace/debug logging
//   --nuclear                start in nuclear ☢️ permission mode (DoR #6)
//   --subagent               internal: launched as a sub-agent worker
//
// Subcommands:
//   setup-sidecar            install / update the Python Scrapling sidecar venv
//   migrate                  one-time migration from ~/.claude/* → ~/.batbox/*
//     --apply                execute copies (default is dry-run)
//     --force                overwrite existing destinations
//     --from-dir <path>      override source root (default: ~/.claude)
//     --to-dir <path>        override destination root (default: ~/.batbox)
// =============================================================================

#include "App.hpp"
#include "migrate.hpp"
#include "setup_sidecar.hpp"

#include <batbox/core/Logging.hpp>

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    batbox::AppArgs args;

    // -----------------------------------------------------------------------
    // CLI11 app definition
    // -----------------------------------------------------------------------
    CLI::App cli{"batbox — terminal AI assistant"};
    cli.set_version_flag("--version", "batbox 0.1.0");

    // -- Positional prompt (optional) ---------------------------------------
    // Stored directly in args.prompt.  The --prompt flag below shares the
    // same destination; CLI11 allows a positional and a named option to bind
    // the same variable as long as the long-option name differs from the
    // positional's metavar.  We use metavar "PROMPT" and option "--prompt".
    cli.add_option("PROMPT", args.prompt,
                   "initial prompt text (same as --prompt)")
       ->expected(0, 1);

    // -- Headless flags -----------------------------------------------------
    cli.add_flag("--print",
                 args.print_mode,
                 "headless mode: output to stdout, no TUI");

    // --print-format: controls how headless output is formatted (CPP A.1).
    // Accepts "plain" (default), "json", or "markdown".
    // Parsed into args.print_format enum.
    std::string print_format_str = "plain";
    cli.add_option("--print-format",
                   print_format_str,
                   "output format for --print: plain (default) | json | markdown")
       ->check(CLI::IsMember({"plain", "json", "markdown"}));

    // --prompt overwrites the positional when both are supplied (last wins).
    cli.add_option("--prompt",
                   args.prompt,
                   "prompt text for headless --print mode");

    // -- Session resumption -------------------------------------------------
    cli.add_option("--resume",
                   args.resume_id,
                   "resume session by UUID");

    cli.add_flag("--resume-latest",
                 args.resume_latest,
                 "resume the most recent session");

    // -- Model override -----------------------------------------------------
    cli.add_option("--model",
                   args.model,
                   "model name override (e.g. gpt-4o, ollama:llama3.1)");

    // -- Config override ----------------------------------------------------
    cli.add_option("--env",
                   args.env_file,
                   "path to alternative .env file");

    // -- Verbosity ----------------------------------------------------------
    cli.add_flag("--verbose",
                 args.verbose,
                 "enable debug-level logging");

    // -- Permission mode ----------------------------------------------------
    // Decision of Record #6: --nuclear is valid for both headless (--print)
    // and interactive TUI sessions.  The flag sets permission mode to Nuclear
    // at launch; in-session mode cycling (Shift+Tab) is wired in CPP 12.X.
    cli.add_flag("--nuclear",
                 args.nuclear,
                 "start in nuclear mode: auto-accept all permissions (\xe2\x98\xa2\xef\xb8\x8f)");

    // -- Subagent infrastructure --------------------------------------------
    // --subagent is consumed internally when batbox spawns itself as a
    // child worker process.  It is intentionally not documented in --help
    // to avoid confusing end users (group("") hides it from help output).
    cli.add_flag("--subagent",
                 args.subagent,
                 "internal: suppress splash, force headless for sub-agent use")
       ->group(""); // empty group hides from standard --help output

    // -----------------------------------------------------------------------
    // Subcommands
    // -----------------------------------------------------------------------

    // setup-sidecar — install/update Python Scrapling venv
    auto* sc = cli.add_subcommand(
        "setup-sidecar",
        "install (or update) the Python Scrapling sidecar venv under "
        "~/.batbox/sidecar/.venv");
    sc->callback([&args]() { args.run_setup_sidecar = true; });

    // migrate — one-time ~/.claude/* → ~/.batbox/* migration
    batbox::cmd::MigrateArgs migrate_args;

    auto* mc = cli.add_subcommand(
        "migrate",
        "copy ~/.claude/* settings to ~/.batbox/* (non-destructive by default)");
    mc->callback([&args]() { args.run_migrate = true; });

    mc->add_flag("--apply",
                 migrate_args.apply,
                 "execute the copies (default: dry-run, nothing is written)");

    mc->add_flag("--force",
                 migrate_args.force,
                 "overwrite existing destination files/directories");

    mc->add_option("--from-dir",
                   migrate_args.from_dir,
                   "source root directory (default: ~/.claude)");

    mc->add_option("--to-dir",
                   migrate_args.to_dir,
                   "destination root directory (default: ~/.batbox)");

    // -----------------------------------------------------------------------
    // Parse
    // -----------------------------------------------------------------------
    CLI11_PARSE(cli, argc, argv);

    // -----------------------------------------------------------------------
    // Post-parse: convert --print-format string → enum
    // -----------------------------------------------------------------------
    if (print_format_str == "json") {
        args.print_format = batbox::PrintFormat::Json;
    } else if (print_format_str == "markdown") {
        args.print_format = batbox::PrintFormat::Markdown;
    } else {
        args.print_format = batbox::PrintFormat::Plain;
    }

    // -----------------------------------------------------------------------
    // Subcommand dispatch — handled before App::run so the main application
    // object is never constructed for pure-utility subcommands.
    // -----------------------------------------------------------------------
    if (args.run_setup_sidecar) {
        return batbox::cmd::setup_sidecar();
    }
    if (args.run_migrate) {
        return batbox::cmd::run_migrate(migrate_args);
    }

    // -----------------------------------------------------------------------
    // Delegate to App
    // -----------------------------------------------------------------------
    try {
        return batbox::App::run(args);
    } catch (const std::exception& ex) {
        // Catch-all: log + stderr + exit 2 (distinct from CLI11 exit 1).
        std::cerr << "batbox: unhandled exception: " << ex.what() << '\n';
        try { BATBOX_LOG_CRITICAL("unhandled exception: {}", ex.what()); }
        catch (...) {}
        return 2;
    } catch (...) {
        std::cerr << "batbox: unknown exception\n";
        try { BATBOX_LOG_CRITICAL("unknown exception in App::run"); }
        catch (...) {}
        return 2;
    }
}
