NOWFLOCK                                                   RFC-F0690-v3
                                                             2026-07-03


           NOWFLOCK Peer Coordination Protocol -- Version 3


Status of This Document

   This document is the normative protocol specification for NOWFLOCK
   version 3 (FNOW/3).  It is not an IETF standard.  All 34
   conformance scenarios in scripts/sim_nowflock_v3.py MUST pass
   before any implementation of this specification is declared
   conforming.

   This document supersedes RFC-F0690-v2 (NOWFLOCK version 2).
   See rfc/NOWFLOCK_CHANGELOG.md for version history.

   Open use: Copyright (c) 2026 NOWFLOCK authors.  This specification
   text is licensed under Creative Commons Attribution 4.0
   International (CC BY 4.0):

      https://creativecommons.org/licenses/by/4.0/

   Anyone may read, copy, redistribute, and adapt the specification
   text for any purpose, including commercial use, provided they give
   appropriate credit, link to the license, and indicate changes.

   Anyone may implement a compatible FNOW/3 protocol for any purpose,
   including commercial use, without requesting permission from or
   paying a protocol fee to the NOWFLOCK authors.  This open
   implementation statement does not license separately copyrighted
   firmware, grant third-party patent or trademark rights, or permit
   radio operation outside applicable law and authorization.


Abstract

   This document specifies NOWFLOCK protocol version 3 (FNOW/3), an
   inter-badge coordination layer for multi-node ESP-NOW mesh groups.
   Version 3 adds normative Local Scoring Profile LSP-1 and a HELLO
   capabilities byte.  Version 3 is not backward-compatible with
   version 2 (FNOW/2).  Version 2 is not backward-compatible with version 1
   (RFC-F0690).  Version 2 added: an application-layer frame tag, UTC epoch
   time synchronization via ASSIGN, channel-division advertising via
   HELLO, automatic leader election, expanded SIGHTING capacity (7
   candidates vs. 4), a peer-corroboration model for cross-node
   candidate confidence boosting, two new message types (SYNC and
   PEER_REQ), an optional PIGBROTHER wardrive export role, battery-
   aware transmission throttling, sequence deduplication, and a
   compact redesigned header.  Version 3 retains that frame shape and
   makes local scoring, capability signaling, and SYNC adoption
   normative.

   The protocol still runs over ESP-NOW broadcast, requires no
   infrastructure, and keeps routine summary payloads free of raw
   device identifiers.  TARGET remains the explicit exception because
   it carries the selected BSSID for coordinated action.  PIGBROTHER
   EXPORT_SNAPSHOT frames are also explicit opt-in export material:
   they may carry wardrive CSV fields for configured export nodes.


Table of Contents

   1.   Motivation and Changes from Version 1 .................   4
   1.1.   What Version 1 Got Right ............................   4
   1.2.   Confirmed Protocol Constraints ......................   4
   1.3.   Deficiencies Addressed in Version 2 .................   5
   1.4.   Changes from Version 2 to Version 3 .................   5
   2.   Prior Art and Design Influences .......................   5
   2.1.   IEEE 802.11s Mesh ...................................   5
   2.2.   BATMAN-Adv OGM Interval Sizing ......................   6
   2.3.   SWIM Gossip .........................................   6
   2.4.   FTSP/TPSN Time Synchronization ......................   6
   2.5.   BLE Advertising as a Hunt-Window Heartbeat ..........   6
   2.6.   Frame Tag Approach ..................................   7
   2.7.   IEEE 802.11k Neighbor Report ........................   7
   2.8.   GhostESP and Existing Single-Node Tools .............   7
   3.   Frame Format ..........................................   8
   3.1.   Header -- Version 3 (24 Bytes) ......................   8
   3.2.   Frame Tag (Optional, 4 Bytes) .......................   9
   3.3.   Frame Validation ....................................  10
   3.4.   Sequence Deduplication ..............................  10
   3.5.   Frame Size Budget ...................................  11
   4.   Message Types .........................................  11
   4.1.   Type 1 -- HELLO .....................................  11
   4.2.   Type 2 -- ASSIGN ....................................  12
   4.3.   Type 3 -- SIGHTING ..................................  13
   4.3.1.   Candidate Wire Record (30 Bytes) ..................  14
   4.3.2.   Peer Sighting Ingest .............................  15
   4.4.   Type 4 -- TARGET ....................................  15
   4.5.   Type 5 -- CAPTURE ...................................  16
   4.6.   Type 6 -- SYNC (Added in v2) ........................  16
   4.7.   Type 7 -- PEER_REQ (Added in v2) ....................  17
   4.8.   Type 8 -- EXPORT_SNAPSHOT (Optional, PIGBROTHER) ....  17
   5.   Node Identifier .......................................  17
   6.   Role Model and Coordination ...........................  18
   6.1.   Role Definitions ....................................  18
   6.2.   Leader Election -- Max-Node_ID Bully ................  18
   6.3.   Channel Division ....................................  19
   6.4.   Sighting Interval Negotiation .......................  20
   7.   Timing Reference ......................................  20
   8.   Battery-Aware Transmission Policy .....................  20
   9.   Optional: BLE Advertisement Heartbeat .................  21
   9.1.   BLE Beacon Format ...................................  22
   10.  Evidence and Flag Enumerations ........................  22
   10.1.  Evidence Bits .......................................  22
   10.2.  Authorization Cap Bits ..............................  23
   11.  Implementation Checklist ..............................  24
   12.  Version Interoperability ..............................  25
   13.  Packet Examples .......................................  25
   13.1.  HELLO Frame .........................................  25
   13.2.  ASSIGN Frame ........................................  26
   13.3.  SIGHTING Frame with One Candidate ...................  27
   14.  Constant Reference ....................................  28
   15.  Known Limitations of Version 3 ........................  29
   Appendix A.  Struct Sizes ..................................  30
   Appendix B.  Simulation Conformance Suite ..................  30


Conventions Used in This Document

   The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
   "SHOULD", "SHOULD NOT", "RECOMMENDED", "MAY", and "OPTIONAL" in
   this document are to be interpreted as described in RFC 2119.

   All multi-byte integer fields are little-endian unless stated
   otherwise.  All structs are packed (no implicit padding).
   "LE" in field descriptions is shorthand for little-endian.


1.  Motivation and Changes from Version 1

1.1.  What Version 1 Got Right

   RFC-F0690 established the foundational structural decisions:
   broadcast delivery over ESP-NOW, a salted node_id for per-sender
   deduplication, variable-length bodies, five message types, and the
   privacy-first candidate abstraction (no raw BSSIDs, BLE addresses,
   or exact GPS coordinates in summaries).  These decisions are
   preserved through version 3.

1.2.  Confirmed Protocol Constraints

   NOWFLOCK compatibility cap is 250 bytes.  ESP-NOW v1.0 devices are
   limited to 250-byte vendor content.  FNOW/3 intentionally keeps
   every frame at or below 250 bytes so that mixed Espressif stacks,
   sniffing tools, and tight callback buffers can all handle the same
   wire image.

   Broadcast frames are unencrypted.  ESP-NOW CCMP encrypts paired
   unicast traffic.  Broadcast vendor-specific action frames are not
   encrypted by the ESP-NOW link layer.  Version 3 therefore uses an
   application-layer frame tag only as a group pollution and replay
   filter, not as a confidentiality mechanism.

   ESP-NOW v2.0 is not NOWFLOCK v2.  Espressif's transport version
   and this RFC's protocol version are independent.  NOWFLOCK is self-
   versioned in the FNOW header and does not rely on ESP-NOW v2.0
   payload expansion.

