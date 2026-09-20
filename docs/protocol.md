# ESP-NOW protocol v1

Extracted from the authoritative 2026-09-19 specification, including the WSS transport revision. Integers are little endian; C structures are never wire images.

### SDK facts and consequences

Use ESP-IDF v5.5.3, Wi-Fi STA on every badge, ESP-NOW on `WIFI_IF_STA`, and exactly one peer `ff:ff:ff:ff:ff:ff`, channel=0, encrypt=false. This avoids allocating one SDK peer per player. Wi-Fi must be started before ESP-NOW; the default PHY rate is 1 Mbit/s. Keep every application packet <=250 bytes. Broadcast encryption is unsupported; use project HMAC below. MAC send success is not application delivery: game acknowledgements remain necessary. Send one frame at a time, waiting for its callback; callbacks only copy bounded metadata and bytes into a queue. [ESP-NOW v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/network/esp_now.html)

Use `esp_now_recv_cb_t(const esp_now_recv_info_t *, const uint8_t *, int)` and `esp_now_send_cb_t(const esp_now_send_info_t *, esp_now_send_status_t)`. The RX info is valid only inside its callback. Copy source MAC, RSSI, channel, receive monotonic timestamp and payload; never retain SDK pointers. [Pinned header](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_now.h)

Configure `esp_wifi_set_country_code("CA", false)` and query the resulting country; discovery iterates only its allowed channels intersected with 1..11. Do not inherit the handoff's blind 1..13 sweep. Use HT20, `WIFI_PS_NONE`, no BLE, and no connectionless sleeping. STA's home channel follows its associated AP; channel changes are disallowed during scanning/connecting. [Wi-Fi API](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/network/esp_wifi.html), [Wi-Fi driver guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi.html)

### Channel and gateway state machine

A single radio cannot provide continuous simultaneous mesh operation on channel A and hotspot traffic on channel B. The round has one immutable `round_channel`, persisted with its prepare record. Host and phone can move; hotspot must expose 2.4 GHz and retain that channel for an uninterrupted backend connection.

LOBBY: host connects to its configured hotspot, reads its actual channel, then advertises the lobby there. Ordinary badge starts on last known channel for 2 seconds, then passively listens for ESP-NOW on each allowed channel for 700 ms. It accepts only HMAC-valid matching game packets, locks on a fresh packet from the designated host in any phase, or on a matching lobby participant with gateway_age_s below10, and obtains the complete frozen roster before READY. No fallback creates a second game. Discovery repeats with 1 second backoff. Selected host is provisioned; no automatic host election.

PREPARE/RUNNING: persist round_channel; participants never channel-scan merely because the host disappears. Isolated clusters remain together and continue direct tags. Loss of gateway heartbeat means `server offline`, not round reset or all players missing. On reboot the badge restores this round channel, but must obtain a fresh same-round time sample before playing. It must not treat persisted elapsed time as a running real-time clock.

Host reconnect: a single radio-owner task owns all Wi-Fi state. On STA disconnect, tear down the WSS connection and clear IP/HTTP state, then restore round_channel after the disconnect/scan operation has completed. A socket-only failure with the STA still associated needs a socket reconnect, never a Wi-Fi scan or channel change. At backoff 10, 20, 30, then 30 seconds, run one asynchronous AP scan restricted to `scan_config.channel=round_channel`, configured SSID (prefer the last BSSID among results), active min=40/max=120 ms. Free scan results exactly once. Only if an AP with that exact configured SSID and required security is currently found on that channel, attempt one connection with its returned BSSID fixed, channel hint set, `failure_retry_cnt=0`, roaming disabled. Start a 3-second association watchdog. After successful credential authentication, save the returned BSSID as the last successful AP; a hotspot restart changing BSSID therefore remains recoverable on the same channel. Once associated on the correct channel, DHCP can proceed on-channel; a separate 8-second IP timeout cancels it. Failed attempts remain offline between retries.

