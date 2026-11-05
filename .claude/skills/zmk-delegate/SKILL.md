---
name: zmk-delegate
description: Which work in this ZMK firmware repo to hand to subagents and which model tier (haiku / sonnet / main model) to use, with ready-made agents and a briefing template. Read before spawning any subagent here.
---

# Delegating to subagents

A subagent pays off when the work produces lots of output you would otherwise read
(logs, source trees, docs) but the answer is short. It does not pay off when the
brief is as long as the answer, or a script already prints 1-3 lines (`build.sh`,
`ci.sh wait`): run those yourself.

## Ready-made agents (`.claude/agents/`)

| agent | model | give it | returns |
|---|---|---|---|
| `ci-triage` | haiku | run id (+ branch) | <=15 lines: target, file:line, error, likely cause |
| `zmk-lookup` | haiku | a ZMK/Zephyr question (Kconfig symbol, behavior binding, DT property, default value, which file implements X) | <=20 lines with file:line refs from local ZMK/Zephyr sources and docs |
| `host-checks` | haiku | branch + which checks (`scripts/check_*.py`, OLED preview) | PASS/FAIL per check + only the failing/diff lines |
| `keymap-editor` | sonnet | the binding change in plain words | applied edit (keymap + ASCII comment + `legend.html`), local build result, 5-line summary |

For anything else use the Agent tool with a `model` override, e.g. `general-purpose`
with `model: "haiku"`.

## Tiers

**Haiku** - mechanical, verifiable, answer fits in a few lines:
- CI failure triage; grepping a run's Kconfig/Devicetree dump for a value
- looking up ZMK/Zephyr Kconfig options, behaviors, bindings, defaults, docs pages
- running host check scripts / OLED preview and reporting pass/fail
- comparing `.config` between two builds or branches (`diff <(grep ^CONFIG a) <(...)`)
- listing what changed between branches (`git diff --stat`, commit subjects)
- finding every place a symbol/keycode/layer index is used

**Sonnet** - bounded edits or research needing some judgment:
- keymap / combo / layer edits that must keep the ASCII art and `legend.html` in sync
- writing or extending a host-side check script for an existing C function
- researching breaking changes for a ZMK pin bump (upstream changelog / blog / PRs)
- reviewing a diff for DT/Kconfig mistakes, split central vs peripheral config issues
- resolving straightforward merge conflicts between `claude/*` feature branches

**Main model (keep it)** - design and anything needing conversation context:
- firmware module design (`src/prgb*.c/h`, OLED status screen), threading, stack
  sizes, work-queue priorities, power/sleep behaviour
- interpreting the user's hardware reports ("encoder skips", "right half drops")
- deciding what to change after a subagent's findings

## Parallelism

- Independent lookups (e.g. three Kconfig questions, two branches to compare) -> spawn
  in one message, run in background.
- Do NOT parallelise local builds across agents: one west workspace, 4 cores, and
  builds share `~/zmk-ws`. Do NOT spawn an agent just to wait for CI: `ci.sh wait` in
  a background Bash costs nothing until it exits.

## Briefing template (keep briefs short; the agent starts cold)

    Repo /home/user/zmk-config, branch <b>. Task: <one sentence>.
    Context: <only what it can't find itself, e.g. run id, symbol name>.
    Don't: edit files / push / run setup (unless the task is that).
    Return: <=N lines, file:line refs, no code dumps, say "unknown" rather than guess.
