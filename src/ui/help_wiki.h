/**
 * Help Wiki — per-screen noir narrator overlay
 * tap status bar to open, tap to advance pages, swipe left to dismiss
 */
#pragma once

#include "../hamlet.h"

namespace HelpWiki {
    const char* getHelpTitle(HamletMode mode);
    const char* getHelpPage(HamletMode mode, uint8_t page);
    uint8_t     getPageCount(HamletMode mode);
}
