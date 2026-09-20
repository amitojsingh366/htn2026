# Operations skeleton

## Private inputs

TODO(orchestrator): supply the HTTPS backend base URL, game ID, host token, 2.4 GHz hotspot SSID/password, 32-byte mesh group key, designated host MAC, and private primary/independent secondary archive roots at execution time. None of these private session inputs are ever committed. Supply participant names and configuration through private files, never command-line secrets. The backend teammate supplies the service and operator controls.

## Build and release

Use ESP-IDF v5.5.3 and espressif/led_strip 3.0.3 and espressif/esp_websocket_client 1.8.0. Set target esp32c3, build, and inspect application size against the 0x2A0000 factory partition. Generated bootloader and partition-table artifacts are not deployment targets.

TODO(orchestrator): record source commit, build ID, SDK/tool versions, application/ELF/map hashes, sanitized configuration, reviewed schemas, size/heap observations, and guarded release manifest. Integrate task creation and replace every F00 scaffold before release.

## Enrollment, installation and restoration ordering

See [recovery.md](recovery.md), owned by R01, for the guarded recovery procedure and exact gates. This document does not replace that runbook.

1. TODO(orchestrator): establish the private archive and verify a second copy on independent durable storage.
2. Enroll each physical badge independently with two matching complete original reads and both verified archive copies before any custom write.
3. Rehearse whole-image restoration on the first badge and verify complete readback plus stock boot before its first custom installation.
4. Create the reviewed release manifest. Use only the guarded application installation path, including a fresh prewrite capture, identity/layout/security checks and complete readback.
5. Boot to WAIT_INSTALL; guarded install initialization writes the installation header last. Provision privately, then commission protected-region preservation after both player and host initialization.
6. TODO(orchestrator): record first-badge rehearsal, per-badge commissioning and actual gameplay/mobile-host observations privately. Hand each owner their verified local bundle and receipt.
7. Restore the same physical badge using its verified archive and the gates in recovery.md. Never substitute another badge's image.

## Known limitations

F00 is a contracts-only scaffold. No gameplay, radio, storage, UI, gateway or console implementation is delivered here. Backend availability and agreed contract, hardware access and private operational inputs remain execution prerequisites. RSSI is approximate proximity, never distance or direction. Twenty players is a capacity limit, not measured venue performance. Offline results remain provisional pending reconciliation. A hotspot must use 2.4 GHz and retain the locked round channel. USB does not recharge AA cells. No verified battery ADC or percentage is available. Accelerometer and NFC remain uninitialized. Restoration covers flash on the same functioning badge, not hardware damage or irreversible device changes.

TODO(orchestrator): record observed limits, host reconnect behavior, display orientation/color confirmation, heap high-water measurements and unresolved deployment issues.