1.3.  Deficiencies Addressed in Version 2

   The following gaps identified in RFC-F0690 Section 13 are
   addressed in version 2:

      Gap (RFC-F0690 S.13)              v2 Fix
      --------------------------------  --------------------------------
      No frame provenance filter        4-byte keyed CRC32 frame tag
      Only 4 candidates per SIGHTING    Increased to 7 (validated 242B)
      Channel division is local only    claimed_channels in HELLO body
      No time synchronization           utc_epoch_min in ASSIGN; SYNC
      No leader election                Max-node_id bully (Section 6.2)
      Peer sightings not ingested       Peer candidate ingest (S.4.3.2)
      No battery-aware TX              battery_pct header; TX throttle
      No replay protection              Per-remote seq dedup (S.3.4)

1.4.  Changes from Version 2 to Version 3

   Version 3 preserves the v2 wire frame sizes and message types.  The
   following gaps are addressed:

      Gap (v2)                          v3 Fix
      --------------------------------  --------------------------------
      Scoring rules implicit            Normative LSP-1 (Section 10.3)
      HELLO byte 6 unused               capabilities bitfield (Section 4.1)
      SYNC adoption underspecified      Child RTC guard + drift (Section 4.6)
      Conformance without LSP tests     5 new scenarios in sim_nowflock_v3.py


2.  Prior Art and Design Influences

2.1.  IEEE 802.11s Mesh (802.11-2020 S.14)

   802.11s uses a 3-way Mesh Peering Management (MPM) handshake and
   Gate Announcement (GANN) flooding for coordinator election.
   NOWFLOCK adopts the GANN pattern for leader election: the node
   with the highest node_id wins without a dedicated election message
   round -- it promotes itself after the master timeout, equivalent to
   GANN with a deterministic metric.  802.11s's HWMP route-
   request/reply model is overkill for a single-hop, at-most-20-node
   mesh; NOWFLOCK retains flat broadcast.

2.2.  BATMAN-Adv OGM Interval Sizing

   BATMAN-Adv OGMv2: 20-byte packet, 1 s default interval, TTL = 5.
   For a 20-node mesh at 5 s HELLO interval, convergence takes 2 HELLO
   rounds (10 s), matching the 802.11s neighbor discovery window.  The
   NOWFLOCK HELLO interval of 5 s is appropriate and is not reduced in
   version 3.

2.3.  SWIM Gossip (Das et al., 2002)

   SWIM infection-style gossip convergence rounds scale as
   log(N)/log(fanout).  For N = 20, fanout = 3, that is approximately
   3 rounds.  For 2-round convergence, fanout >= 5 is needed.
   NOWFLOCK broadcast achieves fanout = N (all nodes receive every
   frame), meaning 1-round convergence at N = 20.  No gossip
   optimization is needed.  The SWIM lesson applied here: piggybacking
   candidate summaries onto periodic SIGHTING frames rather than using
   separate messages is correct, and version 3 retains this approach.

2.4.  FTSP/TPSN Time Synchronization

   FTSP (Maroti et al., 2004) achieves approximately 1 microsecond
   accuracy with MAC-layer timestamps and linear regression.  TPSN
   achieves approximately 30 microseconds with pairwise 2-message
   exchanges.  For FLOCKGRAPH's 30-second pass-bucket correlation,
   minute granularity is sufficient.  Version 3 uses a single-field
   epoch broadcast (utc_epoch_min = unix_timestamp / 60, 4 bytes) in
   ASSIGN and the new SYNC message type.  This achieves at most 60 s
   worst-case time error, which is acceptable for corroborating multi-
   node pass-bucket counts.

2.5.  BLE Advertising as a Hunt-Window Heartbeat

   During HUNT mode WiFi-only slices, NOWFLOCK's ESP-NOW TX gate
   blocks frames.  A BLE advertisement beacon carrying 6 bytes of
   status data (candidate count, exportable count, battery percentage,
   claimed channels, flags) can fill this window so peers maintain a
   presence picture during hunt blackouts.  This feature is defined in
   Section 9 as OPTIONAL.  A conforming FNOW/3 implementation MUST
   work correctly without it.

   Implementations MUST NOT treat the BLE heartbeat as proven until
   it is measured on the target firmware with radio coexistence
   enabled.

2.6.  Frame Tag Approach

   No group-broadcast MAC security exists in ESP-NOW.  FNOW/3
   therefore uses a 4-byte keyed CRC32 tag over the header identity
   fields and body.  This is deliberately not a cryptographic MAC.
   It is a cheap group pollution filter, a replay aid when combined
   with per-sender sequence deduplication, and a way to reject
   accidental foreign-flock ESP-NOW traffic.

   The stronger future profile is an 8-byte truncated HMAC-SHA256 tag
   with an explicit auth_type negotiation bit.  That profile is not
   normative in this document because the current v3 simulator and
   packet examples use the 4-byte tag.

   A 16-bit group_id field (group_key & 0xFFFF) in the HELLO body
   allows receivers to pre-filter frames from foreign groups before
   computing the full tag.

2.7.  IEEE 802.11k Neighbor Report

   The 802.11k Neighbor Report element encodes 13 bytes per AP entry:
   6 bytes BSSID + 4 bytes BSSID Info + 1 byte regulatory class +
   1 byte channel + 1 byte PHY type.  NOWFLOCK's candidate wire format
   at 30 bytes is 2.3x larger, justified by the additional scoring
   fields (site_score, evidence_bits, pair_events, pass_count) that
   enable cross-node confidence synthesis.  The 802.11k format carries
   no temporal evidence counts or GPS tile data.

2.8.  GhostESP and Existing Single-Node Tools

   GhostESP (2024-2025) and similar single-node ESP32 wardrivers
   perform passive WiFi, BLE, and GPS logging with WiGLE CSV export.
   The design gap NOWFLOCK targets is cooperative, single-hop, real-
   time passive correlation under ESP-NOW broadcast constraints.

   Pwnagotchi's peer protocol (pwngrid) uses 802.11 management frame
   vendor IEs with RSA-2048-signed protobuf payloads.  That is a
   different transport requiring no infrastructure but with higher
   implementation complexity, limited to WPA handshake sharing rather
   than site-level correlation.


3.  Frame Format

3.1.  Header -- Version 3 (24 Bytes)

   The v3 header is 24 bytes, packed, all multi-byte fields little-
   endian.  It retains the v2 header shape.  The v1 header was 20
   bytes; 4 bytes were added at offsets 18-23.

      Bytes   Field              Description
      ------  -----------------  ---------------------------------------
        0-3   magic              0x574F4E46 ("FNOW" LE).  Identifies the
                                 frame as NOWFLOCK.  MUST be this value.
          4   version            Protocol version.  MUST be 0x03.
          5   type               Message type (1-8); see Section 4.
        6-7   seq                Monotonic counter; starts at 1, wraps
                                 at 0xFFFF.  Auth nonce (Section 3.2).
       8-11   node_id            Sender identifier; see Section 5.
      12-15   uptime_ms          Sender's local uptime in milliseconds.
                                 Not wall-clock time.
         16   role               0 = CHILD, 1 = MASTER.
         17   channel            NOWFLOCK control channel (1-13).
         18   battery_pct        Battery state of charge 0-100.
                                 0xFF = unknown or not exposed.
         19   flags              Bit 0: FLAG_AUTH -- 4-byte auth tag
                                   appended after body (Section 3.2).
                                 Bit 1: FLAG_LONG_RANGE -- PHY hint.
                                 Bit 2: FLAG_RELAY -- reserved, future.
                                 Bits 3-7: reserved, MUST be zero.
         20   body_len           Body byte count, not counting auth tag.
      21-23   reserved           MUST be 0x000000.  Receivers MUST
                                 ignore this field.

