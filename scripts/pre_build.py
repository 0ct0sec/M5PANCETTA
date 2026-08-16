#!/usr/bin/env python3
"""Generate Hamlet build metadata and apply dependency compatibility fixes."""

Import("env")
import os
import re
import subprocess
from datetime import datetime

def get_git_commit():
    """Get short git commit hash, or 'unknown' if not in a git repo"""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=5,
            cwd=env.get("PROJECT_DIR")  # Run from project root
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return "unknown"

def get_build_revision():
    """Use an explicit release marker or fall back to the current Git commit."""
    revision = os.environ.get("HAMLET_BUILD_REVISION", "").strip()
    if not revision:
        return get_git_commit()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,31}", revision):
        raise ValueError("HAMLET_BUILD_REVISION must be 1-32 safe label characters")
    return revision

# Generate build info immediately (before compilation starts)
build_info = {
    "build_time": datetime.now().strftime("%d %b %Y"),  # e.g. "27 Dec 2025"
    "version": env.GetProjectOption("custom_version", "0.1.0"),
    "commit": get_build_revision()
}

info_path = os.path.join(env.get("PROJECT_SRC_DIR"), "build_info.h")
with open(info_path, "w") as f:
    f.write("// Auto-generated build info - DO NOT EDIT\n")
    f.write("#pragma once\n\n")
    f.write(f'#define BUILD_TIME "{build_info["build_time"]}"\n')
    f.write(f'#define BUILD_VERSION "{build_info["version"]}"\n')
    f.write(f'#define BUILD_COMMIT "{build_info["commit"]}"\n')

print(f"[HAMLET] Build info: {build_info['build_time']} revision {build_info['commit']}")

# ==[ NimBLE IRAM patch ]== strip IRAM_ATTR from NimBLE host code to fit IRAM budget.
# NimBLE marks 105 functions IRAM_ATTR for latency-critical BLE connections.
# We only do passive scanning — flash execution is fine. Saves ~4KB IRAM.
nimble_src = os.path.join(
    env.get("PROJECT_LIBDEPS_DIR", ""),
    env.get("PIOENV", "m5stack-core2"),
    "NimBLE-Arduino", "src"
)
marker = "/* HAMLET_IRAM_PATCH */"
patched_count = 0
if os.path.isdir(nimble_src):
    for root, dirs, files in os.walk(nimble_src):
        for fname in files:
            if not (fname.endswith(".c") or fname.endswith(".h")):
                continue
            fpath = os.path.join(root, fname)
            with open(fpath, "r", errors="replace") as f:
                content = f.read()
            if "IRAM_ATTR" not in content or marker in content:
                continue
            # Replace IRAM_ATTR with empty (keep the space for readability)
            patched = content.replace("IRAM_ATTR ", " ")
            patched = patched.replace("IRAM_ATTR\n", "\n")
            if patched != content:
                patched = marker + "\n" + patched
                with open(fpath, "w") as f:
                    f.write(patched)
                patched_count += 1
    if patched_count:
        print(f"[HAMLET] NimBLE IRAM_ATTR stripped from {patched_count} files")
    else:
        print("[HAMLET] NimBLE IRAM patch already applied")

# ==[ I2S compatibility patch ]== replace deprecated ADC_ATTEN_DB_11 usage with ADC_ATTEN_DB_12.
packages_root = os.path.expanduser("~/.platformio/packages")
i2s_cpp = os.path.join(
    packages_root,
    "framework-arduinoespressif32",
    "libraries",
    "I2S",
    "src",
    "I2S.cpp"
)
if os.path.exists(i2s_cpp):
    with open(i2s_cpp, "r", encoding="utf-8", errors="replace") as f:
        i2s_content = f.read()
    patched_i2s = i2s_content.replace(
        "esp_i2s::ADC_ATTEN_DB_11",
        "esp_i2s::ADC_ATTEN_DB_12"
    )
    if patched_i2s != i2s_content:
        with open(i2s_cpp, "w", encoding="utf-8") as f:
            f.write(patched_i2s)
        print("[HAMLET] I2S deprecated ADC_ATTEN_DB_11 replaced with ADC_ATTEN_DB_12")
    else:
        print("[HAMLET] I2S deprecation patch already applied")
else:
    print(f"[HAMLET] I2S source not found, skipping deprecation patch: {i2s_cpp}")
