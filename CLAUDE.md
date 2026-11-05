# zmk-config (splitkb Aurora Lily58, nice!nano, ZMK)

- `config/splitkb_aurora_lily58.keymap` keymap (ASCII-art comment above each layer),
  `config/*.conf` Kconfig (`_left`/`_right` files are per-half),
  `config/west.yml` ZMK pin, `build.yaml` CI matrix (left has ZMK Studio),
  `legend.html` printable key legend (keep in sync with the keymap).
- The repo is also a Zephyr module (`zephyr/module.yml`, `src/` per-key RGB + OLED
  status screen, `dts/`, `Kconfig`), with host checks in `scripts/check_*.py` and an
  OLED preview renderer in `scripts/oled_preview/`.
- The user flashes the `firmware` artifact from the GitHub Actions run page.

## Token rules

- Build = CI by default: skill `zmk-ci` (push, `ci.sh wait` in background, read only
  error lines). Local build only to narrow down a failure / iterate: skill `zmk-build`.
- Never print whole build/CI logs, `.config`, `zephyr.dts`, `wrist.svg` or
  `legend.html`: grep with narrow patterns and `head`.
- Before spawning subagents read skill `zmk-delegate`; cheap agents in `.claude/agents/`
  (`ci-triage`, `zmk-lookup`, `host-checks`: haiku; `keymap-editor`: sonnet).
- Bumping ZMK: `.github/workflows/build.yml @sha` and `config/west.yml revision` must match.