3.2.  Frame Tag (Optional, 4 Bytes)

   When flags bit 0 (FLAG_AUTH) is set, a 4-byte tag is appended
   immediately after the body and before no further data:

      auth_tag = CRC32(
                   LE32(group_key XOR node_id) ||
                   LE32(seq)                   ||
                   LE32(CRC32(body))
                 )

   where:

      group_key   32-bit shared group value, distributed out of band
                  (e.g., compiled into firmware or loaded via serial
                  provisioning).  Default for lab builds: 0xDEADB4D6.
                  A group_key of zero disables tag verification.

      node_id     The 4-byte sender node_id from the header, LE.

      seq         The 16-bit header sequence field, zero-extended to
                  32 bits, LE.

      body        The exact body bytes as transmitted.

      CRC32       ISO 3309 / zlib CRC-32, result encoded LE on wire.

   Total frame length with auth tag:
      24 (header) + body_len + 4 (tag).

   This tag is NOT a cryptographic authentication mechanism.  It
   rejects accidental foreign-group traffic and naive wrong-key
   injection.  A node that knows the group_key can forge valid frames.
   See Section 15, item 1 for the known limitation.

   Receiver filter:  If FLAG_AUTH is set and group_key is nonzero,
   the receiver MUST compute the expected tag and MUST discard any
   frame whose received tag does not match, incrementing stat_auth_fail.
   If group_key is zero (unconfigured), the receiver MUST skip tag
   verification.

   Pre-filter by group_id:  Before computing the tag, a receiver MAY
   check the group_id field in a received HELLO body against its own
   (group_key & 0xFFFF).  A mismatch cheaply filters foreign-group
   frames without computing the full tag.

3.3.  Frame Validation

   A receiver MUST discard a frame and increment stat_rx_bad if any
   of the following conditions hold:

      a.  Frame length < 24.
      b.  magic != 0x574F4E46.
      c.  version != 0x03.
      d.  Frame length != 24 + body_len + (FLAG_AUTH ? 4 : 0).
      e.  node_id == 0.
      f.  node_id equals the receiver's own node_id (self-echo).
      g.  FLAG_AUTH is set, group_key is nonzero, and the auth tag
          does not match.  (Auth failures increment stat_auth_fail
          separately and do not also increment stat_rx_bad.)

   Frames that fail validation MUST be silently discarded.

3.4.  Sequence Deduplication

   To reject replayed frames, each receiver MUST maintain a per-sender
   record of the last accepted sequence number (last_seq), keyed by
   node_id.

   For a frame received from a known sender:

      delta = (incoming_seq - last_seq) & 0xFFFF
      if delta == 0 or delta > 0x7FFF:
          discard; increment stat_rx_dup
      else:
          update last_seq = incoming_seq; proceed

   For a frame from an unknown sender (first frame observed), skip the
   check and set last_seq = incoming_seq after successful dispatch.

   The sliding-window width of 0x7FFF (32767) accommodates a sender
   that transmits up to 32767 frames before the receiver sees the
   next one.  At the HELLO interval of 5 s, wrapping the full 16-bit
   range takes approximately 91 hours of continuous operation.

3.5.  Frame Size Budget

   All frame sizes are validated by scenario "frame_sizes" in
   scripts/sim_nowflock_v3.py.  Sizes include the 4-byte auth tag.

      Message type       Body (B)  Total (B)  Margin to 250 B
      -----------------  --------  ---------  ---------------
      HELLO                    13         41              209
      ASSIGN                   14         42              208
      SIGHTING (N=7, max)     214        242                8
      TARGET                    8         36              214
      CAPTURE (L=48, max)      49         77              173
      SYNC                      9         37              213
      PEER_REQ                  1         29              221
      EXPORT_SNAPSHOT (L=180) 181        209               41

   SIGHTING at maximum capacity (7 candidates) leaves 8 bytes margin.
   Implementors MUST NOT add header fields that increase frame size
   without first reducing SIGHTING_MAX_CANDIDATES to compensate.


4.  Message Types

4.1.  Type 1 -- HELLO

   Sent by all nodes.  Nominal period: 5000 ms (+-jitter).

   Body: 13 bytes, packed.

      Bytes   Field                  Description
      ------  ---------------------  -----------------------------------
         0    max_nodes              Maximum peer slots.  Default: 20.
         1    default_control_nodes  Preferred maximum children.
                                     Default: 6.
         2    encrypted_peer_limit   ESP-NOW unicast encryption limit.
                                     17 on ESP32-S3.
         3    fg_candidates          Current FLOCKGRAPH candidate count,
                                     saturated at 255.
         4    fg_exportable          Current exportable candidate count,
                                     saturated at 255.
         5    mode                   Operating mode:
                                       0 = IDLE
                                       1 = HUNT
                                       2 = MISCHIEF
                                       3 = AUTO
         6    capabilities           Node feature bits (Section 4.1.1).
                                     This byte was reserved in v2.
       7-8    claimed_channels       Bitmask of channels this node has
                                     claimed for hunting.  Bit N set
                                     means channel N is claimed.  Bit 1
                                     = channel 1; bit 13 = channel 13.
                                     Bits 0 and 14-15: unused, zero.
       9-10   group_id               group_key & 0xFFFF.  Allows a
                                     receiver to detect group mismatch
                                     before computing the full auth tag.
      11-12   report_interval_s      Current sighting report interval
                                     in seconds.

   Behavior:  The MASTER reads claimed_channels from each received
   HELLO to build per-child channel usage and to compute the
   channel_mask field in subsequent ASSIGN frames.  Receivers SHOULD
   use group_id to fast-path-discard frames from foreign groups.

4.1.1.  HELLO Capabilities Byte (New in v3)

      Bit   Mask    Name               Description
      ---   ------  -----------------  ---------------------------------
        0   0x01    CAP_LSP1           Implements LSP-1 (Section 10.3)
        1   0x02    CAP_BLE_HEARTBEAT  Optional BLE presence beacon (S.9)
        2   0x04    CAP_PIGBROTHER     EXPORT_SNAPSHOT capable
      3-7   --      Reserved           MUST be zero; receivers MUST ignore

   Transmitters implementing LSP-1 MUST set CAP_LSP1.  Receivers MUST
   ignore unknown capability bits.

4.2.  Type 2 -- ASSIGN

   Sent by MASTER nodes only.  Nominal period: 2000 ms (+-jitter).

   Body: 14 bytes, packed.

      Bytes   Field               Description
      ------  ------------------  --------------------------------------
         0    control_channel     Suggested NOWFLOCK channel (1-13).
         1    max_children        Master's preferred maximum children.
       2-3    report_interval_s   Requested sighting interval (2-60 s).
                                  Children MUST clamp to this range.
       4-5    channel_mask        Bitmask (bits 1-13) of channels
                                  available to children.
                                  0x3FFE = all channels available.
                                  Computed as:
                                    all_channels XOR
                                    union(children.claimed_channels)
       6-9    utc_epoch_min       GPS-derived Unix timestamp / 60.
                                  0 = master has no GPS UTC.  Children
                                  without GPS adopt this value for
                                  session timestamping.
      10-13   master_node_id      The sending master's node_id.  Used
                                  to detect and resolve multi-master
                                  conflicts; see Section 6.2.

   Note:  The v1 rendezvous_ms field is removed.  channel_mask shrinks
   from 4 bytes to 2 bytes (channels 1-13 fit in 13 bits).  The freed
   bytes are occupied by utc_epoch_min and master_node_id.

   Child behavior on receipt:

      1.  If hdr.node_id > own node_id, or no current master is
          known: accept as master.  Update master_node_id,
          last_master_ms, channel, and report_interval_ms.

      2.  If body.master_node_id < own node_id AND own role is
          MASTER: discard.  We outrank this sender.

      3.  If body.master_node_id > own node_id AND own role is
          MASTER: demote to CHILD.  Stop sending ASSIGN.

      4.  If utc_epoch_min > 0 and local GPS is unavailable: adopt
          utc_epoch_min as the current epoch estimate.

