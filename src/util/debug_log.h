#pragma once

// ==[ DEBUG LOG ]== production builds stay quiet. flip -DHAMLET_DEBUG_LOG=1 to talk.
#ifndef HAMLET_DEBUG_LOG
#define HAMLET_DEBUG_LOG 0
#endif

#if HAMLET_DEBUG_LOG
#include <Arduino.h>
#define HAMLET_LOGF(...) Serial.printf(__VA_ARGS__)
#define HAMLET_LOGLN(msg) Serial.println(msg)
#define HAMLET_LOG(msg) Serial.print(msg)
#else
#define HAMLET_LOGF(...) ((void)0)
#define HAMLET_LOGLN(msg) ((void)0)
#define HAMLET_LOG(msg) ((void)0)
#endif
