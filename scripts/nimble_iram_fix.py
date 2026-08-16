#!/usr/bin/env python3
"""IRAM budget fixes for ESP32 builds.

1. Strips -mfix-esp32-psram-cache-issue from libraries that don't access PSRAM.
   The flag adds per-function IRAM window stubs. Safe libs don't need them.

2. Patches sections.ld to evict ~4KB of libc functions from IRAM to flash.
   The ESP32 PSRAM cache workaround places 120+ libc functions in IRAM.
   Most never operate on PSRAM pointers. Removing their explicit IRAM placement
   lets them fall through to .flash.text's wildcard — freeing IRAM for BLE.

PSRAM cache fix:
  SAFE to strip: NimBLE, ArduinoJson, HTTPClient, WebServer, WiFiClientSecure
  NOT SAFE: M5GFX (DMA + sprite buffers), M5Unified (display init)
"""

Import("env")
import os, re

# ==[ TARGET GATE ]== both fixes below are ESP32-only.
# The PSRAM cache errata - and therefore -mfix-esp32-psram-cache-issue and
# the IRAM bloat it causes - does not exist on the S3, and the sections.ld
# path is per-target. Without this gate an S3 build silently no-ops, which
# is the wrong failure mode for a script whose whole job is fitting IRAM.
MCU = env.BoardConfig().get("build.mcu", "esp32")
IS_ESP32 = (MCU == "esp32")

# ==[ 1. STRIP PSRAM CACHE FIX FROM SAFE LIBS ]== saves IRAM
STRIP_LIBS = {"NimBLE", "ArduinoJson", "HTTPClient", "WebServer", "WiFiClientSecure", "FastLED"}

def strip_psram_fix(lb):
    target = "-mfix-esp32-psram-cache-issue"
    stripped = False
    for key in ("CCFLAGS", "CFLAGS", "CXXFLAGS"):
        flags = lb.env.get(key, [])
        filtered = [f for f in flags if target not in str(f)]
        if len(filtered) != len(flags):
            lb.env.Replace(**{key: filtered})
            stripped = True
    if stripped:
        print(f"[HAMLET] {lb.name}: PSRAM cache fix stripped (IRAM savings)")

for lb in (env.GetLibBuilders() if IS_ESP32 else []):
    if any(name in lb.name for name in STRIP_LIBS):
        strip_psram_fix(lb)

# ==[ 2. PATCH SECTIONS.LD ]== evict libc from IRAM → flash
# Remove specific libc entries from .iram0.text in sections.ld.
# These functions then fall through to .flash.text's wildcard.
# Safe because they never operate on PSRAM pointers, and the PSRAM cache fix
# window stubs (from -mfix-esp32-psram-cache-issue) still protect them in flash.

EVICT_LIBC = {
    # time — never called with PSRAM pointers
    "lib_a-strftime", "lib_a-strptime", "lib_a-mktime", "lib_a-tzset_r",
    "lib_a-lcltime_r", "lib_a-lcltime", "lib_a-gmtime_r", "lib_a-gmtime",
    "lib_a-timelocal", "lib_a-tzcalc_limits", "lib_a-asctime", "lib_a-asctime_r",
    "lib_a-ctime", "lib_a-ctime_r", "lib_a-time", "lib_a-tzset", "lib_a-tzlock",
    "lib_a-tzvars", "lib_a-gettzinfo", "lib_a-month_lengths",
    # environment — unused on ESP32
    "lib_a-environ", "lib_a-envlock", "lib_a-getenv_r",
    # syscalls — no PSRAM involvement
    "lib_a-system", "lib_a-raise", "lib_a-sbrk",
    "lib_a-sysopen", "lib_a-sysclose", "lib_a-sysread", "lib_a-syswrite",
    "lib_a-syssbrk", "lib_a-systimes",
    "lib_a-creat", "lib_a-open", "lib_a-close", "lib_a-read",
    # stdio internals — buffered I/O plumbing
    "lib_a-findfp", "lib_a-fvwrite", "lib_a-fwalk", "lib_a-fclose", "lib_a-fflush",
    "lib_a-fputwc", "lib_a-makebuf", "lib_a-refill", "lib_a-stdio",
    "lib_a-ungetc", "lib_a-wbuf", "lib_a-wsetup",
    "lib_a-wcrtomb", "lib_a-wctomb_r", "lib_a-sccl",
    # random — no PSRAM access
    "lib_a-rand", "lib_a-rand_r", "lib_a-srand",
    # math/conversion — pure computation
    "lib_a-div", "lib_a-ldiv", "lib_a-abs", "lib_a-labs",
    "lib_a-setjmp", "lib_a-longjmp",
    "lib_a-quorem", "lib_a-rshift", "lib_a-sf_nan", "lib_a-s_fpclassify",
    "lib_a-itoa", "lib_a-utoa", "lib_a-atoi", "lib_a-atol",
    "lib_a-strtol", "lib_a-strtoul",
    # char classification — lookup tables, no PSRAM
    "lib_a-ctype_", "lib_a-isalnum", "lib_a-isalpha", "lib_a-isascii",
    "lib_a-isblank", "lib_a-iscntrl", "lib_a-isdigit", "lib_a-isgraph",
    "lib_a-islower", "lib_a-isprint", "lib_a-ispunct", "lib_a-isspace",
    "lib_a-isupper", "lib_a-toascii", "lib_a-tolower", "lib_a-toupper",
    # rare string ops — never used with PSRAM data
    "lib_a-strcoll", "lib_a-strlwr", "lib_a-strupr",
    "lib_a-strdup", "lib_a-strdup_r", "lib_a-strndup", "lib_a-strndup_r",
    # misc
    "lib_a-impure", "lib_a-lock",
}