4.3.  Type 3 -- SIGHTING

   Sent by all nodes.  Default period: 10000 ms, overridden by ASSIGN.
   Carries up to 7 FLOCKGRAPH candidate summaries.

   Body: 4 + N*30 bytes, where N is in [0, 7].

      Bytes     Field       Description
      --------  ----------  --------------------------------------------
         0      count       Number of candidate rows (0-7).
         1      total       Total candidates in sender's local table,
                            saturated at 255.
         2      exportable  Exportable candidates in sender's table,
                            saturated at 255.
         3      reserved    Set to 0x00.  Receivers MUST ignore.
      4..end    rows        N candidate records (see Section 4.3.1).

   body_len MUST equal 4 + count * 30.

   Candidate selection (sender):  Sort all local candidates descending
   by site_score.  Include candidates where tier >= TIER_MEDIUM
   (value 2) or where the exportable flag is set.  Skip candidates
   with state DECAYED.  Take the top 7.

4.3.1.  Candidate Wire Record (30 Bytes)

   Each candidate row is 30 bytes, packed.  This is 2 bytes smaller
   than the v1 wire record (32 bytes): the separate tier, state, and
   flags bytes are replaced by a 1-byte confidence field and a 1-byte
   packed_flags field.

      Bytes   Field               Description
      ------  ------------------  --------------------------------------
       0-3    candidate_id        Sender's local salted summary ID
                                  (uint32 LE).  Opaque to receivers.
                                  MUST NOT be used as a cross-node
                                  deduplication key; see Section 4.3.2.
       4-7    tile_lat_e7         Coarse latitude tile (int32 LE).
                                  Units: 1e-7 degrees, rounded to
                                  nearest 5000-unit (approx. 50 m)
                                  grid boundary.
       8-11   tile_lon_e7         Coarse longitude tile (int32 LE),
                                  same encoding as tile_lat_e7.
      12-13   site_score          Aggregate quality score, 0-1000
                                  (uint16 LE).
      14-15   evidence_bits       Evidence flags (uint16 LE).
                                  Bit 10: PEER_CORROBORATION (v2 new).
                                  See Section 10.1.
      16-17   authorization_caps  Authorization guard flags (uint16 LE).
                                  See Section 10.2.
      18-19   wifi_events         WiFi observation count, saturated at
                                  65535 (uint16 LE).
      20-21   ble_events          BLE observation count (uint16 LE).
      22-23   pair_events         Co-time WiFi+BLE pair count (uint16).
      24-25   pass_count          Distinct 30 s pass bucket count
                                  (uint16 LE).
      26-27   age_s               Seconds since last update, capped at
                                  65535 (uint16 LE).
         28   confidence          Display confidence, 0-100.
         29   packed_flags        Bits [0-2]: tier (0-4).
                                  Bits [3-5]: state (0-4).
                                  Bit 6: gps_valid.
                                  Bit 7: exportable.

   Tier values:  0=NONE, 1=LOW, 2=MEDIUM, 3=HIGH, 4=VERY_HIGH.
   State values: 0=UNKNOWN, 1=UNLIKELY, 2=PROBABLE,
                 3=LIKELY, 4=CONFIRMED.

   Privacy:  Senders MUST NOT include raw MAC addresses, BLE
   advertisement addresses, handshake material, or coordinates more
   precise than the 50 m tile granularity in any SIGHTING frame.

4.3.2.  Peer Sighting Ingest

   Receivers MUST process received candidate rows and update their
   local FLOCKGRAPH candidate table.  Processing MUST use the tile
   coordinates as the cross-node merge key, NOT the candidate_id.
   (Two nodes observing the same physical site will derive different
   candidate_id values from their salted hash.  Tile overlap is the
   only cross-node identity signal.)

   For each candidate row in a received SIGHTING:

      1.  Compute tile_key = (tile_lat_e7, tile_lon_e7).

      2.  If tile_key matches a locally-held candidate from a
          different originating node than the sender:
          a.  Set evidence_bits |= PEER_CORROBORATION (bit 10).
          b.  Record the sending peer in the candidate's peer_seen_by
              bitmask.
          c.  If wire.confidence >= 70 AND wire.authorization_caps
              == 0: apply score boost min(60, wire.site_score / 10)
              to the local candidate's site_score (capped at 1000).

      3.  If tile_key is new (not present in the local tile index):
          create a provisional local entry from the wire data.  Set
          PEER_CORROBORATION on the new entry.

      4.  If tile_key matches a locally-held candidate that was
          originated by the same sender node (a sender reporting
          multiple candidates in the same tile): create a separate
          entry keyed by candidate_id.  Do not merge.

   Receivers MUST NOT:

      -  Set exportable = true on any peer-derived candidate.  The
         export gate requires local radio observation.
      -  Advance pair_events or pass_count from peer data.

4.4.  Type 4 -- TARGET

   Unchanged from v1.  Sent when the WARBADGER state machine selects
   a strike target.

   Body: 8 bytes, packed.

      Bytes   Field    Description
      ------  -------  -----------------------------------------------
       0-5    bssid    Target BSSID, 6 bytes, network byte order.
         6    channel  Channel on which the BSSID is operating (1-13).
         7    pad      Set to 0x00.

   body_len MUST be exactly 8.  Receivers MUST reject TARGET frames
   where channel < 1 or channel > 13.

   Privacy note:  TARGET is the only message type carrying a raw
   device identifier.  Use of FLAG_AUTH is STRONGLY RECOMMENDED for
   TARGET frames.

4.5.  Type 5 -- CAPTURE

   Structural definition unchanged from v1.  Carries an anonymized
   capture annotation of at most 48 characters.

   Body: 1 + L bytes, where L is in [0, 48].

      Bytes   Field      Description
      ------  ---------  -----------------------------------------------
         0    line_len   Length L of the annotation string.
      1..end  line       UTF-8 annotation, not NUL-terminated.

   Annotation format:  A conforming annotation MUST NOT contain raw
   BSSIDs, BLE device addresses, or GPS coordinates.  Recommended
   format for a PMKID capture annotation:

      PMKID oui=AC84C6 ch=6 conf=high

   Use of FLAG_AUTH is STRONGLY RECOMMENDED for CAPTURE frames.

   Session layer note:  Conforming implementations MAY defer CAPTURE
   session integration until a higher-layer handler is available.

4.6.  Type 6 -- SYNC (Added in v2)

   Explicit time synchronization message.  Sent by the MASTER node
   when the current GPS UTC is fresh and it wishes to push an update
   outside the ASSIGN cadence (e.g., immediately after acquiring a
   GPS fix).

   Body: 9 bytes, packed.

      Bytes   Field          Description
      ------  -------------  -------------------------------------------
       0-3    utc_epoch_min  Unix timestamp / 60 (minutes since epoch).
                             MUST be > 0 to be acted on.
       4-7    uptime_ref_ms  Sender's uptime_ms when the GPS UTC value
                             was captured.
         8    accuracy_s     Estimated UTC accuracy in seconds.
                             0 = sub-second.  Informational only.

   Receiver behavior:  If local GPS is unavailable AND local clock is
   not trusted (no NTP/GPS RTC), a receiver MUST adopt utc_epoch_min
   when accuracy_s <= 120.  It MUST NOT overwrite a trusted local GPS
   or NTP-synchronized clock.  It MAY correct for uptime drift:

      local_epoch_min ~= utc_epoch_min +
                         (local_uptime_ms - uptime_ref_ms) / 60000

   Propagation delay is typically less than 10 ms and negligible at
   minute resolution.

   Only a MASTER SHOULD send SYNC.  A receiver that receives SYNC from
   a node with hdr.role != ROLE_MASTER SHOULD discard it.

