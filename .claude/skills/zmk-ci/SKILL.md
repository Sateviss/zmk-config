---
name: zmk-ci
description: Build the keyboard firmware on GitHub Actions and check the result cheaply - push, wait in the background, read only failing-step error lines. Use for every "does it build / build it / produce firmware" request (CI is the default build path).
---

# CI build (default build path)

Workflow: `.github/workflows/build.yml` -> ZMK's reusable `build-user-config.yml`
(pinned commit), one job per `build.yaml` entry. Triggers: push, pull_request,
workflow_dispatch. The user downloads the `firmware` artifact from the run page
themselves: give them the run URL, never download artifacts unless asked.

## Steps

1. Commit and `git push -u origin <branch>` (pushing triggers the run).
2. Wait without spending tokens - Bash with `run_in_background: true`:
   `.claude/skills/zmk-ci/scripts/ci.sh wait` (ref defaults to HEAD).
   Do other work or end the turn; you are notified when it exits. Then read only
   the output file it names (a few lines). Never use `gh run watch` (redraws, huge output).
3. Exit 0 -> one `SUCCESS run <id> ... <url>` line: report the URL, done.
   Exit 1 -> the script already printed failed jobs/steps and up to 30 deduplicated
   error lines. Usually enough to fix. Only if not:
   - `grep -n -B5 -A5 '<pattern>' ~/zmk-ws/ci-logs/run-<id>-failed.log | head -60`
   - never `cat` / `Read` the whole log (thousands of lines).
   Exit 2 -> no run found / timeout / gh error (see below).
4. Rebuild without a new commit: `ci.sh dispatch [branch]` (background, as above).

## Looking things up in a run without reading the log

Each job also has "Kconfig file" (full sorted `.config`) and "Devicetree file" steps
that dump thousands of lines - grep them, never read them:
`ci.sh grep <run-id> 'CONFIG_ZMK_SLEEP|CONFIG_ZMK_STUDIO=' left` (last arg = job-name substring).

## Narrowing a failure down

CI failure you cannot explain from the error lines, or you need fast edit/compile
loops on one target -> switch to the `zmk-build` skill (local build, ~8 s incremental)
and come back to CI for the final firmware.

## gh problems

- `HTTP 403 ... not enabled for this session` / invalid GH_TOKEN: call the
  `add_repo` tool (owner `Sateviss`, repo `zmk-config`, access `push`). If that is
  refused, tell the user: it must be allowed (permission rule for
  `mcp__claude-code-remote__add_repo`) for CI checks to work. Do not work around it.
- Upstream workflow/West pin: `.github/workflows/build.yml` `@<sha>` and
  `config/west.yml` `revision:` must stay the same commit when bumping ZMK.

## Cheap delegation

Triage of a big, unclear failure: `ci-triage` agent (haiku) with the run id; it returns
<=15 lines. Not worth it for the normal case - the script output is already small.
