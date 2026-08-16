/** Focused Wi-Fi acquisition producer. No fusion or consumer-table access. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_wifi_types.h>

#include "defense_contracts.h"

namespace WifiObservationProducer {

size_t collectActiveScan(Defense::WifiObservation* out, size_t capacity,
                         int resultCount, uint32_t timestampMs);
size_t collectWardrive(const wifi_ap_record_t* records, uint16_t resultCount,
                       Defense::WifiObservation* out, size_t capacity,
                       uint32_t timestampMs);

}  // namespace WifiObservationProducer