**The STA channel field is a hint, not a hard lock.** An AP can move between scan and connection, and SDK connection scanning can visit other channels. Monitor `WIFI_EVENT_HOME_CHANNEL_CHANGE` plus a 100-ms `esp_wifi_get_channel` check while reconnecting. Unexpected channel, association timeout, or cancelled scan triggers controlled cancellation; never advertise the new channel to participants. Suspend sends, stop scan, disconnect; if state does not settle within 500 ms, deinit ESP-NOW, stop Wi-Fi, restart Wi-Fi without calling connect, restore round_channel, then recreate ESP-NOW peer/callbacks. This is a bounded host-only radio interruption; queue retries survive it. Do not issue `set_channel` concurrently with a scan/connect. At most one recovery attempt may be in flight. An AP now on a different channel simply will not be found by the next restricted scan; host remains an offline player until hotspot returns to round_channel or the round ends. Internet outage with intact AP association needs a socket reconnect with the §5.6 backoff only, never a Wi-Fi scan or a channel change. [Scan configuration, STA hint and channel-change event source](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_wifi_types_generic.h), [Connection and scan lifecycle source](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_wifi.h)

After local round expiry, allow lobby discovery/channel changes, but retain last round's durable unsent events for upload. Ordinary badges return to discovery only after expiry/end, never on host silence. Hardware acceptance must observe host AP loss/reconnect on same channel and changed-channel fallback; this is the main integration risk, not a proven guarantee from API names alone.

### Packet contract

All integer fields are explicit little endian; serialize field-by-field, never transmit a C struct. Exact total length =48+payload_len+16 and must be <=250, so payload <=186 bytes. HMAC-SHA256 truncated to 16 bytes covers the complete header+payload using a provisioned game key. Constant-time comparison. Relays alter ttl/hops/age and recompute HMAC. This identifies the configured group, not individual trustworthy participants: every member has the key. Host HTTPS token stays host-only. Packet magic/type/length/round/roster checks precede expensive application processing.

| Offset | Header field | Bytes |
|---|---|---:|
| 0 | magic=0x5A54 | 2 |
| 2 | protocol_version=1 | 1 |
| 3 | type | 1 |
| 4 | payload_len | 2 |
| 6 | flags (bit0 relayed-capable; other bits zero) | 1 |
| 7 | ttl_remaining | 1 |
| 8 | hops | 1 |
| 9 | reserved=0 | 1 |
| 10 | game_id | 8 |
| 18 | round_id (0 lobby) | 8 |
| 26 | transport origin STA MAC | 6 |
| 32 | origin_boot_nonce | 8 |
| 40 | packet_seq | 4 |
| 44 | accumulated age_ms | 4 |

Boot nonce is random after radio RNG is available and changes per reboot. Packet sequence increments per newly originated envelope; no wrap reuse (reboot first). Flood retries get new packet_seq, while stable object IDs remain unchanged. Relays preserve origin/boot/packet_seq. Dedupe key=(origin,boot,packet_seq), not packet_seq alone. Stable infection event key=(round_id,victim_slot,event_seq u16); commit the sequence within the immutable event and reconstruct the counter from committed evidence before role flip/ACK; separate NVS keys are not a transaction. Event seq starts at1; only patient zero's causal root uses seq0. Slot is valid only in the frozen roster. Full MAC is the durable badge identity; never truncate its low four bytes.

