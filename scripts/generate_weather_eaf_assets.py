#!/usr/bin/env python3
"""Generate Emote EAF weather animations from Meteocons animated SVG assets."""

import argparse
import io
import json
import math
import re
import struct
from pathlib import Path
from xml.etree import ElementTree as ET


EAF_MAGIC_HEAD = 0x5A5A
EAF_FORMAT_MAGIC = 0x89
EAF_ENCODING_RLE = 0
EAF_ENCODING_RAW = 5


def import_render_deps():
    try:
        import cairosvg
        from PIL import Image
    except ImportError as exc:
        raise SystemExit(
            "Missing Python dependencies. Install them with:\n"
            "  python3 -m pip install --target /private/tmp/codex_pydeps pillow cairosvg\n"
            "Then run with:\n"
            "  PYTHONPATH=/private/tmp/codex_pydeps python3 scripts/generate_weather_eaf_assets.py"
        ) from exc
    return cairosvg, Image


def local_name(tag):
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def parse_time(value, default=0.0):
    if not value:
        return default
    value = value.split(";")[0].strip()
    match = re.match(r"^(-?\d+(?:\.\d+)?)(ms|s)?$", value)
    if not match:
        return default
    number = float(match.group(1))
    unit = match.group(2) or "s"
    return number / 1000.0 if unit == "ms" else number


def parse_numbers(value):
    return [float(item) for item in re.findall(r"-?\d+(?:\.\d+)?", value)]


def interpolate_values(left, right, progress):
    left_values = parse_numbers(left)
    right_values = parse_numbers(right)
    if len(left_values) != len(right_values):
        return parse_numbers(left)
    return [a + (b - a) * progress for a, b in zip(left_values, right_values)]


def format_number(value):
    if abs(value - round(value)) < 0.001:
        return str(int(round(value)))
    return f"{value:.3f}".rstrip("0").rstrip(".")


def smoothstep(value):
    return value * value * (3.0 - 2.0 * value)


def sample_values(values_text, key_times_text, local_time, duration, calc_mode):
    values = [item.strip() for item in values_text.split(";") if item.strip()]
    if not values:
        return []
    if len(values) == 1 or duration <= 0:
        return parse_numbers(values[0])

    if key_times_text:
        key_times = [float(item) for item in key_times_text.split(";") if item.strip()]
    else:
        key_times = [i / (len(values) - 1) for i in range(len(values))]

    fraction = max(0.0, min(1.0, local_time / duration))
    for index in range(len(values) - 1):
        start = key_times[index] if index < len(key_times) else index / (len(values) - 1)
        end = key_times[index + 1] if index + 1 < len(key_times) else (index + 1) / (len(values) - 1)
        if fraction <= end or index == len(values) - 2:
            if end <= start:
                progress = 0.0
            else:
                progress = (fraction - start) / (end - start)
            progress = max(0.0, min(1.0, progress))
            if calc_mode == "spline":
                progress = smoothstep(progress)
            return interpolate_values(values[index], values[index + 1], progress)
    return parse_numbers(values[-1])


def animation_local_time(anim, timestamp):
    begin = parse_time(anim.attrib.get("begin"), 0.0)
    duration = parse_time(anim.attrib.get("dur"), 0.0)
    if duration <= 0 or timestamp < begin:
        return None, duration
    return (timestamp - begin) % duration, duration


def sampled_svg(svg_text, timestamp):
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    root = ET.fromstring(svg_text)
    parent_map = {child: parent for parent in root.iter() for child in parent}
    transform_updates = {}
    remove_list = []

    for anim in list(root.iter()):
        name = local_name(anim.tag)
        if name not in ("animate", "animateTransform"):
            continue

        parent = parent_map.get(anim)
        if parent is None:
            continue

        local_time, duration = animation_local_time(anim, timestamp)
        if local_time is not None:
            values = sample_values(
                anim.attrib.get("values", ""),
                anim.attrib.get("keyTimes", ""),
                local_time,
                duration,
                anim.attrib.get("calcMode", "linear"),
            )

            if name == "animateTransform":
                transform_type = anim.attrib.get("type", "")
                if transform_type == "translate" and len(values) >= 2:
                    transform = f"translate({format_number(values[0])} {format_number(values[1])})"
                    transform_updates.setdefault(parent, []).append(transform)
                elif transform_type == "rotate" and len(values) >= 1:
                    if len(values) >= 3:
                        transform = (
                            f"rotate({format_number(values[0])} "
                            f"{format_number(values[1])} {format_number(values[2])})"
                        )
                    else:
                        transform = f"rotate({format_number(values[0])})"
                    transform_updates.setdefault(parent, []).append(transform)
            else:
                attr = anim.attrib.get("attributeName")
                if attr and values:
                    parent.set(attr, format_number(values[0]))

        remove_list.append((parent, anim))

    for parent, anim in remove_list:
        parent.remove(anim)

    for node, transforms in transform_updates.items():
        base = node.attrib.get("transform", "").strip()
        node.set("transform", " ".join(item for item in [base, *transforms] if item))

    return ET.tostring(root, encoding="utf-8")


