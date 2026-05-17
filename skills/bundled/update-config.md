---
name: update-config
description: Read, validate, and safely modify ~/.batbox/settings.json or a project-level config without clobbering existing keys
allowed_tools: [Read, Write, Edit]
---
# Update Config Skill

You are making a targeted, safe modification to a batbox configuration file. The cardinal rule is: touch only what was asked. Every key that exists before this operation must exist after it, with its original value, unless the user explicitly asked to change or remove it.

## Config file locations

| Config | Path | Purpose |
|--------|------|---------|
| User settings | `~/.batbox/settings.json` | Global BATBOX_* configuration (model, theme, token limits, etc.) |
| Project settings | `.batbox/settings.json` | Project-local overrides; takes precedence over user settings |
| Agents | `~/.batbox/agents.json` | Named agent configurations |
| MCP servers | `~/.batbox/mcp.json` | Model Context Protocol server definitions |
| Keybindings | `~/.batbox/keybindings.json` | Custom keyboard shortcut overrides |

If the user's request does not specify which file, use these defaults:
- Changes to model, theme, token limits, or tool permissions → `~/.batbox/settings.json`
- Changes that should only apply in the current project → `.batbox/settings.json`

## 1. Identify the target file

Determine which config file applies based on the user's request and the table above. If ambiguous, ask before reading — writing to the wrong file is harder to undo than asking one clarifying question.

## 2. Read the current config

Read the entire file before making any change. You need to:

1. Confirm the file exists. If it does not exist, note that you will create it with only the requested key(s) and a minimal valid JSON structure — do not fabricate defaults for other keys.
2. Parse and understand the current shape of the JSON. Note every top-level key and its current value.
3. Identify whether the key the user wants to change already exists. If it does, record its current value — you will report this as the "before" value in the confirmation.

## 3. Validate the change

Before writing anything, confirm:

- **Type correctness**: The new value is the right JSON type for this key. A key that expects a number should not receive a string; a key that expects an array should not receive a scalar.
- **Allowed values**: If the key is an enum (e.g., `theme: "dark" | "light" | "system"`), confirm the requested value is a valid member.
- **Schema constraints**: If the config has documented min/max values or format requirements (e.g., `max_tokens` must be a positive integer), confirm the new value satisfies them.

If validation fails, report the issue clearly and do not write. Suggest the correct value or format.

## 4. Apply the minimum diff

Make the smallest possible edit:

- Change only the key(s) the user asked to change.
- Do not reformat the entire file — preserve the existing indentation, key ordering, and whitespace style.
- Do not add keys the user did not ask to add.
- Do not remove keys the user did not ask to remove.
- Preserve comments if the format supports them (JSONC). Standard JSON does not support comments — do not add any.

If the file does not exist, create it with a minimal valid JSON object containing only the requested key(s):

```json
{
  "requested_key": "requested_value"
}
```

## 5. Confirm the change

After writing, report exactly what changed in a structured format:

```
Config updated
--------------
File    : ~/.batbox/settings.json
Key     : <key_name>
Before  : <old_value>  (or "key did not exist")
After   : <new_value>

All other keys preserved: yes
```

If multiple keys were changed, list each one. If the file was created from scratch, say so.

## 6. Edge cases

- **Nested keys**: If the key is nested (e.g., `display.theme`), read the parent object and merge the change in — do not replace the parent object entirely.
- **Array values**: If the user wants to add an item to an array, append it — do not replace the array. If they want to remove an item, filter it out and confirm the array length changed as expected.
- **Concurrent writes**: batbox may read the config during a session. If the user is running a live session, warn them that the change will take effect on the next session start (or immediately if hot-reload is supported for this key).
- **Backup on risky changes**: If the change modifies more than three keys or restructures a complex object, offer to show the full diff before applying.
