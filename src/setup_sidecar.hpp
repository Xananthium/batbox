// src/setup_sidecar.hpp
// =============================================================================
// batbox::cmd::setup_sidecar — entry point for the `batbox setup-sidecar`
// subcommand.
//
// Creates (or updates) the Python Scrapling sidecar virtual environment at
// ~/.batbox/sidecar/.venv and pip-installs the requirements listed in
// python-sidecar/requirements.txt.
//
// The full implementation lands in CPP 7.X (Sidecar Manager task).  This
// skeleton provides the declaration so main.cpp can compile against it now.
// =============================================================================

#pragma once

namespace batbox::cmd {

/// Run the setup-sidecar subcommand.
///
/// Returns 0 on success, 1 on failure (error already printed to stderr).
int setup_sidecar();

} // namespace batbox::cmd
