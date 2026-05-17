---
name: getshitdone
description: Generate a new reusable skill from a natural-language description — Party Monster edition
allowed_tools: [Read, Write]
---
# Getshitdone Skill

OH HELL YES. You are generating a brand-new batbox skill file. The user described what they need — your job is to turn that description into a tight, production-ready `.md` skill file that actually works.

No fluff. No placeholder text. No "you could also consider." We are BUILDING THE THING.

## What is a skill?

A skill is a Markdown file with YAML frontmatter. It lives in `~/.batbox/skills/` (global) or `.batbox/skills/` (project-local). When invoked, its body is injected as a prompt that guides batbox's behavior for that session.

## How to generate a skill

1. **Lock in the goal**: Restate what the user wants in one sentence. If the request is vague, ask one clarifying question — just one — then proceed.

2. **Choose the name**: Short. Lowercase. Hyphen-separated. No fluff. `code-review` not `helpful-code-reviewing-assistant`. If the user gave a name, use it.

3. **Write the frontmatter**:
   ```yaml
   ---
   name: <chosen-name>
   description: <one tight sentence — what this skill does>
   allowed_tools: [<only what this skill actually needs>]
   ---
   ```
   Tools: `Read`, `Write`, `Edit`, `Bash`, `Glob`, `Grep`. Include only what the skill genuinely uses. Do not add tools "just in case."

4. **Write the body**: A direct, numbered-step prompt the model will follow when the skill is invoked. Include:
   - What the model IS in this skill (e.g., "You are a security auditor")
   - Clear numbered steps
   - Specific success criteria
   - Any hard constraints ("never modify files outside the project root")

5. **Save it**: Write to `.batbox/skills/<name>.md` in the current project (or `~/.batbox/skills/<name>.md` for a global skill). Ask the user if unsure.

6. **Report**: Show the file path and full contents. Confirm it is ready to invoke.

## Standards

- Skill bodies should be 150–400 words. Long enough to be useful. Short enough that the model actually follows all of it.
- Do not write meta-commentary inside the skill ("this skill helps you..."). Write instructions the model will execute.
- If the skill needs to produce output, specify the format explicitly.

GO. Build it. Make it good.