4.7.  Type 7 -- PEER_REQ (Added in v2)

   Requests that all peers immediately broadcast a SIGHTING, allowing
   rapid candidate catch-up after joining a flock in progress.

   Body: 1 byte.

      Bytes   Field     Description
      ------  --------  ------------------------------------------------
         0    min_tier  Minimum candidate tier requested (0-4).
                        Responding nodes SHOULD include only candidates
                        with tier >= min_tier.  Typical: 2 (MEDIUM).

   Receiver behavior:  If enabled and the receiver has candidates
   matching min_tier, it MUST schedule a SIGHTING within one tick,
   bypassing the normal report interval timer.

   Rate limit:  A receiver MUST NOT respond to more than one PEER_REQ
   from the same sender within HELLO_INTERVAL_MS (5000 ms).

   Battery throttle:  Nodes with battery_pct < 15 MUST NOT send
   PEER_REQ.  See Section 8.

4.8.  Type 8 -- EXPORT_SNAPSHOT (Optional, PIGBROTHER)

   EXPORT_SNAPSHOT carries one replayable wardrive export record from
   a PIGBROTHER-capable node.  This message is OPTIONAL.  A node can be
   FNOW/3 conforming without implementing it.

   Body: 1 + L bytes, where L is in [0, 180].

      Bytes   Field      Description
      ------  ---------  -----------------------------------------------
         0    line_len   Length L of the export line.
      1..end  line       UTF-8 export line, not NUL-terminated.

   The line format is implementation-selected but MUST begin with a
   profile token followed by comma-separated key=value fields:

      profile=wigle-v1,kind=wifi,ts=1783449600,
      lat_e7=523456000,lon_e7=210123000,ssid_hash=9F01A2B3,
      bssid_hash=7721CC08,channel=6,rssi=-57,auth=wpa2,
      source=9D5C0A11,pass=42

   Profiles registered by this document:

      wigle-v1     Replayable WiGLE-like WiFi/BLE wardrive record.
      wdwars-v1    Replayable wdwars.pl-style wardrive record.
      fmh-v1       FLOCKMEHARD replay fixture; same payload grammar,
                   no upload semantics implied.

   Privacy and consent:

      EXPORT_SNAPSHOT MUST be disabled by default unless the firmware
      owner explicitly chooses an upload/export profile.

      EXPORT_SNAPSHOT MUST NOT be used for covert exfiltration.  User
      interface and configuration text MUST make clear that records may
      be written to local CSV and may be uploaded by firmware-specific
      adapters.

      Raw BSSID, BLE device address, exact client MAC, credentials,
      probe payloads, and handshake material MUST NOT appear in
      EXPORT_SNAPSHOT.  Use salted hashes and coarse coordinates unless
      a local firmware deliberately writes a first-party WiGLE CSV for
      its own configured upload path outside NOWFLOCK.

   Receiver behavior:

      Receivers MAY write accepted lines to a local replay CSV.  The
      recommended filename pattern is:

         /nowflock/pigbrother/fnow_export_<session>.csv

      Receivers SHOULD add local metadata columns outside the wire line
      when writing CSV: rx_node_id, rx_rssi, rx_uptime_ms, auth_ok,
      group_id, and source_node_id.

      Receivers MUST NOT mark peer-derived export rows as locally
      observed FLOCKGRAPH candidates unless the local radio also
      observed matching evidence.  EXPORT_SNAPSHOT is for replay/export
      reconstruction, not evidence laundering.

   Firmware freedom:

      NOWFLOCK defines only the replayable line envelope.  Upload
      endpoints, HTTP methods, credentials, retry/backoff behavior,
      local CSV naming, success markers, and cleanup-after-success are
      firmware policy.  Typical adapters include WiGLE and wdwars.pl.


5.  Node Identifier

   Unchanged from v1.  Reproduced here for completeness.

      node_id = FNV1a-32(SALT_BYTES || MAC_BYTES)

   FNV-1a 32-bit parameters:

      Offset basis:  0x811C9DC5
      Prime:         0x01000193

   SALT_BYTES:  0x11, 0x0A, 0x5C, 0x9D  (the value 0x9D5C0A11 in LE)
   MAC_BYTES:   WiFi STA MAC address, 6 bytes, network byte order.

   If the result is zero, substitute 1.  node_id == 0 in a received
   frame is an error (see Section 3.3, condition e).


6.  Role Model and Coordination

6.1.  Role Definitions

   CHILD (role = 0):   Sends HELLO, SIGHTING, PEER_REQ.
   MASTER (role = 1):  Sends HELLO, ASSIGN, SIGHTING, SYNC, PEER_REQ.
   PIGBROTHER:         A capability, not a header role value.  A node
                       with this capability may additionally write and
                       broadcast EXPORT_SNAPSHOT records while in
                       wardrive/export mode.

   All nodes begin as CHILD.  Promotion to MASTER occurs automatically
   via the election algorithm in Section 6.2.

6.2.  Leader Election -- Max-Node_ID Bully

   Version 2 introduced automatic leader election; Version 3 retains
   it unchanged.  No separate election message type is needed; the
   ASSIGN message carries master_node_id, which serves as the
   authoritative tie-breaker.

   Election algorithm:

      1.  Every node tracks last_master_ms, the time of the most
          recently received valid ASSIGN frame.

      2.  If (now_ms - last_master_ms) > MASTER_TIMEOUT_MS (12000)
          AND node_id == max(own node_id, all active remote node_ids):
          promote to MASTER.

      3.  Begin sending ASSIGN with master_node_id = own node_id.

      4.  On receiving ASSIGN where body.master_node_id > own node_id
          AND own role is MASTER: demote to CHILD.  Update
          master_node_id and last_master_ms.

      5.  On receiving ASSIGN where body.master_node_id < own node_id
          AND own role is MASTER: discard the ASSIGN channel/interval
          fields.  We outrank this sender.

   Rationale for max node_id:  Node IDs are derived from MAC addresses
   via FNV-1a -- uniformly distributed, with ties impossible.  The
   winner is deterministic without additional election rounds.  This
   matches IEEE 802.11s's GANN coordinator election metric (scalar
   comparison, highest metric wins).

   Simulation result:  In a 4-node flock (1 master + 3 children),
   after the master goes silent, the correct node (highest node_id)
   elects itself within 11.4 s of master silence -- within one
   MASTER_TIMEOUT (12 s) plus one HELLO round.

   Stability guard:  After promotion, a node SHOULD wait
   ASSIGN_INTERVAL_MS * 3 (6000 ms) before accepting demotion, to
   prevent thrashing when a peer appears briefly and then disappears.

6.3.  Channel Division

   The MASTER SHOULD compute channel_mask for ASSIGN as follows:

      peer_claimed = bitwise OR of claimed_channels from all children
                     seen in the most recent REMOTE_ACTIVE_MS window
      available    = 0x3FFE & ~peer_claimed   (channels 1-13,
                                               exclude all claimed)
      if available == 0:
          available = 0x3FFE                  (fallback: all channels)

   Children SHOULD restrict active PMKID sweeps to channels where
   their own claimed_channels overlaps with the assigned channel_mask.
   Children MUST NOT restrict passive WiFi scanning -- passive scan
   continues on all channels; only active mischief sweep is bounded.

   Simulation result:  With 3 children claiming channels 1, 6, and 11
   respectively, the master sees the full claimed union within one
   HELLO interval (5 s) and broadcasts the correct exclusion mask.

6.4.  Sighting Interval Negotiation

   Same as v1.  Children clamp the received report_interval_s to
   [2, 60], then multiply by 1000 ms.  Default: 10 s.