| Type | ID | Payload fields in order | Flood? |
|---|---:|---|---|
| BEACON | 0x01 | slot u8, role u8, phase u8, flags u8, role_rev u16, infection_cause_seq u16, own_produced_seq u16, own_server_received u16, own_server_final u16, elapsed_ms i32, time_quality u8, round_channel u8, snapshot_rev u32, gateway_serial u32, gateway_hops u8, gateway_age_s u8, cache_digest u64, cache_count u16, uncertainty_ms u16, gateway_boot_nonce u64 (50 bytes) | Never |
| TAG_REQUEST | 0x02 | victim_slot u8, actor_slot u8, actor_cause_seq u16, actor_role_rev u16, known_victim_role_rev u16, attempt_seq u32, actor_elapsed_ms u32, tagger_observed_victim_rssi i8 (17 bytes); boot ID in header | Never |
| TAG_RESULT | 0x03 | actor_slot u8, request_boot u64, request_seq u32, result u8, accepted EVENT body (30 bytes; zero when rejected) (44 bytes) | Never |
| EVENT | 0x10 | victim_slot u8, event_seq u16, actor_slot u8, actor_cause_seq u16, actor_role_rev u16, victim_prior_role_rev u16, request_boot u64, request_seq u32, occurred_elapsed_ms u32, uncertainty_ms u16, tagger_observed_victim_rssi i8, victim_observed_tagger_rssi i8 (30 bytes) | Yes |
| ROSTER_PAGE | 0x20 | snapshot_rev u32, roster_hash u64, page_index u8, page_count u8, entry_count u8, entries <=8 each {slot u8, MAC6, name_len u8, name[12]} (<=175 bytes) | Yes |
| HOST_STATE | 0x21 | snapshot_rev u32, roster_hash u64, phase u8, page_index u8, page_count u8, entry_count u8, host_elapsed_ms i32, rule_rev u16, round_channel u8, winner u8, duration_ms u32, remaining_ms u32, patient_zero_slot u8, gateway_serial u32; <=12 entries each {slot u8, role u8, role_rev u16, cause_slot u8, cause_seq u16, covered_seq u16, flags u8}, uncertainty_ms u16 (<=159 bytes) | Yes |
| WATERMARKS | 0x22 | snapshot_rev u32, roster_hash u64, count u8, then <=20 {slot u8, server_received_contiguous u16, server_final_contiguous u16} (<=113 bytes) | Yes |
| COMMAND | 0x23 | command_seq u32, cmd_kind u8, target_slot u8 (255=all), valid_until_elapsed_ms u32, args_len u8, args <=175 bytes | Yes |
| COMMAND_RECEIPT | 0x24 | slot u8, command_seq u32, state u8, detail u16, applied_snapshot_rev u32 (12 bytes) | Yes |
| CACHE_PAGE | 0x30 | target_slot u8 (255=broadcast), cache_digest u64, page_index u8, page_count u8, count u8, <=56 {origin_slot u8, event_seq u16} (<=180 bytes) | Never |
| WANT_EVENTS | 0x31 | target_slot u8, request_seq u16, count u8, <=16 {origin_slot u8,event_seq u16} (<=52 bytes) | Never |
| EVENT_COPY | 0x32 | target_slot u8, full EVENT body30 (31 bytes) | Never |
| TIME_QUERY | 0x33 | target_slot u8, request_nonce u32 (5 bytes) | Never |
| TIME_REPLY | 0x34 | target_slot u8, request_nonce u32, sampled_elapsed_ms i32, source_snapshot_rev u32, source_age_ms u32, uncertainty_ms u16, time_quality u8 (20 bytes) | Never |

TAG_RESULT codes: ACCEPTED=0, ALREADY_ZOMBIE=1, OUT_OF_RANGE=2, ROUND_INACTIVE=3, STALE_ACTOR=4, BUSY=5, NOT_ROSTERED=6, PENDING=7. Duplicate accepted requests return ACCEPTED with the same event reference. These match §4.4. Do not expose success before the victim's durable commit. Invalid/malformed packets get no response. Type lengths are exact except explicitly counted arrays; trailing bytes are rejected.

Command arguments are specified below and never exceed 175 bytes. Use roster/state pages, not generic fragmentation. ANNOUNCE96 ASCII easily fits. PREPARE and START refer to assembled roster_hash/snapshot_rev. No command embeds an unbounded log. Fields marked role_rev u16 must not wrap within a round.

### Direct proximity and timing

Only a BEACON with hops=0, ttl=0, header origin MAC equal to SDK RX source MAC, valid slot/MAC mapping and matching round can update that player's proximity. A relayed EVENT received loudly is evidence of a nearby relay, not of a nearby victim/actor. Direct TAG_REQUEST and TAG_RESULT also require actual SDK source MAC equals the expected roster player; they are never forwarded. Threshold checks use both tagger's recent direct samples and victim's own direct samples/request RSSI. The tighter §4.3 source-age/three-sample rules and two-sided checks are mandatory.

BEACON period500ms with independent +-100ms jitter; do not use packet floods for proximity. Direct peers stale after4seconds, tag eligibility1.0seconds, eviction10seconds (roster itself never evicted). EWMA0.6 old+0.4 new, initialize from first sample. Channel scan/driver recovery invalidates old proximity samples so returning radio does not permit stale range tags.

