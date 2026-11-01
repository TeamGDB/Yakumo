#!/usr/bin/env python3
"""Bake this project's restricted, original SVG touch glyphs into ImGui geometry.

No runtime SVG parser, texture loader or third-party dependency is required.
Run from any directory; --check verifies the committed public-asset geometry.
"""
import argparse
import json
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "profiles/mhp3rd/host/ui/touch_icons_data.hpp"
TOKEN = re.compile(r"[MLHVQCAZmlhvqcaz]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?")


def flatten(data):
    tokens = TOKEN.findall(data)
    if re.sub(TOKEN, "", data).strip(" ,\t\r\n"):
        raise ValueError("Unsupported SVG path token")
    i, command, current, start, points = 0, None, (0., 0.), (0., 0.), []
    result = []

    def take(n):
        nonlocal i
        values = [float(x) for x in tokens[i:i+n]]
        if len(values) != n or not all(math.isfinite(v) for v in values):
            raise ValueError("Invalid path coordinates")
        i += n
        return values

    while i < len(tokens):
        if tokens[i].isalpha():
            command = tokens[i]
            i += 1
        if command not in {"M", "L", "H", "V", "Q", "C", "A", "Z"}:
            raise ValueError("Only absolute SVG commands are supported")
        x, y = current
        if command == "Z":
            if points:
                result.append((points, True))
                points = []
            current = start
            command = None
        elif command == "M":
            if points:
                result.append((points, False))
            current = start = tuple(take(2))
            points = [current]
            command = "L"
        elif command in {"L", "H", "V"}:
            end = tuple(take(2)) if command == "L" else (take(1)[0], y) if command == "H" else (x, take(1)[0])
            points.append(end)
            current = end
        elif command in {"Q", "C"}:
            values = take(4 if command == "Q" else 6)
            end = tuple(values[-2:])
            for n in range(1, 17):
                t = n / 16
                u = 1 - t
                if command == "Q":
                    point = (u*u*x + 2*u*t*values[0] + t*t*end[0], u*u*y + 2*u*t*values[1] + t*t*end[1])
                else:
                    point = (u**3*x + 3*u*u*t*values[0] + 3*u*t*t*values[2] + t**3*end[0],
                             u**3*y + 3*u*u*t*values[1] + 3*u*t*t*values[3] + t**3*end[1])
                points.append(point)
            current = end
        else:
            rx, ry, rotation, large, sweep, ex, ey = take(7)
            if rotation or rx <= 0 or ry <= 0 or large not in (0, 1) or sweep not in (0, 1):
                raise ValueError("Unsupported elliptical arc")
            dx, dy = (x-ex)/2, (y-ey)/2
            ratio = dx*dx/(rx*rx)+dy*dy/(ry*ry)
            if ratio > 1:
                rx *= math.sqrt(ratio)
                ry *= math.sqrt(ratio)
            denominator = rx*rx*dy*dy+ry*ry*dx*dx
            if denominator == 0:
                raise ValueError("Degenerate elliptical arc")
            factor = math.sqrt(max(0., (rx*rx*ry*ry-denominator)/denominator)) * (-1 if large == sweep else 1)
            cx, cy = factor*rx*dy/ry, -factor*ry*dx/rx
            angle = math.atan2((dy-cy)/ry, (dx-cx)/rx)
            end_angle = math.atan2((-dy-cy)/ry, (-dx-cx)/rx)
            delta = (end_angle-angle) % (2*math.pi)
            if not sweep:
                delta -= 2*math.pi
            cx += (x+ex)/2
            cy += (y+ey)/2
            for n in range(1, 33):
                t = angle+delta*n/32
                points.append((cx+rx*math.cos(t), cy+ry*math.sin(t)))
            current = (ex, ey)
    if points:
        result.append((points, False))
    return result


