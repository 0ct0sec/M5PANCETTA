# NOWFLOCK Changelog

Normative spec: `rfc/NOWFLOCK_STANDARD.md` (RFC-F0690-v3). When the RFC
changes, update `scripts/nowflock_lsp.py`, `scripts/sim_nowflock_v3.py`, and
`tools/sim_nowflock_visual.html` before declaring conformance.

The specification text is licensed under CC BY 4.0. Anyone may read, copy,
adapt, redistribute, and implement FNOW/3 for any purpose, including commercial
use. Copied or adapted specification text must retain attribution and identify
changes; compatible implementations require no separate protocol fee.

## v3 (RFC-F0690-v3, 2026-07-03)

- 2026-08-18 documentation-only update: published the normative RFC under
  CC BY 4.0 with an explicit open implementation statement. No wire-format or
  conformance behavior changed.

- FNOW wire version `0x03`; supersedes v2.
- HELLO byte 6 repurposed as `capabilities` (`CAP_LSP1`, `CAP_BLE_HEARTBEAT`, `CAP_PIGBROTHER`).
- Normative **Local Scoring Profile LSP-1**: tile quantizer, merge rules, tier/state from confidence, export gate.
- SYNC child adoption rules clarified (GPS/trusted-clock guard, uptime drift correction).
- Conformance suite grows to **34 scenarios** in `scripts/sim_nowflock_v3.py`.
- Visual simulator: `tools/sim_nowflock_visual.html` (FNOW/3 + LSP-1 labels).

### Firmware implementation notes (HAMLET PANCETTA)

- `nowflock_feed` injects XBand WiFi+BLE cohort observations (local LSP-1 source, not on wire).
- ESP-NOW TX deferred while `Recon::isScanning()` (radio coexistence).
- Master SYNC also sent on 30s cadence (`SYNC_INTERVAL_MS`) in addition to post-GPS-fix path.

## v2 (RFC-F0690-v2, 2026-06-30)

- FNOW/2 wire format; not backward-compatible with v1.
- 4-byte keyed CRC32 auth tag; per-sender sequence deduplication.
- HELLO `claimed_channels` + `group_id`; ASSIGN channel mask + `utc_epoch_min` + `master_node_id`.
- SIGHTING capacity increased to 7 candidates (30-byte wire rows).
- Peer corroboration ingest (`EVID_PEER_CORROBORATION`).
- New message types: SYNC, PEER_REQ, EXPORT_SNAPSHOT (optional PIGBROTHER).
- Battery-aware TX throttling; max-node_id bully leader election.
- 29 conformance scenarios in `scripts/sim_nowflock_v2.py`.

## v1 (RFC-F0690)

- Initial ESP-NOW broadcast coordination; 4 candidates per SIGHTING; 32-byte wire rows.
- Superseded by v2.
