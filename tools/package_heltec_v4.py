#!/usr/bin/env python3
"""Package and verify Heltec WiFi LoRa 32 V4 firmware artifacts."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path


FLASH_SIZE = 16 * 1024 * 1024
SEGMENT_LAYOUT = (
    (0x0000, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "firmware.bin"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create app-only and merged Heltec V4 Marauder images.",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(".pio/build/heltec_v4"),
        help="PlatformIO heltec_v4 build directory",
    )
    parser.add_argument(
        "--boot-app0",
        type=Path,
        required=True,
        help="Arduino-ESP32 boot_app0.bin path",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("artifacts/heltec_v4"),
        help="Artifact output directory",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_file(path: Path) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"Required build input is missing: {path}")
    return path


def verify_layout(segments: list[tuple[int, Path]]) -> None:
    previous_end = 0
    for offset, path in segments:
        size = path.stat().st_size
        if offset < previous_end:
            raise ValueError(
                f"Flash segments overlap at 0x{offset:x}: {path} begins before 0x{previous_end:x}",
            )
        previous_end = offset + size
    if previous_end > FLASH_SIZE:
        raise ValueError(
            f"Merged image ends at 0x{previous_end:x}, beyond the 16 MB flash boundary",
        )


def verify_embedded_segments(merged_path: Path, segments: list[tuple[int, Path]]) -> None:
    merged = merged_path.read_bytes()
    for offset, path in segments:
        expected = path.read_bytes()
        actual = merged[offset : offset + len(expected)]
        if actual != expected:
            raise ValueError(
                f"Merged image does not contain an exact {path.name} copy at 0x{offset:x}",
            )

    expected_size = segments[-1][0] + segments[-1][1].stat().st_size
    if len(merged) != expected_size:
        raise ValueError(
            f"Merged image size is {len(merged)} bytes; expected {expected_size}",
        )


def inspect_esp32_image(path: Path) -> None:
    subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3", "image_info", str(path)],
        check=True,
    )


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    boot_app0 = require_file(args.boot_app0.resolve())

    inputs = {
        "bootloader.bin": require_file(build_dir / "bootloader.bin"),
        "partitions.bin": require_file(build_dir / "partitions.bin"),
        "boot_app0.bin": boot_app0,
        "firmware.bin": require_file(build_dir / "firmware.bin"),
    }
    segments = [(offset, inputs[name]) for offset, name in SEGMENT_LAYOUT]
    verify_layout(segments)

    output_dir.mkdir(parents=True, exist_ok=True)
    merged_path = output_dir / "ESP32Marauder-Heltec-V4.bin"
    app_path = output_dir / "ESP32Marauder-Heltec-V4-app.bin"
    checksums_path = output_dir / "SHA256SUMS.txt"

    merge_command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        str(merged_path),
    ]
    for offset, path in segments:
        merge_command.extend((f"0x{offset:x}", str(path)))
    subprocess.run(merge_command, check=True)

    shutil.copyfile(inputs["firmware.bin"], app_path)
    verify_embedded_segments(merged_path, segments)
    inspect_esp32_image(inputs["bootloader.bin"])
    inspect_esp32_image(app_path)

    artifacts = (merged_path, app_path)
    checksums_path.write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in artifacts),
        encoding="utf-8",
    )

    print("Verified Heltec V4 flash layout:")
    for offset, path in segments:
        print(f"  0x{offset:05x}  {path.name}  {path.stat().st_size} bytes")
    for path in (*artifacts, checksums_path):
        print(f"  artifact  {path.name}  {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