def geometry(root):
    output = []

    def visit(node, transform=(1., 1., 0., 0.)):
        sx, sy, tx, ty = transform
        raw = node.get("transform", "")
        matches = list(re.finditer(r"(translate|scale)\(([^)]+)\)", raw))
        if re.sub(r"(translate|scale)\(([^)]+)\)", "", raw).strip():
            raise ValueError("Unsupported SVG transform")
        for match in matches:
            v = [float(n) for n in re.split(r"[ ,]+", match[2].strip())]
            if len(v) not in (1, 2) or not all(math.isfinite(n) for n in v):
                raise ValueError("Invalid SVG transform")
            if match[1] == "translate":
                tx += sx*v[0]
                ty += sy*(v[1] if len(v) == 2 else 0)
            else:
                sx *= v[0]
                sy *= v[-1]
        tag = node.tag.split("}")[-1]
        if tag in {"svg", "g", "title"}:
            for child in node:
                visit(child, (sx, sy, tx, ty))
            return
        if tag == "path":
            shapes = flatten(node.attrib["d"])
        elif tag == "circle":
            cx, cy, r = (float(node.get(k, "0")) for k in ("cx", "cy", "r"))
            if r <= 0:
                raise ValueError("Invalid SVG circle")
            shapes = [([(cx+r*math.cos(i*math.tau/64), cy+r*math.sin(i*math.tau/64)) for i in range(64)], True)]
        else:
            raise ValueError("Unsupported SVG element: "+tag)
        filled = node.get("fill", "black") != "none"
        width = float(node.get("stroke-width", "1")) * abs(sx) if node.get("stroke", "none") != "none" else 0.
        if sx != sy or sx <= 0 or not math.isfinite(width) or width < 0:
            raise ValueError("Only positive uniform scaling is supported")
        for points, closed in shapes:
            mapped = [(x*sx+tx, y*sy+ty) for x, y in points]
            if not all(math.isfinite(x) and math.isfinite(y) and -2 <= x <= 66 and -2 <= y <= 66 for x, y in mapped):
                raise ValueError("Glyph geometry outside its viewBox")
            if len(points) < 2:
                raise ValueError("Incomplete SVG contour")
            output.append((mapped, closed, filled, width))
    visit(root)
    return output


def generate():
    points, paths, glyphs = [], [], []
    for folder in ["adaptive-controls", "adaptive-weapons"]:
        directory = ROOT / "docs/images" / folder
        manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        for name in manifest["glyphs"]:
            root = ET.parse(directory / (name+".svg")).getroot()
            first = len(paths)
            for contour, closed, filled, width in geometry(root):
                if filled:
                    area = sum(a[0]*b[1]-b[0]*a[1] for a, b in zip(contour, contour[1:]+contour[:1]))
                    if area < 0:
                        contour.reverse()
                if len(contour) > 256:
                    raise ValueError("Glyph contour exceeds the renderer buffer")
                paths.append((len(points), len(contour), width, closed, filled))
                points.extend(contour)
            glyphs.append((name, first, len(paths)-first, int(root.get("color", "#F1EDD8")[1:], 16)))
    def ident(name):
        return "".join(p.title() for p in name.split("-"))
    result = ["// Generated from original docs/images/adaptive-{controls,weapons} SVGs.",
              "// Regenerate: python profiles/mhp3rd/tools/touch_icons.py", "#pragma once", "#include <cstdint>",
              "namespace mhp3rd::ui::touch_icons {", "// clang-format off", "struct Point { float x, y; };",
              "struct Path { unsigned first, count; float width; bool closed, filled; };",
              "struct Glyph { unsigned first, count; std::uint32_t colour; };", "enum class Icon {",
              *["    "+ident(g[0])+"," for g in glyphs], "    Count", "};",
              "inline constexpr Point points[] = {"]
    result.extend(f"    {{{x:.4f}f, {y:.4f}f}}," for x, y in points)
    result.extend(["};", "inline constexpr Path paths[] = {"])
    result.extend(f"    {{{first}, {count}, {width:.4f}f, {str(closed).lower()}, {str(filled).lower()}}}," for first,count,width,closed,filled in paths)
    result.extend(["};", "inline constexpr Glyph glyphs[] = {"])
    result.extend(f"    {{{first}, {count}, 0x{colour:06x}u}}, // {name}" for name,first,count,colour in glyphs)
    result.extend(["};", "// clang-format on", "} // namespace mhp3rd::ui::touch_icons", ""])
    return "\n".join(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != data:
            parser.exit(1, "Touch icon geometry needs regeneration\n")
    else:
        OUTPUT.write_text(data, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
