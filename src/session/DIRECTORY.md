# src/session

Session persistence implementations: crash-safe JSON file I/O, JSONL index, background recovery, and the SessionStore facade.

## Files

### SessionFile.cpp
`write_initial()` implementation: renders full JSON skeleton; writes to temp file; renames atomically. `append_message()` implementation: opens in r+ mode; scans backwards for "]\n}" marker; splices new message; fsyncs; triggers save_compressed() when file exceeds 1 MB. `read_session_file()` implementation: auto-detects .json/.json.gz extension; calls zlib decompression for .gz; crash-recovery tail scan. `save_compressed()` implementation: compresses to zlib deflate stream; renames; unlinks original.

### SessionIndex.cpp
`append_index_record()` implementation: serialises SessionIndexRecord to compact JSON line; appends with O_APPEND|O_WRONLY|O_CREAT; fsyncs. `read_latest_per_id()` implementation: single buffered pass; keeps highest updated_at per id via unordered_map; stable sort descending; returns first n. `rebuild_from_dir()` implementation: walks directory; reads each session file header; writes fresh JSONL index atomically. `is_corrupt()` implementation: calls read_latest_per_id(n=0); checks corrupt_lines_skipped count.

### SessionRecovery.cpp
`IndexRebuilder::start()` implementation: launches std::jthread; optionally renames corrupt index to .bak; calls rebuild_from_dir(); calls progress_cb between files; checks stop_token between iterations.

### SessionStore.cpp
All SessionStore method implementations; per-session mutex map management; build_index_record() populates SessionIndexRecord from in-memory state; auto-starts IndexRebuilder on corrupt/missing index.

### State.cpp
`read_last_seen_changelog_version()` and `write_last_seen_changelog_version()` implementations; reads/writes ~/.batbox/state.json with the "last_seen_changelog_version" key.

### CMakeLists.txt
Build rules for the session static library.