HOST_STATE/CLOCK sample is generated fresh by host, not replayed as if its time were new. `age_ms` accumulates local queue residence at each relay and a nominal3ms/link transmission allowance; receiver estimates round time = sample elapsed + age. Queue an individual clock frame at most200ms locally and drop copies with accumulated age above1500ms. Local `esp_timer` advances learned elapsed time and deadline. Never extend an already learned local round deadline based on a later sample. Repeated START with same command ID never resets it. A rebooted registered badge requests a TIME_REPLY from a same-round direct peer, measuring RTT with request_nonce; accept RTT<=250ms and good peer time quality, otherwise await host. The peer samples its current elapsed at reply construction; receiver adds halfRTT. This is approximate subsecond synchronization, not precision distributed time; the engineering expiry/uncertainty rule in §4.2 applies. Mark source freshness/quality; a peer without initialized time cannot initialize another badge.

### Forwarding, delivery and congestion

Flood types originate ttl=7,hops=0 (at most eight radio links including the original transmission); forwarding requires ttl>0, decrements ttl and increments hops. A receiver processes an authenticated object once, but may relay a fresh envelope for a stable object already stored. Envelopes leave each node at most once; randomized forwarding jitter30..120ms. No flooding of BEACON, TAG, TAG_RESULT, or cache exchange. Do not claim guaranteed reach beyond eight radio links in one flood; later neighbor cache exchange provides store-carry-forward.

Origin sends EVENT immediately and retries at0.7s and1.7s, then every30seconds until server_received watermark covers it. Retry schedule is jittered +-20%; a node services at most one pending replay persecond. Retain durable local events through the round even after received; only server final decisions settle acceptance/scoring. Host receiving a packet, host's HTTP200, server durable event receipt, server adjudication, and badge applying a command are distinct states. No one ACK substitutes for the rest. Server watermarks advance only over contiguous event IDs, including rejected final events; gaps must be repaired, never skipped.

Every participant caches up to128 distinct event bodies in RAM across at most active/previous round; own unfinalized events and host-upload obligations are durable. Relay-only caches may be RAM-only; the original victim remains a durable custodian, and two-hop delivery is not a promise to survive loss of every custodian. Cache summaries in direct BEACON allow peers to notice different sets. A badge may run one neighbor exchange at a time: choose a changed-digest neighbor round-robin, at most once perneighbor per10seconds; send up to3 CACHE_PAGEs for128 keys at200ms spacing, within rate limit. Receiver requests up to16 missing keys, responder sends EVENT_COPYs at maximum2/second. All are link-local broadcasts with explicit target_slot; unintended recipients may cache EVENT_COPY but must not answer a request not addressed to them. Loss heals on the next exchange, new digest or30-second retry. Never erase missing events just because another badge says its digest differs. Full cache means stop accepting new own tag transitions with `storage full`, retain existing unfinalized events, and drop nonessential relay copies first. The storage layout/capacity in §3 applies.

Host emits a fresh clock every2seconds using HOST_STATE with page_count=0,page_index=0,entry_count=0; this is clock/phase metadata only and never clears a roster or roles. Full role snapshots have page_count>=1 and are assembled separately by snapshot/hash/type.  latest role pages/watermarks every10seconds and promptly after changes (coalesce250ms). Frozen roster pages are sent during preparation and on explicit rejoin request, not perpetually eachsecond. Commands retry every2seconds until actual target receipt, with per-round idempotent command IDs; latest critical state remains available by snapshot. Announcements can expire/drop without blocking later commands. Host receipt must reflect each badge's applied state, not simply its own successful RF send.

Single TX owner priorities: TAG_RESULT / own TAG_REQUEST; new EVENT and essential phase/role control; clocks and repair; cached event replay; cosmetic messages/telemetry. Perbadge token buckets: critical local direct frames8/s burst8, flooded/control6/s burst8, repair2/s burst2, BEACON2/s burst2. Total transmissions20/s burst20 including every category. Expired low-priority packets drop; pending critical objects remain in durable queues for retry. No continuously flooded perplayerREPORT in core. Optional nearest4-neighbor telemetry no faster than10seconds, and disabled first under congestion.

Budget illustration, not throughput guarantee:20 players produce about40 direct beacons/s. Host clock one flood/2s permits up to10 transmissions/s network-wide; two role pages plus one watermark per10s add up to6/s. A simultaneous19-infection burst needs up to380 transmissions per flood attempt before retries, far larger than the handoff's beacon-only estimate. Token buckets smooth it; convergence may take seconds. Local tagging stays higher priority. At1Mbit/s a250-byte payload alone takes2ms, before framing, backoff, interference and hotspot traffic. Thus avoid claiming20-player venue capacity without observation. With3 badges, verify direct tag, broken host path, reconnect delivery and chain relay before increasing roster.