# Also evict non-lib_a-prefixed entries from sections.ld
EVICT_BARE = {"creat", "isatty"}

def patch_sections_ld():
    """Patch framework sections.ld: remove libc IRAM entries so they go to flash."""
    if not IS_ESP32:
        print("[HAMLET] %s: skipping libc IRAM eviction "
              "(ESP32-only; no PSRAM cache errata on this target)" % MCU)
        return
    # Find framework sections.ld
    sdk_ld = os.path.join(
        env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
        "tools", "sdk", "esp32", "ld", "sections.ld"
    )
    if not os.path.isfile(sdk_ld):
        print(f"[HAMLET] WARNING: sections.ld not found at {sdk_ld}")
        return

    patched_ld = os.path.join(env.get("PROJECT_DIR"), "ld", "sections_patched.ld")

    with open(sdk_ld, "r") as f:
        lines = f.readlines()

    evicted = 0
    out = []
    # State machine: IDLE → FOUND_HEADER → INSIDE
    state = "IDLE"
    brace_depth = 0
    for line in lines:
        stripped = line.strip()
        if state == "IDLE":
            if ".iram0.text :" in stripped:
                state = "FOUND_HEADER"
                brace_depth = stripped.count("{") - stripped.count("}")
                if brace_depth > 0:
                    state = "INSIDE"
        elif state == "FOUND_HEADER":
            brace_depth += stripped.count("{") - stripped.count("}")
            if brace_depth > 0:
                state = "INSIDE"
        elif state == "INSIDE":
            brace_depth += stripped.count("{") - stripped.count("}")
            if brace_depth <= 0:
                state = "IDLE"
        # Only evict libc entries that are inside .iram0.text
        skip = False
        if state == "INSIDE" and stripped.startswith("*libc.a:"):
            m = re.match(r'\*libc\.a:([^.]+)', stripped)
            if m:
                obj_name = m.group(1)
                if obj_name in EVICT_LIBC or obj_name in EVICT_BARE:
                    skip = True
                    evicted += 1
        if not skip:
            out.append(line)

    os.makedirs(os.path.dirname(patched_ld), exist_ok=True)
    with open(patched_ld, "w") as f:
        f.writelines(out)

    # Swap sections.ld in LINKFLAGS
    linkflags = env.get("LINKFLAGS", [])
    swapped = False
    for i, flag in enumerate(linkflags):
        if str(flag) == "sections.ld":
            linkflags[i] = patched_ld
            swapped = True
            break
    if swapped:
        env.Replace(LINKFLAGS=linkflags)
        print(f"[HAMLET] sections.ld patched: {evicted} libc functions evicted from IRAM")
    else:
        print(f"[HAMLET] WARNING: could not swap sections.ld in LINKFLAGS")

patch_sections_ld()
