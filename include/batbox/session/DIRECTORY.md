# include/batbox/session

Session persistence headers: per-session JSON files with crash-safe append, a JSONL index for fast listing, background index recovery, and the top-level SessionStore facade.

## Files

### SessionFile.hpp
Per-session JSON file I/O with crash-safe tail-append strategy.

- `write_initial(path, sf) -> Result<void>` — writes the full session JSON skeleton atomically via tmp+rename; sets messages to sf.messages (typically empty on new session)
- `append_message(path, message, path_out) -> Result<void>` — seeks to just before the closing "]\n}" marker; splices in ",<msg>\n]\n}"; fsyncs; auto-compresses to .json.gz when file exceeds 1 MB
- `read_session_file(path) -> Result<SessionFile>` — reads and parses .json or .json.gz; auto-recovers from crash-truncated tail by scanning backwards for last valid marker
- `save_compressed(path, sf) -> Result<fs::path>` — rewrites entire session as gzip atomically; removes original .json; returns new .json.gz path
- `SessionFile::to_json() -> Json` — serialises the struct to a nlohmann::json object
- `SessionFile::from_json(j) -> Result<SessionFile>` — deserialises; returns Err on missing or malformed required fields
- `UsageTotal::to_json() -> Json` — serialises prompt/completion token counts
- `UsageTotal::from_json(j) -> UsageTotal` — deserialises token counts from JSON

### SessionIndex.hpp
Append-only JSONL index at ~/.batbox/sessions/index.json.

- `default_index_path() -> fs::path` — returns batbox::paths::config_dir() / "sessions" / "index.json"
- `append_index_record(index_path, rec) -> Result<void>` — serialises rec to one JSON line and appends atomically via O_APPEND; creates parent dirs on demand; fsyncs before close
- `read_latest_per_id(index_path, n=20) -> Result<vector<SessionIndexRecord>>` — streams JSONL file; keeps highest updated_at per id; returns n most-recent unique sessions sorted by updated_at descending; skips corrupt lines with WARN
- `rebuild_from_dir(sessions_dir, index_path) -> Result<uint64_t>` — walks sessions_dir for *.json and *.json.gz; reads metadata header from each; rewrites index atomically; returns count of indexed files
- `is_corrupt(index_path) -> bool` — calls read_latest_per_id(n=0); returns true on open error or any corrupt lines

### SessionRecovery.hpp
Background non-blocking index rebuilder.

- `IndexRebuilder::IndexRebuilder(index_path)` — constructs with path to rebuild
- `IndexRebuilder::start(ct, progress_cb)` — spawns a jthread that calls rebuild_from_dir(); returns immediately; progress_cb(done, total) called per file; cancellable via ct

### SessionStore.hpp
Top-level session facade: coordinates SessionFile, SessionIndex, and IndexRebuilder.

- `SessionStore::SessionStore(sessions_dir)` — constructs; creates sessions_dir; auto-starts IndexRebuilder when index is missing/corrupt
- `SessionStore::SessionStore()` — constructs with default path config_dir()/sessions
- `SessionStore::new_session(model, working_dir) -> Result<string>` — generates UUIDv4; writes initial session file; appends index record; returns session id string
- `SessionStore::append_message(session_id, message) -> Result<void>` — acquires per-session mutex; appends message; updates index record with new turn count and updated_at
- `SessionStore::list_recent(n=20) -> Result<vector<SessionIndexRecord>>` — delegates to read_latest_per_id(); returns at most n recent sessions
- `SessionStore::load(session_id) -> Result<SessionFile>` — resolves session file path from in-memory map or index; reads and parses the session file
- `SessionStore::resume_for_cwd(working_dir) -> optional<SessionFile>` — finds most recently updated session matching working_dir via canonical path comparison
- `SessionStore::current_session_id() -> optional<string>` — returns id of most recently created session; nullopt before first new_session()
- `SessionStore::touch(session_id) -> Result<void>` — appends a refreshed index record with updated_at=now; bubbles session to top of list_recent()