7.  Timing Reference

   All values in milliseconds:

      HELLO_INTERVAL_MS      5000   All nodes; subject to jitter.
      ASSIGN_INTERVAL_MS     2000   MASTER only; subject to jitter.
      SIGHTING_DEFAULT_MS   10000   Overridden by ASSIGN.
      SIGHTING_MIN_MS        2000   Floor after ASSIGN clamp.
      SIGHTING_MAX_MS       60000   Ceiling after ASSIGN clamp.
      REMOTE_ACTIVE_MS      15000   Peer considered active.
      MASTER_TIMEOUT_MS     12000   Election trigger in CHILD.
      PEER_REQ_RATE_MS       5000   Per-sender response rate limit.
      ELECTION_GUARD_MS      6000   Post-promotion demotion holdoff.

   Transmit jitter (RECOMMENDED):  Implementations SHOULD apply
   plus-or-minus J% random jitter to each transmit interval (HELLO,
   ASSIGN, SIGHTING).  Recommended J = 20.  This reduces timing-
   fingerprint detectability.  J = 40 reduces detectability by at
   least 0.40.  See scenario jitter_reduces_timing_detectability.

   Jitter formula:  next_delay = nominal + randint(-nominal*J/100,
   +nominal*J/100), clamped to [nominal/2, nominal*2].

   Boot-time stagger (RECOMMENDED):  Implementations SHOULD stagger
   the first transmit to avoid synchronous-boot collision, where all
   nodes fire in the same tick.  Recommended stagger:

      first_hello_delay = (node_id & 0xFF) * (HELLO_INTERVAL_MS / 256)

   Validated by scenario boot_jitter_collision: synchronous boot
   produces a collision peak of 6; staggered boot produces a peak
   of 1.


8.  Battery-Aware Transmission Policy

   The battery_pct header field (0-100; 0xFF = unknown) governs local
   TX throttling.  The following rules MUST be applied:

      battery_pct      Permitted TX types
      ---------------  -------------------------------------------------
      >= 30%           All: HELLO, ASSIGN, SIGHTING, TARGET, CAPTURE,
                        SYNC
      15% to 29%       HELLO and ASSIGN only
      < 15%            HELLO only
      0xFF (unknown)   No throttling applied

   A MASTER with battery_pct 15-29% continues sending ASSIGN because
   ASSIGN is essential for child coordination.  It suppresses SIGHTING
   and SYNC to conserve power during burst transmissions.

   Implementations MUST NOT cache battery_pct for more than one HELLO
   interval.  If no reliable state of charge is available, the
   implementation MUST set battery_pct = 0xFF.


9.  Optional: BLE Advertisement Heartbeat

   During HUNT mode WiFi-only slices, the radio_window_allows_send()
   gate blocks ESP-NOW TX.  This creates coordination gaps of up to
   several seconds between peers.  A BLE5 extended non-connectable
   undirected advertisement can fill this gap.

   This section is OPTIONAL.  A NOWFLOCK v3 conforming implementation
   MUST support ESP-NOW-based coordination and MAY additionally
   implement BLE heartbeat beacons.

9.1.  BLE Beacon Format

   Use a non-connectable, non-scannable BLE advertisement.  The
   manufacturer-specific AD element carries 6 bytes of NOWFLOCK
   status:

      AD Length:  0x09 (9 bytes following this length byte)
      AD Type:    0xFF (Manufacturer Specific Data)
      Company ID: Deployment-specific.  Lab builds MAY use 0xFFFF
                  ONLY when the advertisement is confined to
                  controlled test environments.
      Data (6 B): Byte 0: candidate_count (0-255)
                  Byte 1: exportable_count (0-255)
                  Bytes 2-3: claimed_channels, LE uint16
                  Byte 4: battery_pct (0-100; 0xFF = unknown)
                  Byte 5: flags
                            bit 0: flock_active
                            bit 1: own role is MASTER
                            bits 2-7: reserved, zero

   Privacy:  The BLE beacon MUST NOT contain the node_id, any BSSID,
   any BLE device address, or any GPS data.

   Advertising interval:  Start at 1000 ms.  Implementations MAY
   lower the interval after bench validation on the target firmware
   confirms that WiFi scan/capture quality is not degraded.

   Reception:  Peer nodes passively receive these beacons during their
   own BLE windows.  Receivers MAY update the remote node table using
   fg_candidates, battery_pct, and claimed_channels extracted from the
   beacon payload.

   ESP32-S3 feasibility note:  Set BLE scan window < BLE scan interval
   to avoid WiFi starvation.  Disable the beacon automatically if HUNT
   capture quality regresses under measurement.


10.  Evidence and Flag Enumerations

10.1.  Evidence Bits (evidence_bits, uint16 LE)

   Bits 0-9 are unchanged from v1.  Bit 10 was added in v2.

      Bit   Mask    Name                  Description
      ---   ------  --------------------  ------------------------------
        0   0x0001  WIFI_FAMILY           Recognized WiFi OUI or family
        1   0x0002  WIFI_STRONG           SSID keyword or advanced probe
        2   0x0004  WIFI_REPEAT           WiFi seen >= 2 times
        3   0x0008  BLE_FAMILY            Recognized BLE vendor or UUID
        4   0x0010  BLE_STRONG            BLE name or service UUID match
        5   0x0020  BLE_REPEAT            BLE seen >= 2 times
        6   0x0040  COTILE                WiFi and BLE in same GPS tile
        7   0x0080  COTIME                WiFi and BLE within 5000 ms
        8   0x0100  REPEAT_PASS           Seen across >= 2 pass buckets
        9   0x0200  SITE_COHERENT         GPS valid, <= 75 m accuracy,
                                          both radios present
       10   0x0400  PEER_CORROBORATION    At least one peer reported a
                    (added in v2)          candidate at the same GPS tile
      11+   --      Reserved              MUST be zero; receivers MUST
                                          NOT interpret

10.2.  Authorization Cap Bits (authorization_caps, uint16 LE)

   Unchanged from v1.  Reproduced for completeness.

      Bit   Mask    Name               Description
      ---   ------  -----------------  ---------------------------------
        0   0x0001  NO_GPS             No GPS fix at observation time
        1   0x0002  STALE_GPS          GPS accuracy > 100 m
        2   0x0004  SINGLE_MEDIUM      Only one radio type has events
        3   0x0008  SINGLE_WEAK_CLUE   Single weak evidence clue only
        4   0x0010  RANDOMIZED_BLE     BLE address is randomized
        5   0x0020  NO_PAIR            No WiFi+BLE co-time pair event
        6   0x0040  STALE              Not refreshed within 15 s
        7   0x0080  NO_UTC             No GPS UTC timestamp
       8+   --      Reserved           MUST be zero

   A candidate meets the LSP-1 export quality gate when
   authorization_caps == 0 and packed_flags bit 7 (exportable) is 1.
   See Section 10.3 for LSP-1 export gate rules.

10.3.  Local Scoring Profile LSP-1 (New in v3)

   LSP-1 defines how nodes build local FLOCKGRAPH candidates before
   encoding SIGHTING rows.  Conformance reference implementations
   (MUST track this section when it changes):
   scripts/nowflock_lsp.py, scripts/sim_nowflock_v3.py,
   tools/sim_nowflock_visual.html.

   Tile quantizer (real GPS, int32 1e-7 degrees):

      tile_e7 = ((coord_e7 + 2500) / 5000) * 5000

   candidate_id (local opaque id):

      FNV1a-32(SALT_LSP || tile_lat_e7 || tile_lon_e7 ||
               wifi_family_hash || ble_family_hash)

   Merge rules per tile (wifi and/or ble observation):

      - Increment wifi_events or ble_events on new medium sighting.
      - pass_count increments when pass bucket (30 s) changes and
        last_seen gap > 4 s; set EVID_REPEAT_PASS when pass_count >= 2.
      - When both radios seen within 5 s: increment pair_events,
        set EVID_COTILE|COTIME|SITE_COHERENT, clear SINGLE_MEDIUM
        and NO_PAIR caps.
      - site_score clamped 320-980 (540 when caps set).
      - confidence capped at 69 when caps set; export requires >= 75.
      - tier/state in packed_flags derived from confidence, not score.

   Export gate:

      exportable = (authorization_caps == 0) && (pair_events > 0) &&
                   (pass_count >= 2) && (confidence >= 75)

   Decay: no refresh for 120 s -> STATE_DECAYED (5), excluded from TX.


