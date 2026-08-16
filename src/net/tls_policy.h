/**
 * TLS policy for the two fixed cloud services.
 *
 * ==[ TRUST NO HOTSPOT ]== credentials only leave after the clock is trusted.
 */
#pragma once

#include <WiFiClientSecure.h>

namespace TlsPolicy {

    // Official WiGLE API (ISRG Root X1).
    bool configureWigle(WiFiClientSecure& client);

    // Official WPA-SEC endpoint (GTS Root R4). Custom endpoints keep
    // their legacy caller-trusted behavior because their CA is unknowable here.
    bool configureWpaSec(WiFiClientSecure& client, const char* url);
}
