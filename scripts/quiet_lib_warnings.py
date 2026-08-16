#!/usr/bin/env python3
"""Silence third-party #warning noise we can't act on.

FastLED 3.10.x ships an ESP32-S3 parallel clockless I2S LED driver
(third_party/yves/I2SClockLessLedDriveresp32s3) that needs esp_memory_utils.h,
an ESP-IDF 5 header. This project builds on Arduino-ESP32 2.x (ESP-IDF 4.4),
so FastLED's FL_HAS_INCLUDE guard compiles the driver out and emits

    #warning "esp_memory_utils.h is not available, are you on esp-idf 4? ..."

on every S3 build. We drive no parallel LED strips, so the driver's absence is
the intended outcome and the message is pure noise in a file we don't own.

GCC files #warning under -Wcpp. Scoping -Wno-cpp to the offending library keeps
our own #warning directives loud everywhere else.
"""

Import("env")

# Libraries whose #warning output is known-benign and out of our control.
QUIET_LIBS = {"FastLED"}

for lb in env.GetLibBuilders():
    if any(name in lb.name for name in QUIET_LIBS):
        lb.env.Append(CCFLAGS=["-Wno-cpp"])
        print(f"[HAMLET] {lb.name}: -Wno-cpp (third-party #warning silenced)")