11.  Implementation Checklist

   Minimum conforming FNOW/3 peer:

      [ ] Header is exactly 24 bytes (Section 3.1).
      [ ] version field = 0x03 in every transmitted frame.
      [ ] Discard received frames with version != 0x03 (Section 3.3).
      [ ] battery_pct field populated in every transmitted frame header;
          set to 0xFF if state of charge is unavailable.
      [ ] HELLO body is exactly 13 bytes; capabilities, claimed_channels
          and group_id fields are populated.
      [ ] ASSIGN body is exactly 14 bytes; utc_epoch_min is read and
          applied for time synchronization when GPS is unavailable.
      [ ] SIGHTING body_len == 4 + count * 30; received candidates are
          ingested per Section 4.3.2.
      [ ] EXPORT_SNAPSHOT, when implemented, is opt-in and validates
          line_len <= 180 before any CSV write.
      [ ] Auth tag: compute and verify 4-byte keyed CRC32 when
          group_key is nonzero (Section 3.2).
      [ ] Sequence deduplication: maintain per-remote last_seq and
          reject replays (Section 3.4).
      [ ] All multi-byte fields transmitted little-endian.
      [ ] node_id computed per Section 5 with the specified salt.
      [ ] Discard received frames with node_id == 0 or own node_id.
      [ ] Battery TX throttle applied per Section 8.
      [ ] Leader election implemented per Section 6.2.

   Sending SIGHTING:

      [ ] PEER_CORROBORATION bit included in evidence_bits when set.
      [ ] Top 7 candidates selected by site_score; tier >= MEDIUM or
          exportable; DECAYED candidates excluded.
      [ ] Candidate wire records are 30 bytes each.
      [ ] No raw MAC addresses, BLE addresses, or sub-50 m GPS
          coordinates in any candidate row.

   Optional PIGBROTHER export:

      [ ] User-visible configuration enables/disables the role.
      [ ] Export profile selected explicitly (e.g., wigle-v1,
          wdwars-v1, fmh-v1).
      [ ] Local CSV write path is bounded and tolerant of SD failure.
      [ ] Upload adapters are firmware-defined, not required by this
          RFC.
      [ ] Cleanup-after-success is a local policy toggle and only runs
          after the adapter reports durable success.
      [ ] FLOCKMEHARD replay consumes the same EXPORT_SNAPSHOT payloads
          emitted by live wardrive mode.

   Optional BLE heartbeat:

      [ ] Non-connectable BLE advertising at 1000 ms starting interval.
      [ ] 6-byte NOWFLOCK status payload inside manufacturer-specific
          AD element per Section 9.1.
      [ ] No node identifier or device address in BLE payload.


12.  Version Interoperability

   NOWFLOCK v1 receivers reject v2/v3 frames on HELLO body length
   checks (v1 expects 8-byte HELLO, v2/v3 send 13-byte HELLO).
   NOWFLOCK v2 receivers reject v3 frames because version is 0x03.
   There is no automatic fallback.  The three versions are
   intentionally incompatible.

   To support mixed flocks during a firmware migration:

      1.  Detect peer version from received HELLO version field.

      2.  A v3 node MAY additionally transmit v2-format HELLO frames
          (version = 0x02, 13-byte body with capabilities byte set to
          zero) to remain discoverable by v2 nodes.  It MAY also
          transmit v1-format HELLO frames (version = 0x01, 8-byte body)
          for a planned v1 migration window.

      3.  V3 nodes SHOULD NOT send v1 or v2 SIGHTING frames.  The v1
          candidate wire format (32 bytes) differs from v2/v3 (30
          bytes), and v2 lacks the normative LSP-1/capability contract
          required by this document.

   Mixed-version flocks are a transitional operational concern.  Full
   v3 deployment is the stable target.


13.  Packet Examples

13.1.  HELLO Frame (41 bytes with auth tag)

   Node 0x9D5C0A11, role CHILD, channel 6, battery 87%, uptime
   60012 ms, seq 1, 7 candidates, 2 exportable, claimed ch6,
   group_id 0xB4D6, report interval 10 s.

      Header (24 bytes):
        46 4E 4F 57  magic
        03           version = 3
        01           type = HELLO (1)
        01 00        seq = 1
        11 0A 5C 9D  node_id
        6C EA 00 00  uptime_ms = 60012 (0x0000EA6C LE)
        00           role = CHILD
        06           channel = 6
        57           battery_pct = 87
        01           flags = 0x01 (FLAG_AUTH)
        0D           body_len = 13
        00 00 00     reserved

      Body (13 bytes):
        14           max_nodes = 20
        06           default_control_nodes = 6
        11           encrypted_peer_limit = 17
        07           fg_candidates = 7
        02           fg_exportable = 2
        01           mode = HUNT
        01           capabilities = CAP_LSP1
        40 00        claimed_channels = 0x0040 (bit 6 = ch6)
        D6 B4        group_id = 0xB4D6
        0A 00        report_interval_s = 10

      Auth tag (4 bytes):
        CRC32(LE32(group_key XOR node_id) || LE32(seq) ||
              LE32(CRC32(body)))

13.2.  ASSIGN Frame (42 bytes with auth tag)

   Master 0x103FD6B4, channel 6, all channels available, no GPS UTC,
   interval 10 s.

      Header (24 bytes):
        46 4E 4F 57  magic
        03           version = 3
        02           type = ASSIGN (2)
        05 00        seq = 5
        B4 D6 3F 10  node_id (master)
        [uptime_ms]  local uptime
        01           role = MASTER
        06           channel = 6
        64           battery_pct = 100
        01           flags = FLAG_AUTH
        0E           body_len = 14
        00 00 00     reserved

      Body (14 bytes):
        06           control_channel = 6
        06           max_children = 6
        0A 00        report_interval_s = 10
        FE 3F        channel_mask = 0x3FFE (all channels 1-13)
        00 00 00 00  utc_epoch_min = 0 (no GPS)
        B4 D6 3F 10  master_node_id = 0x103FD6B4

      Auth tag (4 bytes): as above

13.3.  SIGHTING Frame with One Candidate (62 bytes with auth tag)

   Candidate 0x7B13C2AA, tile lat=29400000, lon=12000000, score=730,
   confidence=72, tier=MEDIUM, state=PROBABLE, gps=true.

      Header (24 bytes):
        46 4E 4F 57  magic
        03           version = 3
        03           type = SIGHTING (3)
        [seq, node_id, uptime_ms, role, channel, battery, flags]
        22           body_len = 34

      Body (34 bytes = 4 header + 1*30 candidate):
        01           count = 1
        09           total candidates in table = 9
        02           exportable = 2
        00           reserved

        Candidate row (30 bytes):
          AA C2 13 7B  candidate_id = 0x7B13C2AA
          00 56 BF 01  tile_lat_e7 = 29400064 (rounded tile)
          00 C0 B6 00  tile_lon_e7 = 12000000
          DA 02        site_score = 730
          DF 01        evidence_bits = 0x01DF
                         (WIFI_FAMILY | WIFI_STRONG | WIFI_REPEAT |
                          BLE_FAMILY  | BLE_STRONG  | BLE_REPEAT  |
                          COTILE | COTIME | SITE_COHERENT)
          00 00        authorization_caps = 0x0000
          02 00        wifi_events = 2
          01 00        ble_events = 1
          01 00        pair_events = 1
          01 00        pass_count = 1
          1E 00        age_s = 30
          48           confidence = 72
          52           packed_flags = 0x52 = 0b01010010
                         bits[0-2] = 2 (tier MEDIUM)
                         bits[3-5] = 2 (state PROBABLE) -> 2<<3 = 16
                         bit 6 = 1 (gps_valid) -> 64
                         bit 7 = 0 (not exportable)
                         total: 2 + 16 + 64 = 82 = 0x52

      Auth tag (4 bytes): as above


