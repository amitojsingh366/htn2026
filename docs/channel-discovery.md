# LIVEG013: follow the advertised game channel

Reported symptom: two registered badges, host on channel 6 and client on channel
5, with preparation never completing. Source review found that discovery used
the receiver's channel metadata to renew its lobby lease. That metadata does not
identify the host's configured channel. If a host advertisement arrives while
the client listens on a different channel, the old code keeps the client there
instead of following the advertisement's authenticated `round_channel`.

The client now follows the advertised channel from fresh BEACON and HOST_STATE
messages. The radio owner changes the hardware channel before publishing lobby
contact. Messages without a channel, including JOIN_RESULT, cannot establish or
renew that lease. Delayed client Wi-Fi channel-change notifications no longer
overwrite the owner's newer selection. Same-channel advertisements renew the
lease without clearing peer contact history; loss of contact still resumes
discovery after the existing ten-second lease.

A registered client missing its first roster can also receive a direct,
authenticated host beacon for the prepared round. Its existing snapshot requests
then recover the roster and cached PREPARE. Game identity, host/source identity,
HMAC verification, roster admission and locked-round channel rules remain in
force. The server still waits for both real PREPARED_READY receipts before
assigning roles and starting the shared countdown. No backend change is needed.

## Verification

The native channel and mesh discovery regressions exercise the production C
implementation using SDK stubs; they do not simulate RF propagation. Their entry
points are `firmware/tests/run-channel-tests.sh` and
`firmware/tests/run-mesh-discovery-tests.sh`. Build the firmware with ESP-IDF 5.5.3.

Validation completed: all nine channel scenarios and the mesh discovery suite
pass, including real wire HMAC rejection. The primary channel and mesh cases
fail against the original implementation. Existing announcement and gateway
diagnostic tests also pass. The ESP32-C3 firmware compiles and links with ESP-IDF
5.5.3; its application is `0x1438f0` bytes in the `0x2a0000` factory partition
(52% free). The pre-existing unused `self_install` warning remains.

Physical confirmation remains necessary: update the affected badges through the
existing guarded `tools/badge_ops.py` flash workflow, boot the host and client,
register both, and start from the host. The client should settle on the host's
channel, both badges should become ready, and both should enter the countdown.
Also verify recovery when the client misses the first preparation messages.
This change has not been flashed or validated on physical badges by this task.
