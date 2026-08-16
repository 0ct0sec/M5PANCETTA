#pragma once

#include <stddef.h>
#include <stdint.h>

namespace SpectrumRsnMath {

// Information-element identifiers and suite selectors used by the parser.
static constexpr uint8_t kRsnElementId = 0x30u;
static constexpr uint8_t kVendorElementId = 0xDDu;
static constexpr uint8_t kIeeeRsnOui[3] = {0x00u, 0x0Fu, 0xACu};
static constexpr uint8_t kLegacyWpaSelector[4] = {
    0x00u, 0x50u, 0xF2u, 0x01u
};
static constexpr uint16_t kRsnVersion = 1u;
static constexpr uint16_t kMaxSuiteCount = 8u;

enum class AuthKind : uint8_t {
    UNKNOWN = 0,
    OPEN,
    WEP,
    WPA1_PSK,
    WPA2_PSK,
    WPA_WPA2_PSK,
    WPA2_ENTERPRISE,
    WPA3_SAE,
    WPA2_WPA3_TRANSITION
};

struct ParseResult {
    AuthKind auth = AuthKind::UNKNOWN;
    bool pmfRequired = false;
    bool pmfCapable = false;
    bool valid = false;
};

struct SecurityEvidence {
    bool privacy = false;
    bool sawRsn = false;
    bool sawLegacyWpa = false;
    ParseResult rsn{};
};

static inline bool hasBytes(size_t len, size_t offset, size_t count) {
    return offset <= len && count <= len - offset;
}

static inline bool readLe16(const uint8_t* data, size_t len,
                            size_t offset, uint16_t& value) {
    if (!data || !hasBytes(len, offset, 2u)) return false;
    value = static_cast<uint16_t>(data[offset]) |
            static_cast<uint16_t>(
                static_cast<uint16_t>(data[offset + 1u]) << 8u);
    return true;
}

static inline bool matchesBytes(const uint8_t* data, size_t len,
                                const uint8_t* expected,
                                size_t expectedLen) {
    if (!data || !expected || len < expectedLen) return false;
    for (size_t i = 0u; i < expectedLen; ++i) {
        if (data[i] != expected[i]) return false;
    }
    return true;
}

static inline bool isIeeeRsnSuite(const uint8_t* suite, size_t remaining) {
    return matchesBytes(suite, remaining, kIeeeRsnOui,
                        sizeof(kIeeeRsnOui));
}

// Parse the body of an RSN information element (the bytes after id/length).
// Unknown secure AKMs remain UNKNOWN rather than being mislabeled as PSK.
static inline ParseResult parse(const uint8_t* rsn, size_t rsnLen) {
    ParseResult out{};
    if (!rsn || rsnLen < 8u) return out;

    uint16_t version = 0u;
    if (!readLe16(rsn, rsnLen, 0u, version) || version != kRsnVersion) {
        return out;
    }

    size_t offset = 6u;  // version(2) + group cipher suite(4)
    uint16_t pairwiseCount = 0u;
    if (!readLe16(rsn, rsnLen, offset, pairwiseCount) ||
        pairwiseCount > kMaxSuiteCount) {
        return out;
    }
    offset += 2u;

    const size_t pairwiseBytes =
        static_cast<size_t>(pairwiseCount) * 4u;
    if (!hasBytes(rsnLen, offset, pairwiseBytes)) return out;
    offset += pairwiseBytes;

    uint16_t akmCount = 0u;
    if (!readLe16(rsn, rsnLen, offset, akmCount) ||
        akmCount == 0u || akmCount > kMaxSuiteCount) {
        return out;
    }
    offset += 2u;

    const size_t akmBytes = static_cast<size_t>(akmCount) * 4u;
    if (!hasBytes(rsnLen, offset, akmBytes)) return out;

    bool hasPsk = false;
    bool hasSae = false;
    bool hasEnterprise = false;
    for (uint16_t i = 0u; i < akmCount; ++i, offset += 4u) {
        if (!isIeeeRsnSuite(rsn + offset, rsnLen - offset)) continue;

        switch (rsn[offset + 3u]) {
            case 1u:   // IEEE 802.1X
            case 3u:   // FT IEEE 802.1X
            case 5u:   // IEEE 802.1X SHA-256
            case 11u:  // IEEE 802.1X Suite-B
            case 12u:  // IEEE 802.1X Suite-B-192
            case 13u:  // FT IEEE 802.1X SHA-384
                hasEnterprise = true;
                break;
            case 2u:  // PSK
            case 4u:  // FT-PSK
            case 6u:  // PSK SHA-256 (not SAE)
                hasPsk = true;
                break;
            case 8u:  // SAE
            case 9u:  // FT-SAE
                hasSae = true;
                break;
            default:
                break;
        }
    }

    uint16_t capabilities = 0u;
    if (readLe16(rsn, rsnLen, offset, capabilities)) {
        // IEEE RSN Capabilities: MFPR is bit 6, MFPC is bit 7.
        out.pmfRequired = (capabilities & 0x0040u) != 0u;
        out.pmfCapable = (capabilities & 0x0080u) != 0u;
    }

    if (hasPsk && hasSae) {
        out.auth = AuthKind::WPA2_WPA3_TRANSITION;
    } else if (hasSae) {
        out.auth = AuthKind::WPA3_SAE;
    } else if (hasPsk) {
        out.auth = AuthKind::WPA2_PSK;
    } else if (hasEnterprise) {
        out.auth = AuthKind::WPA2_ENTERPRISE;
    }

    // A syntactically valid RSN IE with an unsupported AKM remains UNKNOWN.
    out.valid = true;
    return out;
}

static inline void observe(SecurityEvidence& evidence, uint8_t ieType,
                           const uint8_t* ieData, size_t ieLen) {
    if (ieType == kRsnElementId) {
        evidence.sawRsn = true;
        const ParseResult candidate = parse(ieData, ieLen);
        if (candidate.valid || !evidence.rsn.valid) {
            evidence.rsn = candidate;
        }
        return;
    }

    if (ieType == kVendorElementId &&
        matchesBytes(ieData, ieLen, kLegacyWpaSelector,
                     sizeof(kLegacyWpaSelector))) {
        evidence.sawLegacyWpa = true;
    }
}

// Fold all security evidence from one beacon/probe response into a truthful
// class. Privacy distinguishes legacy WEP from an open BSS. Malformed or
// unsupported RSN stays UNKNOWN rather than looking attackable.
static inline AuthKind classify(bool privacy, bool sawLegacyWpa,
                                bool sawRsn, const ParseResult& rsn) {
    if (sawRsn) {
        if (!rsn.valid) return AuthKind::UNKNOWN;
        if (rsn.auth == AuthKind::WPA2_PSK && sawLegacyWpa) {
            return AuthKind::WPA_WPA2_PSK;
        }
        return rsn.auth;
    }
    if (sawLegacyWpa) return AuthKind::WPA1_PSK;
    return privacy ? AuthKind::WEP : AuthKind::OPEN;
}

static inline AuthKind classify(const SecurityEvidence& evidence) {
    return classify(evidence.privacy, evidence.sawLegacyWpa,
                    evidence.sawRsn, evidence.rsn);
}

// Stable comparison key for evil-twin heuristics. This describes a security
// profile; it is not proof that either BSSID is malicious.
static inline int16_t securityProfile(AuthKind auth, bool pmfRequired) {
    if (auth == AuthKind::UNKNOWN) return -1;
    return static_cast<int16_t>(static_cast<int16_t>(auth) * 2 +
                                (pmfRequired ? 1 : 0));
}

}  // namespace SpectrumRsnMath