Static suggested radio allocations: RX ring32*(250+24)=8768B; TX pool24*(250+24)=6576B; envelope dedupe256*24=6144B; direct peers20*80=1600B; event cache128*40=5120B; reassembly2KiB; misc flags/counters<2KiB, subtotal<34KiB excluding Wi-Fi/RTOS and shared game/durable buffers. Dedupe covers flood envelopes only; perpeer boot/sequence state filters direct beacons, avoiding cache churn from40beacons/s. Copy RX to fixed queue without allocation; if full increment counter and drop. Pending clock/stat pages coalesce. Expose drops, queue highwater, replay backlog, gateway age, channel and RSSI in a diagnostics screen/console for manual demo checks.

### Additional admission, close, and command encodings

The following messages complete the wire contract; workers must not invent private variants. All use the same 48-byte envelope and HMAC. Roster and state pages use zero-based indices. Integers wider than a byte are little endian. Fixed name[12] fields are zero-padded after name_len; reject embedded controls and nonzero padding.

| Type | ID | Payload fields, in order | Forwarding |
|---|---:|---|---|
| JOIN | 0x04 | requested_badge_mac[6], name_len u8, name[12], known_round_id u64, request_nonce u32, build_id[8] (39 bytes) | Flood, rate limit one request per badge per 3 s; round header zero for lobby or known current round for rejoin |
| JOIN_RESULT | 0x05 | target_mac[6], request_nonce u32, status u8, slot u8, snapshot_rev u32 (16 bytes) | Host-origin flood; only named MAC applies |
| SNAPSHOT_REQUEST | 0x25 | slot u8 (255 if unassigned), wanted_snapshot_rev u32, roster_hash u64, need_flags u8 (14 bytes) | Flood at most once per badge per 5 s |
| ROUND_CLOSED | 0x26 | slot u8, produced_seq u16, last_role_rev u16, close_elapsed_ms u32 (9 bytes) | Flood every 5 s until server records close frontier, then stop |
| CLOSE_RECEIPTS | 0x27 | closed_bitmap u32, archived_bitmap u32 (8 bytes; only bottom 20 bits valid) | Host-origin flood; acknowledges server durable round-close records/clearance |
| EVENT_DECISIONS | 0x28 | snapshot_rev u32, count u8, <=20 entries {victim_slot u8, event_seq u16, status u8, reason u8, archived u8} (<=125 bytes) | Host-origin flood; durable per-event decisions |
| DECISION_RECEIPT | 0x29 | slot u8, own_decided_contiguous u16 (3 bytes) | Flood after new durable decisions, coalesce 500 ms |

JOIN status: 0 registered, 1 rejoined, 2 registration closed, 3 room full, 4 bad configuration, 5 waiting for server. Slot 255 means not assigned. Admission still requires complete authoritative roster/rules; a JOIN_RESULT alone is not permission to tag. Replay the same request_nonce until resolved. A direct JOIN must have matching requested MAC/header origin/SDK source; relays preserve its original identity.

Role encoding: human=0, zombie=1, unknown=255. On-wire phase: lobby=0, prepared=1, running=2, expired_pending_sync=3, final=4. Flags are explicitly defined per message; reject unknown mandatory bits. BEACON flags: bit0 designated host, bit1 role provisional, bit2 storage healthy, bit3 registration acknowledged; all others zero. Time quality: 0 unknown, 1 initialized with <=2,000 ms uncertainty. Role-state entry flags: bit0 provisional, bit1 player ready; others zero. Gateway serial increases for each fresh host clock, never for a relay copy. A peer may advertise only the freshest gateway serial it has actually received; gateway age increases with monotonic time and saturates at 255 s.

Common COMMAND payload starts with `command_seq u32, kind u8, target_slot u8, valid_until_elapsed_ms u32, args_len u8`. Critical state commands use expiry `0xffffffff` and rely on round/revision checks; ANNOUNCE uses a real expiry. Kind and exact args:

