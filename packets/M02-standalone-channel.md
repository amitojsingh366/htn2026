# Packet M02 — stand alone on a fixed channel, with no access point

## Goal

Let a host badge operate with **no Wi-Fi access point at all**: park on its configured
channel, never scan, never associate, and beacon so players converge on it.

## Why, from hardware

The proof-of-concept has no backend and no reliable hotspot, and the current design cannot
work without one:

- `channel.c` gives a host the `ZT_CHANNEL_LOBBY` state only when
  `host_credentials_present` with a valid SSID and an 8..63 character password. It then
  scans for that AP indefinitely when the AP is absent, and an active scan **hops
  channels**, so the host never stays anywhere.
- A player is in `ZT_CHANNEL_DISCOVERY` and sweeps looking for a host, by design.
- So with no reachable AP, the host sweeps while hunting and the player sweeps while
  searching, and two badges never share a channel. Observed on hardware: one badge cycling
  channels, another parked on channel 1, `DIRECT PEERS` 0 on both.
- Scanning is also the largest current draw on the badge and browns it out on AA cells,
  which has been reproduced repeatedly.

## Base commit and dependencies

- Base commit: `c077d031cde4c6363dcdc99514f1ba86746a658d` on `firmware/integration`.
- Branch: `firmware/M02-standalone-channel`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/M02`
- You wrote `zt_radio` (M01) and its follow-up. This continues that work.

## Owned file allowlist

```
firmware/components/zt_radio/channel.c
```

Only that file. No headers, no other component, no `app_main.c`, no documents, no tools.

## Required behavior

Add a standalone mode, selected by configuration rather than by a new API:

**When `is_host` is set and the configuration carries no host credentials**
(`host_credentials_present` is zero, or the SSID is empty), the badge must:

1. Enter `ZT_CHANNEL_LOBBY` and stay on `cfg->last_channel` permanently.
2. **Never** call `esp_wifi_scan_start`, `esp_wifi_connect`, or set an STA configuration.
   No scan, no association, no retry ladder, no DHCP.
3. Report status honestly: `associated` and `has_ip` are zero, and the channel is the
   configured one. Do not report a connection that does not exist.
4. Keep ESP-NOW fully functional — this mode exists precisely so the mesh works.

Currently that combination is rejected with `ZT_ERR_INVALID_ARG` during initialisation.
Accept it, and treat it as a deliberate configuration rather than an error.

**Everything else is unchanged.** A host WITH credentials behaves exactly as it does now:
it scans, associates, follows the AP's channel, and runs the reconnect ladder. A player
still discovers. The round channel lock, `zt_channel_lock`, `zt_channel_recover`, the
same-channel reconnection rules and the generation handling are all untouched.

## Excluded

No new API, header change, packet type or console operation. No change to discovery for
players, to the lock/unlock rules, or to any gameplay behaviour. No backend, no gateway.
No tests or harness.

## Allowed checks

Per-file compilation with the installed cross compiler, as before. **Do not run
`idf.py build`**: the sandbox blocks it with the psutil `sysctl()` PermissionError, which
is expected. The orchestrator runs the authoritative build.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only `channel.c` changed.
2. With credentials absent, no scan or connect call is reachable, and the badge stays on
   the configured channel indefinitely.
3. `associated` and `has_ip` stay zero; nothing reports a link that does not exist.
4. A credentialed host is byte-for-byte unchanged in behaviour, as is player discovery.
5. ESP-NOW transmit and receive work throughout.

## Final report

Standard format. State exactly which condition selects standalone mode, and confirm that
no scan or association call can be reached in it.
