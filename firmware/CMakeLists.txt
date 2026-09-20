cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_DEFINITIONS "ZT_STORE_SELF_INSTALL=1" APPEND)
project(zombie_tag)

# Development fleet, 2026-09-19, orchestrator-owned, operator-directed.
#
# The badge installs its own storage on first boot: it verifies the entire 64 KiB tail is
# blank, initialises zt_nvs, and writes a header marked self-installed (distinct flags,
# zeroed UUID and baseline) so it can never be mistaken for a tool-verified installation.
# There is no separate install step and no operator prompt.
#
# What this gives up: a self-installed badge carries no proof that its storage was
# initialised against a verified backup, so it cannot be commissioned. What remains is
# what actually protects the hardware: every badge still has its own verified whole-flash
# archive, identity and security are rechecked on every connection, writes stay inside the
# application range, and the full 4 MiB readback is compared before any boot.
#
# Set to 0 for a participant rollout, where the guarded install-init writes a header whose
# baseline and UUID the tool can verify.