| Kind | Value | Args |
|---|---:|---|
| PREPARE_ROUND | 1 | snapshot_rev u32, roster_hash u64, roster_count u8 (2–20), duration_ms u32=600000, channel u8, tag_rssi i8, tag_cooldown_ms u16=3000 (21 bytes) |
| START_ROUND | 2 | snapshot_rev u32, roster_hash u64, patient_zero_slot u8, initial_role_rev u16, sampled_elapsed_ms i32, duration_ms u32, uncertainty_ms u16 (25 bytes) |
| ROLE_SET | 3 | role u8, role_rev u16, cause_slot u8, cause_seq u16, covered_seq u16 (8 bytes; human cause_slot=255/cause_seq=0) |
| ANNOUNCE | 4 | text_len u8, printable ASCII text[1..96] (2–97 bytes) |
| END_ROUND | 5 | effective_elapsed_ms u32, reason u8, winner u8 (255 unknown), provisional u8 (7 bytes) |
| FINAL_RESULT | 6 | winner u8, complete u8, missing_slots_bitmap u32, state_rev u32 (10 bytes) |
| CANCEL_PREPARE | 7 | reason u8 (1 byte) |

END reason: time_limit=0, all_infected=1, operator_stop=2. CANCEL reason: absent_players=0, operator_cancel=1, invalid_channel=2. A cancel affects PREPARED only, never RUNNING. A replacement preparation uses a new round ID. Final-result completeness is false when explicitly forced with missing evidence; UI says `FINAL · INCOMPLETE DATA`.

COMMAND_RECEIPT state: received=0, applied=1, prepared_ready=2, rejected=3, requires_snapshot=4. Detail: none=0, wrong_round=1, invalid_args=2, storage_failure=3, missing_pages=4, old_events_pending=5, stale_revision=6. Host accepts receipts only from their slot's mapped origin. RECEIVED means durable transport custody; APPLIED/PREPARED_READY means the target actually committed its state.

EVENT_DECISIONS status: accepted=0, rejected=1, pending_dependency=2. Reason: none=0, wrong_round=1, not_rostered=2, invalid_parent=3, outside_round=4, duplicate_conflict=5, stale_role=6, invalid_payload=7. Archive flag is 0/1. Host repeats latest undecided-delivery pages every 10 s and promptly on change until the origin's DECISION_RECEIPT covers them. Pending decisions never advance that receipt's contiguous final frontier. If a watermark says a missing decision exists, send SNAPSHOT_REQUEST need_flags bit3 rather than inventing an accepted result. Backend sync includes `decision_applied:[{slot,through_seq},...]` so individual receipt reaches the server. Relay caches may record decisions for their own retained copies; only an event origin's acknowledgment settles that origin's delivery.

Authoritative types COMMAND, HOST_STATE, ROSTER_PAGE, WATERMARKS, JOIN_RESULT, CLOSE_RECEIPTS, and EVENT_DECISIONS require envelope origin MAC equal to the configured host. Relays preserve that origin. Do not require radio source MAC to equal host for these multihop packets. Group HMAC protects against accidental/unauthorized outsiders without the key; it does not provide malicious-participant-proof authority because members share the key. This POC trusts enrolled participants and makes no competitive anti-cheat guarantee.

SNAPSHOT_REQUEST need_flags: bit0 roster, bit1 roles/phase/time, bit2 watermarks, bit3 individual event decisions; other bits zero. Cache sets/digests/counts are **per round**, not aggregated: every CACHE_PAGE/WANT_EVENTS/EVENT_COPY belongs to the envelope's round ID, and its compact keys are interpreted only in that round. BEACON summarizes its header round. A badge retaining the immediately previous round offers direct old-round CACHE_PAGE inventories once per 30 s, even while its ordinary beacon is current-round; neighbors may answer for either retained round. Limit one repair exchange overall per node. Round ID plus page digest binds assembly; never merge pages across rounds or digests. Cache digest is the first eight SHA-256 bytes of sorted canonical `(slot u8,seq u16)` keys for that round, encoded little endian. An empty cache hashes the empty sequence.

The one-in-flight TX rule has a 500 ms callback watchdog. Missing completion records a failed send and triggers the radio owner's controlled ESP-NOW/Wi-Fi recovery; keep logical objects for retry. Increment a driver-generation counter on each restart, stop old driver callbacks before accepting a new send, and discard queued completions/RX work tagged with the old generation. Never free/reuse the in-flight buffer until callback or completed driver teardown. This prevents a lost callback from permanently stalling every transmission.