14.  Constant Reference

      Constant                  Value          Change from v1
      ------------------------  -------------  -------------------------
      FNOW_MAGIC                0x574F4E46     unchanged
      FNOW_VERSION              0x03           was 0x02 in v2
      FNOW_LOCAL_SALT           0x9D5C0A11     unchanged
      FNOW_REMOTE_MAX           20             unchanged
      FNOW_SIGHTING_MAX_CANDS   7              was 4
      FNOW_DEFAULT_CHANNEL      6              unchanged
      FNOW_DEFAULT_CHILDREN     6              unchanged
      FNOW_HELLO_MS             5000           unchanged
      FNOW_ASSIGN_MS            2000           unchanged
      FNOW_SIGHTING_MS          10000          unchanged
      FNOW_REMOTE_ACTIVE_MS     15000          unchanged
      FNOW_MASTER_TIMEOUT_MS    12000          unchanged
      FNOW_ELECTION_GUARD_MS    6000           new
      FNOW_PEER_REQ_RATE_MS     5000           new
      FNOW_EXPORT_LINE_MAX      180            new
      FNOW_CAPTURE_LINE_MAX     48             unchanged
      FNOW_DEFAULT_GROUP_KEY    0xDEADB4D6     new; update for prod
      FNOW_AUTH_TAG_SIZE        4              was 0 (none)
      HEADER_SIZE               24             was 20 in v1
      CANDIDATE_WIRE_SIZE       30             was 32
      FLAG_AUTH                 0x01           new
      FLAG_LONG_RANGE           0x02           new
      EVID_PEER_CORROBORATION   0x0400         new (bit 10)
      BATTERY_THROTTLE_LOW      15             new
      BATTERY_THROTTLE_MED      30             new
      Tile granularity          5000           ~50 m, units of 1e-7 deg
      FNV-1a basis              0x811C9DC5     unchanged
      FNV-1a prime              0x01000193     unchanged
      ESP-NOW enc peer max      17 (ESP32-S3)  clarified
      BLE beacon interval       1000 ms        new (optional)


15.  Known Limitations of Version 3

   1.  Auth is advisory, not cryptographic.  The 4-byte keyed CRC32
       tag stops accidental group pollution and wrong-key test frames.
       A node that knows the group_key can forge valid frames.  An
       attacker with ESP-NOW sniffing capability can replay captured
       frames until (node_id, seq) deduplication catches them.  No
       confidentiality is provided.  Candidate summaries are privacy-
       safe by construction (no raw identifiers); TARGET is the
       raw-BSSID exception.  See scenario attack_valid_key_injection.

   2.  Single-hop only.  No multi-hop relay in v3.  ESP-NOW broadcast
       range is approximately 100-500 m depending on environment.
       Multi-kilometer coordination would require a hardware addition
       (e.g., UART-attached LoRa on the Grove port, replacing GPS).
       That path is out of scope for this document.

   3.  No encrypted group key distribution.  The group_key MUST be
       pre-provisioned (compiled in, or loaded via the "wb sig"
       serial command).  There is no in-band key exchange.  Key
       rotation requires reflash or explicit serial provisioning.

   4.  BLE heartbeat MUST be benchmarked before deployment.  This
       document does not mandate BLE advertising during HUNT.
       Disable it in high-precision scan modes or whenever
       coexistence measurements show capture degradation.

   5.  Leader election requires at least one peer to be known.
       A node that powers on into an empty flock with no received
       HELLOs will see its own node_id as the maximum and immediately
       promote to MASTER.  This is correct behavior: it becomes master
       of a 1-node flock.  When the first peer appears, election
       resolves correctly.  No race is possible because node_ids are
       deterministic and comparison is transitive.

   6.  Time synchronization is minute-granularity only.  Sub-minute
       pass-bucket correlation across nodes is not possible without
       hardware-level timestamp synchronization (FTSP or TPSN).  For
       FLOCKGRAPH's 30 s pass bucket, cross-node corroboration has a
       +-1 bucket error at worst.  This does not affect the export
       gate because pass_count is local only (Section 4.3.2 prohibits
       advancing it from peer data).


Appendix A.  Struct Sizes (v3)

      Struct                          Size (bytes)  Change from v1
      ------------------------------  ------------  ---------------
      flock_now_hdr_t (v3)                      24  +4
      flock_now_hello_body_t (v3)               13  +5
      flock_now_assign_body_t (v3)              14  +2
      flock_now_candidate_wire_t (v3)           30  -2
      flock_now_target_body_t                    8  unchanged
      flock_now_capture_body_t              1 + L   unchanged
      flock_now_sync_body_t (new)                9  new
      flock_now_peer_req_body_t (new)            1  new
      Auth tag                                   4  new

   SIGHTING body: 4 + N*30 bytes, N in [0, 7].  Maximum: 214 bytes.


Appendix B.  Simulation Conformance Suite

   The authoritative conformance reference is:

      python scripts/sim_nowflock_v3.py --scenario all

   All 34 scenarios MUST pass.

      Scenario                          What It Tests
      --------------------------------  --------------------------------
      frame_sizes                       All types <= 250 B with auth tag
      convergence_3node                 3 nodes converge within 2 HELLOs
      channel_division                  Master excludes claimed channels
      leader_election                   Highest node_id wins on timeout
      sighting_propagation              Candidate spreads to 5 nodes
      time_sync                         GPS master UTC propagates to
                                        GPS-less children
      sync_message                      SYNC adoption, local-GPS guard,
                                        battery gate, and role rejection
      battery_throttle                  Critical battery suppresses
                                        SIGHTING
      auth_filter                       Wrong key rejected; correct
                                        key accepted
      peer_corroboration                Same tile from 2 nodes sets
                                        EVID_PEER_CORROBORATION
      detect_passive_fingerprint        Passive observer IDs nodes from
                                        FNOW magic + node_id
      detect_swarm_correlation          3+ co-present nodes: swarm
                                        detected
      detect_timing_analysis            No-jitter detectability > 0.80
      attack_injection_rejected         Wrong-key ASSIGN rejected
      chaff_disrupts_detection          Probe flood: 0 frames in silence
      jitter_reduces_timing_            40% jitter drops detectability
        detectability                   score by at least 0.40
      stale_peer_expiry                 Peer removed after
                                        REMOTE_ACTIVE_MS
      multi_master_conflict             Lower node_id master demotes to
                                        CHILD
      packet_loss_resilience            4 nodes converge at 20% loss
      sighting_score_ordering           Top-7 selected; lower excluded
      corroboration_export_gate         Peer data never sets exportable
      peer_req_catch_up                 Latecomer syncs via PEER_REQ
      peer_req_rate_limit               3 rapid PEER_REQs: at most 1
                                        SIGHTING response
      chaff_then_resume                 Node resumes after chaff expires
      attack_valid_key_injection        Known limit: valid-key injection
                                        wins (see S.15, item 1)
      tile_corroboration                Different CIDs at same tile
                                        corroborate via tile-based merge
      seq_dedup                         Replayed (node_id, seq) dropped
      boot_jitter_collision             Sync boot peak=6; stagger peak=1
      pigbrother_export_snapshot        EXPORT_SNAPSHOT is bounded,
                                        replayable, and battery gated
      lsp_tile_quantizer                Tile quantizer matches LSP-1
      lsp_wifi_ble_pair                 WiFi+BLE merge clears caps
      lsp_export_gate                   Export gate requires pair/pass/conf
      lsp_decay                         120 s stale -> DECAYED excluded
      sync_rtc_adopt                    SYNC drift adoption on GPS-less child
