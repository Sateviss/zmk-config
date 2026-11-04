# Per-key RGB lighting module (see Kconfig).
zephyr_include_directories(include)

# The behavior is built whenever the keymap uses &prgb, so a keymap that references
# it still links with CONFIG_ZMK_PRGB=n (the behavior then does nothing).
target_sources_ifdef(CONFIG_DT_HAS_ZMK_BEHAVIOR_PRGB_ENABLED app PRIVATE src/behaviors/behavior_prgb.c)
target_sources_ifdef(CONFIG_ZMK_PRGB app PRIVATE src/prgb.c)