The firmware's role_rev field is u16; no wrap is allowed within a round. Snapshot/command/state revisions are u32. The backend rejects would-be overflow and starts a fresh round/game identifier only after ordinary archival rules. Roster page hash is SHA-256 over entries sorted by slot (`slot,mac6,name_len,name12`); take the first eight digest bytes as the transmitted hash bytes. Names cannot be changed within a frozen roster.

### Precise time and event rules

Signed elapsed fields can be negative during a scheduled countdown. `INT32_MIN` means unknown. Event timestamps are unsigned and occur only at elapsed >=0. START's sample describes time when the host constructs that envelope; retry reconstructs the timing sample while retaining the same logical command ID. Relays add actual local queue residence to `age_ms` plus 3 ms per transmitted link and recompute the group HMAC. No peer rebrands an old sample as new.

Host clock anchor comes from the server's `server_time_ms` at response generation plus half measured request round-trip time. Initial uncertainty is `ceil(RTT/2)+50 ms`; accept a new time anchor only for RTT <=2,000 ms and a sane server/round match. Afterward grow uncertainty by 200 ppm of monotonic elapsed time. A forwarded sample adds its accumulated age to estimated elapsed and an additional `100 ms * (hops+1)` engineering margin to uncertainty. The exact queue delay is already measured, not added again as uncertainty. Drop samples whose resulting uncertainty exceeds 2,000 ms. The 100 ms/link allowance is a POC engineering margin, not a proven upper bound on RF contention; deadline accuracy remains approximate. Prefer matched-nonce direct time exchanges where available and report degraded timing instead of claiming precision synchronization.

TIME_QUERY/TIME_REPLY initializes a rebooted badge from a continuing peer: use request_nonce, matching MAC/round, RTT <=250 ms, and source quality 1. Receiver estimates peer sampled elapsed plus half RTT; uncertainty becomes peer uncertainty plus `ceil(RTT/2)+10 ms`. The peer replies using its current locally advanced elapsed/uncertainty, not a stale saved number. Once a badge has a local deadline, later valid samples can shorten it but never extend it. During initial start scheduling, a corrected countdown can shorten the time to start; do not flip back to PREPARED after play begins.

BEACON (50 bytes), HOST_STATE (at most 159 bytes), START args (25 bytes), and TIME_REPLY (20 bytes) include uncertainty in their canonical layouts above. Each event stores uncertainty at victim RX/validation; storage completion latency does not alter occurrence time. Gateway clock identity is (configured host MAC, host boot nonce, gateway_serial). HOST_STATE carries serial explicitly and its envelope carries host boot nonce; participant BEACONs forward both serial and gateway_boot_nonce. On a new host boot epoch, freshness starts from a newly received valid host frame, never by comparing its serial numerically with the previous epoch.

An event's actor cause sequence denotes the actor's own victim-slot infection, except patient-zero sequence 0. A pending parent does not prevent local infection when direct authenticated zombie evidence is current; server causality resolves it later. Corrected roles with missing/invalid parents never grant final score until the server decides.

Flood receipt is not durable storage or finalization. The backend must separately distinguish `received_events` (durably ingested, including events still pending a dependency) from `decisions` (adjudicated); §5.5 carries these as the distinct `receipts` and `decisions` message types, up to eight entries each. A successful WebSocket send acknowledges nothing. WATERMARKS' received frontier is built from durable ingestion, final frontier from accepted/rejected adjudication; pending dependencies do not advance the final frontier. Every badge retains own evidence until final clearance even after the received frontier stops its repeated flood.

On saturation: direct TAG_RESULT/TAG_REQUEST use reserved queue capacity; new critical objects stay in bounded durable storage; ordinary clock/state frames coalesce by snapshot/type; cosmetic traffic drops first. Host-event custody must be committed before advertising it as server-received (which only the actual backend can establish). Persisting an event in the host is not itself server receipt.


### 4.2 Clock behavior

Gameplay uses monotonic `esp_timer_get_time()`, never an adjustable wall clock. Host wall time is needed for HTTPS certificate validation and matching the server's scheduled start; seed it from operator provisioning and refresh by SNTP before round preparation. Do not disable TLS certificate-time checks to work around a bad clock.