def render_svg_frame(cairosvg, Image, svg_text, timestamp, size):
    png_bytes = cairosvg.svg2png(
        bytestring=sampled_svg(svg_text, timestamp),
        output_width=size,
        output_height=size,
    )
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def quantize_rgba_to_indexed(Image, image):
    width, height = image.size
    alpha = image.getchannel("A")
    rgb = Image.new("RGB", image.size, (0, 0, 0))
    rgb.paste(image.convert("RGB"), mask=alpha)
    quantized = rgb.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    palette = quantized.getpalette() or []
    indexed = bytearray(quantized.tobytes())
    alpha_bytes = alpha.tobytes()

    eaf_palette = bytearray(256 * 4)
    for index in range(255):
        r = palette[index * 3] if index * 3 < len(palette) else 0
        g = palette[index * 3 + 1] if index * 3 + 1 < len(palette) else 0
        b = palette[index * 3 + 2] if index * 3 + 2 < len(palette) else 0
        target = (index + 1) * 4
        eaf_palette[target:target + 4] = bytes((b, g, r, 255))

    for pos, alpha_value in enumerate(alpha_bytes):
        indexed[pos] = 0 if alpha_value < 16 else indexed[pos] + 1

    return width, height, bytes(indexed), bytes(eaf_palette)


def rle_encode(data):
    encoded = bytearray()
    pos = 0
    while pos < len(data):
        value = data[pos]
        count = 1
        while pos + count < len(data) and data[pos + count] == value and count < 255:
            count += 1
        encoded.extend((count, value))
        pos += count
    return bytes(encoded)


def make_frame(width, height, indexed_pixels, palette, block_height):
    blocks = math.ceil(height / block_height)
    block_payloads = []
    block_lengths = []

    for block in range(blocks):
        y0 = block * block_height
        y1 = min(height, y0 + block_height)
        raw = bytearray()
        for y in range(y0, y1):
            offset = y * width
            raw.extend(indexed_pixels[offset:offset + width])

        rle = rle_encode(raw)
        if len(rle) < len(raw):
            payload = bytes((EAF_ENCODING_RLE,)) + rle
        else:
            payload = bytes((EAF_ENCODING_RAW,)) + bytes(raw)
        block_payloads.append(payload)
        block_lengths.append(len(payload))

    header = bytearray()
    header.extend(b"_S")
    header.extend(b"\x00")
    header.extend(b"\x00\x00\x00\x00\x00\x01")
    header.extend(bytes((8,)))
    header.extend(struct.pack("<HHHH", width, height, blocks, block_height))
    for length in block_lengths:
        header.extend(struct.pack("<I", length))
    header.extend(palette)
    header.extend(b"".join(block_payloads))

    return struct.pack("<H", EAF_MAGIC_HEAD) + bytes(header)


def checksum(data):
    return sum(data) & 0xFFFFFFFF


def write_eaf(path, frames):
    table = bytearray()
    data = bytearray()
    for frame in frames:
        table.extend(struct.pack("<II", len(frame), len(data)))
        data.extend(frame)

    body = bytes(table + data)
    header = bytearray()
    header.extend(bytes((EAF_FORMAT_MAGIC,)))
    header.extend(b"EAF")
    header.extend(struct.pack("<III", len(frames), checksum(body), len(body)))
    path.write_bytes(bytes(header) + body)


def build_weather_eaf(svg_path, output_path, cairosvg, Image, size, frames, cycle_seconds, block_height):
    svg_text = svg_path.read_text(encoding="utf-8")
    eaf_frames = []
    for frame_index in range(frames):
        timestamp = cycle_seconds * frame_index / frames
        image = render_svg_frame(cairosvg, Image, svg_text, timestamp, size)
        width, height, indexed, palette = quantize_rgba_to_indexed(Image, image)
        eaf_frames.append(make_frame(width, height, indexed, palette, block_height))
    write_eaf(output_path, eaf_frames)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path,
                        default=Path("main/assets/weather/meteocons/flat/animated"))
    parser.add_argument("--output-dir", type=Path,
                        default=Path("main/assets/weather/meteocons/emote-eaf"))
    parser.add_argument("--size", type=int, default=80)
    parser.add_argument("--frames", type=int, default=24)
    parser.add_argument("--cycle-seconds", type=float, default=6.0)
    parser.add_argument("--fps", type=int, default=8)
    parser.add_argument("--block-height", type=int, default=8)
    args = parser.parse_args()

    cairosvg, Image = import_render_deps()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    emote_config = []
    for svg_path in sorted(args.input_dir.glob("*.svg")):
        emote_name = f"weather-{svg_path.stem}"
        eaf_name = f"{emote_name}.eaf"
        output_path = args.output_dir / eaf_name
        build_weather_eaf(svg_path, output_path, cairosvg, Image,
                          args.size, args.frames, args.cycle_seconds, args.block_height)
        emote_config.append({
            "emote": emote_name,
            "src": eaf_name,
            "loop": True,
            "fps": args.fps,
        })
        print(f"{svg_path.name} -> {output_path}")

    (args.output_dir / "emote.json").write_text(
        json.dumps(emote_config, indent=4, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
