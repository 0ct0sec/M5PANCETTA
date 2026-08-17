#!/usr/bin/env python3
"""Build target-correct HAMLET app and merged release images.

The default remains Core2 for compatibility with the existing public release
workflow. Use ``--target cores3se`` for CoreS3 SE or ``--target all`` to build
both supported boards. Artifacts are named from each environment's own
HAMLET_VERSION value, so target images cannot overwrite one another.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


PROJECT_SLUG = "hamlet"
FLASH_SIZE = "16MB"


@dataclass(frozen=True)
class ReleaseTarget:
    key: str
    env_name: str
    chip: str
    flash_mode: str
    partition_file: str
    bootloader_offset: str


TARGETS = {
    "core2": ReleaseTarget(
        key="core2",
        env_name="m5stack-core2",
        chip="esp32",
        flash_mode="dio",
        partition_file="partitions_core2.csv",
        bootloader_offset="0x1000",
    ),
    "cores3se": ReleaseTarget(
        key="cores3se",
        env_name="m5stack-cores3se",
        chip="esp32s3",
        # The framework-generated CoreS3 bootloader deliberately enters in
        # DIO mode even though the application later configures 80 MHz QIO.
        # Preserve its ROM-facing image header instead of rewriting it here.
        flash_mode="keep",
        partition_file="partitions_cores3se.csv",
        bootloader_offset="0x0",
    ),
}


BANNER = r"""
HAMLET RELEASE
"""


def log(message: str, prefix: str = "[*]") -> None:
    print(f"{prefix} {message}")


def log_ok(message: str) -> None:
    log(message, "[+]")


def log_err(message: str) -> None:
    log(message, "[!]")


def log_info(message: str) -> None:
    log(message, "[>]")


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def read_platformio_ini(root: Path | None = None) -> str:
    ini_path = (root or project_root()) / "platformio.ini"
    if not ini_path.exists():
        raise FileNotFoundError(
            f"platformio.ini not found at {ini_path}; run from a valid checkout"
        )
    return ini_path.read_text(encoding="utf-8", errors="replace")


def get_env_section(content: str, env_name: str) -> str:
    match = re.search(
        rf"^\s*\[env:{re.escape(env_name)}\]\s*$",
        content,
        flags=re.MULTILINE,
    )
    if not match:
        raise ValueError(f"PlatformIO environment not found: {env_name}")
    next_section = re.search(r"^\s*\[", content[match.end() :], re.MULTILINE)
    end = (
        match.end() + next_section.start()
        if next_section
        else len(content)
    )
    return content[match.end() : end]


def clean_version(value: str) -> str:
    value = value.strip()
    if value.startswith('"') and value.endswith('"'):
        value = value[1:-1]
    if value.startswith("'") and value.endswith("'"):
        value = value[1:-1]
    return value.replace('\\"', '"').strip('"').strip()


def get_hamlet_define_version(
    env_name: str,
    content: str | None = None,
) -> str:
    """Read the HAMLET_VERSION compiler define for one environment."""
    section = get_env_section(content or read_platformio_ini(), env_name)
    patterns = (
        r"HAMLET_VERSION\s*=\s*'\"([^']+)\"'",
        r'HAMLET_VERSION\s*=\s*"([^"]+)"',
        r"HAMLET_VERSION\s*=\s*'([^']+)'",
        r"HAMLET_VERSION[^0-9]*"
        r"([0-9]+(?:\.[0-9]+)+[A-Za-z0-9._-]*)",
    )
    for pattern in patterns:
        match = re.search(pattern, section)
        if match:
            return clean_version(match.group(1))
    raise ValueError(
        f"Could not find HAMLET_VERSION in [env:{env_name}]"
    )


def get_version(env_name: str, content: str | None = None) -> str:
    """Read the release version and reject metadata/define drift."""
    source = content or read_platformio_ini()
    section = get_env_section(source, env_name)
    define_version = get_hamlet_define_version(env_name, source)

    match = re.search(
        r"^\s*custom_version\s*=\s*([^\s#;]+)",
        section,
        re.MULTILINE,
    )
    if not match:
        return define_version

    metadata_version = clean_version(match.group(1))
    if metadata_version != define_version:
        raise ValueError(
            f"Version mismatch in [env:{env_name}]: custom_version="
            f"{metadata_version!r}, HAMLET_VERSION={define_version!r}"
        )
    return metadata_version


def resolve_platformio(explicit: str | None = None) -> str:
    candidates: list[str | None] = [
        explicit,
        os.environ.get("PLATFORMIO_CMD"),
        shutil.which("platformio"),
        shutil.which("pio"),
    ]
    sibling_name = (
        "platformio.exe" if os.name == "nt" else "platformio"
    )
    candidates.append(str(Path(sys.executable).with_name(sibling_name)))
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return str(Path(candidate))
    raise FileNotFoundError(
        "PlatformIO CLI not found. Pass --pio or run with PlatformIO's Python."
    )


def run_cmd(
    command: Sequence[str],
    description: str,
    cwd: Path,
) -> str:
    log_info(f"{description}...")
    result = subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        log_err(f"Command failed ({result.returncode}): {' '.join(command)}")
        if result.stdout.strip():
            print(result.stdout.rstrip())
        if result.stderr.strip():
            print(result.stderr.rstrip(), file=sys.stderr)
        raise RuntimeError(description)
    return result.stdout


def find_app_bin(build_dir: Path) -> Path:
    preferred = build_dir / "firmware.bin"
    if preferred.exists():
        return preferred

    candidates = [
        path
        for path in build_dir.glob("*.bin")
        if path.name not in ("bootloader.bin", "partitions.bin")
    ]
    if not candidates:
        raise FileNotFoundError(
            f"No application .bin found in {build_dir}"
        )
    candidates.sort(key=lambda path: path.stat().st_size, reverse=True)
    if len(candidates) > 1:
        log_info(
            f"Multiple app candidates; using largest: {candidates[0].name}"
        )
    return candidates[0]


def find_boot_app0(root: Path) -> Path:
    package_roots: list[Path] = []
    env_home = os.environ.get("PLATFORMIO_HOME_DIR")
    if env_home:
        package_roots.append(Path(env_home) / "packages")
    package_roots.extend(
        [
            Path.home() / ".platformio" / "packages",
            root / ".platformio" / "packages",
        ]
    )

    relative = (
        Path("framework-arduinoespressif32")
        / "tools"
        / "partitions"
        / "boot_app0.bin"
    )
    for base in package_roots:
        candidate = base / relative
        if candidate.exists():
            return candidate
    for base in package_roots:
        if base.exists():
            matches = list(base.rglob("boot_app0.bin"))
            if matches:
                return matches[0]
    raise FileNotFoundError("boot_app0.bin not found in PlatformIO packages")


def get_app_offset_from_partitions(partitions_path: Path) -> str:
    if not partitions_path.exists():
        return "0x10000"
    for raw in partitions_path.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[0].lower() == "app0":
            return fields[3]
    return "0x10000"


def _resolve_recorded_path(path_value: str) -> Path:
    path = Path(path_value)
    if path.exists():
        return path
    if os.name != "nt" and len(path_value) > 2 and path_value[1] == ":":
        drive = path_value[0].lower()
        unix_value = path_value[3:].replace("\\", "/")
        return Path(f"/mnt/{drive}/{unix_value}")
    return path


def load_flash_images(
    idedata_path: Path,
    root: Path,
    build_dir: Path,
    target: ReleaseTarget,
) -> tuple[list[tuple[str, Path]], str]:
    if not idedata_path.exists():
        log_info(
            "idedata.json not found; using target-specific fallback offsets"
        )
        images = [
            (target.bootloader_offset, build_dir / "bootloader.bin"),
            ("0x8000", build_dir / "partitions.bin"),
            ("0xE000", find_boot_app0(root)),
        ]
        app_offset = get_app_offset_from_partitions(
            root / target.partition_file
        )
        return images, app_offset

    data = json.loads(idedata_path.read_text(encoding="utf-8"))
    extra = data.get("extra", {})
    images: list[tuple[str, Path]] = []
    for entry in extra.get("flash_images", []):
        offset = entry.get("offset")
        path_value = entry.get("path")
        if offset and path_value:
            images.append((offset, _resolve_recorded_path(path_value)))
    if not images:
        raise ValueError(f"No flash images found in {idedata_path}")
    return images, extra.get("application_offset", "0x10000")


def verify_release_images(
    app_image: Path,
    merged_image: Path,
    app_offset: str,
    components: Sequence[tuple[str, Path]],
) -> None:
    app_data = app_image.read_bytes()
    if not app_data or app_data[0] != 0xE9:
        raise ValueError(f"Invalid ESP application image: {app_image}")

    merged_data = merged_image.read_bytes()
    offset = int(app_offset, 16)
    if offset >= len(merged_data) or merged_data[offset] != 0xE9:
        raise ValueError(
            f"Merged image has no app header at {app_offset}: "
            f"{merged_image}"
        )
    if len(merged_data) < offset + len(app_data):
        raise ValueError(f"Merged image is truncated: {merged_image}")

    # merge_bin can rewrite the first ESP image header when flash-mode or
    # flash-size overrides are supplied. A valid checksum is not enough: the
    # CoreS3 SE ROM watchdog-resets if its DIO bootloader header is changed to
    # QIO. Require every input image to survive the merge byte-for-byte.
    for component_offset, component_path in components:
        component_data = component_path.read_bytes()
        start = int(component_offset, 16)
        actual = merged_data[start : start + len(component_data)]
        if actual != component_data:
            mismatch = next(
                (
                    index
                    for index, (expected, found) in enumerate(
                        zip(component_data, actual)
                    )
                    if expected != found
                ),
                min(len(component_data), len(actual)),
            )
            raise ValueError(
                "Merged image altered component "
                f"{component_path.name} at 0x{start + mismatch:X}"
            )


def build_target(
    target: ReleaseTarget,
    root: Path,
    pio: str,
) -> tuple[Path, Path]:
    version = get_version(target.env_name)
    log("")
    log("=" * 64)
    log(f"TARGET {target.key}: {target.env_name} / v{version}")
    log("=" * 64)

    builds_dir = root / "m5hamlet_builds"
    builds_dir.mkdir(exist_ok=True)
    build_dir = root / ".pio" / "build" / target.env_name

    run_cmd(
        [pio, "run", "-t", "clean", "-e", target.env_name],
        f"Cleaning {target.env_name}",
        root,
    )
    run_cmd(
        [pio, "run", "-e", target.env_name],
        f"Compiling {target.env_name}",
        root,
    )
    log_ok(f"{target.env_name} compiled successfully")

    app_bin = find_app_bin(build_dir)
    firmware_dst = builds_dir / f"firmware_v{version}.bin"
    merged_dst = (
        builds_dir / f"{PROJECT_SLUG}_v{version}_m5burner.bin"
    )
    shutil.copy2(app_bin, firmware_dst)
    log_ok(
        f"Created {firmware_dst.name} "
        f"({firmware_dst.stat().st_size / 1024:.1f} KB)"
    )

    flash_images, app_offset = load_flash_images(
        build_dir / "idedata.json",
        root,
        build_dir,
        target,
    )
    all_images = sorted(
        [*flash_images, (app_offset, app_bin)],
        key=lambda item: int(item[0], 16),
    )
    for offset, path in all_images:
        if not path.exists():
            raise FileNotFoundError(
                f"Required image missing at {offset}: {path}"
            )

    merge_command: list[str] = [
        pio,
        "pkg",
        "exec",
        "-p",
        "tool-esptoolpy",
        "--",
        "esptool.py",
        "--chip",
        target.chip,
        "merge_bin",
        "-o",
        str(merged_dst),
        "--flash_mode",
        target.flash_mode,
        "--flash_size",
        FLASH_SIZE,
    ]
    for offset, path in all_images:
        merge_command.extend([offset, str(path)])
    run_cmd(
        merge_command,
        f"Merging {target.key} full image",
        root,
    )
    verify_release_images(
        firmware_dst,
        merged_dst,
        app_offset,
        all_images,
    )
    log_ok(
        f"Created and verified {merged_dst.name} "
        f"({merged_dst.stat().st_size / 1024:.1f} KB)"
    )
    return firmware_dst, merged_dst


def selected_targets(value: str) -> Iterable[ReleaseTarget]:
    if value == "all":
        # CoreS3 SE is the active bench-primary target; keep Core2 in the
        # release set as the compatibility target.
        return (TARGETS["cores3se"], TARGETS["core2"])
    return (TARGETS[value],)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--target",
        choices=(*TARGETS.keys(), "all"),
        default="core2",
        help="release target (default: core2; use all for both)",
    )
    parser.add_argument(
        "--pio",
        help="explicit PlatformIO executable path",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    root = project_root()
    try:
        pio = resolve_platformio(args.pio)
        print(BANNER)
        log_info(f"Working directory: {root}")
        log_info(f"PlatformIO: {pio}")

        artifacts: list[Path] = []
        for target in selected_targets(args.target):
            artifacts.extend(build_target(target, root, pio))

        log("")
        log("=" * 64)
        log("BUILD COMPLETE")
        log("=" * 64)
        for artifact in artifacts:
            log_ok(str(artifact))
        return 0
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        log_err(str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