Start/host status messages contain round elapsed/remaining time; forwarding includes bounded packet age. A live badge derives its local deadline once, then may shorten it using fresher valid timing but never extend it. Normal disconnected play keeps that deadline. A full reboot has no trusted elapsed-time continuity: retain role/events, show `REJOINING`, and require a fresh same-round time/state exchange with a continuing registered peer or the host before tagging. A saved checkpoint alone cannot restart ten minutes.

Time uncertainty is recorded with events. Reject new local tags when the badge cannot establish that the round is still running. A new tag requires `estimated_elapsed_ms + uncertainty_ms < 600000`. The server uses the same engineering uncertainty-margin rule; an event interval crossing expiry is rejected as OUTSIDE_ROUND. At most the final two seconds may be unavailable for tagging under poor clock quality; the displayed round still ends at its learned deadline. Events with uncertainty above 2,000 ms require resynchronization before local play.

### 4.3 Proximity and tag selection

Only fresh **direct** beacons and the direct TAG receive RSSI influence tag range. Relayed role/event reports never create a local radar contact or proximity measurement.

- Beacon interval: 500 ms ±100 ms jitter. Peer table is the frozen roster plus bounded lobby discoveries.
- EWMA: `filtered = 0.6 * previous + 0.4 * new`, initialized from the first measurement; fixed-point arithmetic is fine.
- Track at least three samples in the last 1,500 ms. Candidate must have been heard within 1,000 ms; drop radar freshness after 4 s and retained direct-sample history after 10 s.
- Initial tag threshold: **−58 dBm**, adjustable during commissioning before a round. Require both filtered and most-recent beacon RSSI at/above threshold. Victim independently requires direct TAG RSSI and its recent sender measurements to meet the threshold.
- Display tiers: at least −58 `IN RANGE`, at least −70 `CLOSE`, at least −82 `NEARBY`, otherwise `FAR`.
- Choose the nearest eligible human by highest filtered RSSI; ties break by lower roster slot. Show the selected name before the button press. A is press-edge only; holding it does not auto-tag.
- Three-second tag cooldown starts when a valid tag attempt is transmitted. An attempt with no eligible target just shows `GET CLOSER`, with 500 ms UI-message rate limiting.

Arm's length is the intended interaction, not a guaranteed radio boundary. Body blocking, badge orientation, and walls can change RSSI. No meters, compass bearings, or exact-distance claims appear on screen.

### 4.4 Tag transaction

1. Tagger must be a running zombie with a known infection cause, local deadline not expired, cooldown clear, and an eligible direct human.
2. Allocate `(tagger boot nonce, attempt sequence)`; bind victim slot, actor cause, current round, and role revisions. Send the same logical request at 0, 250, and 750 ms, each in a fresh transport envelope. Stop retries on a matching response. Wait at most 1,500 ms for confirmation.
3. Victim checks complete packet validity, game/round, roster membership, direct source MAC, matching target, actor role/cause, fresh mutual proximity, running timer, and its own human role. All checks precede state change. The actor’s causal parent event may not yet be cached: accept provisionally when its fresh authenticated direct beacon advertises the matching zombie role/cause, request the missing parent, and let the server adjudicate that dependency. Do not block offline chain infections on an Internet lookup.
4. Victim enters PERSISTING, allocates the next infection sequence, records the accepted request plus causal infection event in one immutable committed value, then changes local role to zombie and emits `TAG_RESULT(ACCEPTED,event_ref)` and an infection EVENT.
5. Send result immediately and again after 150 ms. A duplicate request returns the existing outcome/event reference without a second infection, score, write, or animation. Cache 16 recent tag outcomes for 10 s; durable infection records also prevent duplication after reboot.
6. Simultaneous tag requests are serialized by the game task. The first valid request that is durably committed wins. The loser gets `ALREADY_ZOMBIE`; it does not receive infection credit.
7. Tagger shows success only for a matching accepted result or the victim's matching infection event. With no result, show `UNCONFIRMED`; lack of ACK must not undo an infection that the victim already committed.
8. A victim that cannot persist returns `BUSY` and remains human. Invalid/late/out-of-range requests return bounded reason codes when safe; do not create response storms to malformed traffic.

Result codes: `ACCEPTED=0`, `ALREADY_ZOMBIE=1`, `OUT_OF_RANGE=2`, `ROUND_INACTIVE=3`, `STALE_ACTOR=4`, `BUSY=5`, `NOT_ROSTERED=6`, `PENDING=7`. Never relay TAG or TAG_RESULT.

