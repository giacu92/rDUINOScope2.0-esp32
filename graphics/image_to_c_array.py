#!/usr/bin/env python3
"""Convert an image file into a C/C++ byte array for display assets.

Example:
  python graphics/image_to_c_array.py graphics/stellarium_icon_20p.png \
      --symbol STELLARIUM_ICON_20P_PNG \
      --output /tmp/stellarium_icon.inc

Then paste the generated block into lib/Display/icons.h.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


EXAMPLES = """examples:
  %(prog)s graphics/stellarium_icon_20p.png --symbol STELLARIUM_ICON_20P_PNG
  %(prog)s graphics/nowifi_icon_20p.png --symbol NOWIFI_ICON_20P -o graphics/nowifi_icon_20p.inc
  %(prog)s graphics/wifi_icon_20p.png --symbol WIFI_ICON_20P_PNG -o graphics/wifi_icon_20p.inc
  %(prog)s graphics/wifi_icon_20p.png --symbol WIFI_ICON_20P_PNG --namespace display_assets
"""


def default_symbol(path: Path) -> str:
    stem = re.sub(r"[^A-Za-z0-9]+", "_", path.stem).strip("_")
    if not stem:
        stem = "IMAGE"
    if stem[0].isdigit():
        stem = f"IMAGE_{stem}"

    suffix = path.suffix.lower().lstrip(".")
    if suffix:
        stem = f"{stem}_{suffix}"
    return stem.upper()


def c_array_block(data: bytes, symbol: str, namespace: str, columns: int) -> str:
    lines: list[str] = []

    if namespace:
        lines.append(f"namespace {namespace} {{")
        lines.append("")

    lines.append(f"static const uint8_t {symbol}[] = {{")
    for offset in range(0, len(data), columns):
        chunk = data[offset : offset + columns]
        values = ", ".join(f"0x{byte:02X}" for byte in chunk)
        lines.append(f"    {values},")
    lines.append("};")
    lines.append("")
    lines.append(f"static const size_t {symbol}_SIZE = sizeof({symbol});")

    if namespace:
        lines.append("")
        lines.append(f"}} // namespace {namespace}")

    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a C/C++ uint8_t array from an image file.",
        epilog=EXAMPLES,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("image", type=Path, help="Input image file, for example a PNG.")
    parser.add_argument(
        "--symbol",
        help="Array symbol name. Defaults to an uppercase name based on the file name.",
    )
    parser.add_argument(
        "--namespace",
        default="display_assets",
        help="C++ namespace to wrap the block in. Use '' for no namespace.",
    )
    parser.add_argument(
        "--columns",
        type=int,
        default=12,
        help="Number of bytes per generated row.",
    )
    parser.add_argument(
        "--output",
        "-o",
        type=Path,
        help="Write output to this file. Defaults to stdout.",
    )

    args = parser.parse_args()
    if args.columns <= 0:
        parser.error("--columns must be greater than zero")

    image_path = args.image
    if not image_path.is_file():
        parser.error(f"input file not found: {image_path}")

    symbol = args.symbol or default_symbol(image_path)
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol):
        parser.error(f"invalid C symbol name: {symbol}")

    data = image_path.read_bytes()
    block = c_array_block(data, symbol, args.namespace, args.columns)

    header = "#include <stddef.h>\n#include <stdint.h>\n\n"
    output = header + block if args.output else block

    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
