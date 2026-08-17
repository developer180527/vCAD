#!/usr/bin/env python3
"""Generates vCAD's operation icons as SVG.

# Why these are generated and not drawn

A CAD ribbon icon is geometry, not illustration: a block, a profile, a swept solid, one edge picked
out. Drawing sixty-eight of them by hand guarantees they drift — the isometric angle wanders, the
outline weight varies, the light comes from a different corner on the ones drawn on a Friday. That
drift is exactly what makes an icon set look amateur, and it is invisible one icon at a time.

Describing each icon as a few solids in 3D and projecting them through ONE camera makes drift
impossible. It also means a palette change is a re-run rather than sixty-eight edits.

# The grammar

Everything below follows four rules, and they are the whole reason the icons read at a glance:

  1. **One camera.** Classic isometric, same for every glyph. A box is the same box everywhere.
  2. **Grey is what exists; accent is what the operation does.** This carries the verb. Extrude is a
     grey profile with an accent solid rising off it; fillet is a grey block with the rounded edge
     in accent. The eye finds the coloured part and reads the operation from it.
  3. **Fixed light.** Top face lightest, left darker, right darkest. Never varies, so solids look
     like solids without gradients or shadows.
  4. **Flat fills and one outline weight.** No gradients, no inner detail that dies at 16 px.

Usage:  python3 tools/make_icons.py [outdir]
"""

import math
import pathlib
import sys

# ── the camera ────────────────────────────────────────────────────────────────────────────

COS30 = math.cos(math.radians(30))

def iso(x, y, z):
    """Classic isometric. +x goes right-and-down, +y left-and-down, +z straight up."""
    return ((x - y) * COS30, (x + y) * 0.5 - z)


# ── the palette ───────────────────────────────────────────────────────────────────────────
#
# Neutrals are the theme's own greys and the accent is its blue, so the icons sit in the ribbon
# rather than on it. Literal hex rather than CSS variables because QSvgRenderer's CSS support is
# thin — and because Theme.cpp already recolours #rrggbb, so a dark theme can run the same regex.

OUTLINE = "#3c4045"
GREY = {"top": "#eceae7", "left": "#cfcdc9", "right": "#b9b7b2"}
ACCENT = {"top": "#5aa9ea", "left": "#1f7fd0", "right": "#0a6cc4"}
GUIDE = "#0a70c8"     # profiles, paths, axes — the thing being operated ON
STROKE = 0.9


class Canvas:
    """Collects projected polygons, then fits them to the viewBox.

    Fitting afterwards rather than choosing coordinates that happen to land in a 24-unit box is the
    difference between icons that are all the same optical size and icons that are each nearly
    right. It also means an icon can be described in whatever units suit it.
    """

    def __init__(self):
        self.items = []          # (kind, points, attrs)

    def poly(self, pts3, fill, outline=True, dash=None, width=STROKE):
        self.items.append(("poly", [iso(*p) for p in pts3],
                           {"fill": fill, "outline": outline, "dash": dash, "width": width}))

    def line(self, a3, b3, colour=OUTLINE, dash=None, width=STROKE):
        self.items.append(("line", [iso(*a3), iso(*b3)],
                           {"fill": None, "outline": colour, "dash": dash, "width": width}))

    def polyline(self, pts3, colour=GUIDE, dash=None, width=STROKE):
        self.items.append(("polyline", [iso(*p) for p in pts3],
                           {"fill": None, "outline": colour, "dash": dash, "width": width}))

    def render(self, size=24, pad=1.6):
        flat = [p for _, pts, _ in self.items for p in pts]
        xs = [p[0] for p in flat]
        ys = [p[1] for p in flat]
        w = max(xs) - min(xs)
        h = max(ys) - min(ys)
        scale = (size - 2 * pad) / max(w, h, 1e-9)
        # Centred on both axes, so a wide icon and a tall one share an optical centre.
        ox = pad + (size - 2 * pad - w * scale) / 2 - min(xs) * scale
        oy = pad + (size - 2 * pad - h * scale) / 2 - min(ys) * scale

        def place(p):
            return (p[0] * scale + ox, p[1] * scale + oy)

        out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" '
               f'width="{size}" height="{size}">']
        for kind, pts, a in self.items:
            d = " ".join(f"{x:.2f},{y:.2f}" for x, y in (place(p) for p in pts))
            dash = f' stroke-dasharray="{a["dash"]}"' if a["dash"] else ""
            if kind == "poly":
                stroke = (f' stroke="{OUTLINE}" stroke-width="{a["width"]}" stroke-linejoin="round"'
                          if a["outline"] else "")
                out.append(f'<polygon points="{d}" fill="{a["fill"]}"{stroke}{dash}/>')
            else:
                out.append(f'<polyline points="{d}" fill="none" stroke="{a["outline"]}" '
                           f'stroke-width="{a["width"]}" stroke-linecap="round" '
                           f'stroke-linejoin="round"{dash}/>')
        out.append("</svg>")
        return "\n".join(out)


def solid(c, x0, y0, z0, w, d, h, palette):
    """A box, drawn as its three visible faces.

    Back to front, so the painter's algorithm does the hiding — no depth sort needed at this angle
    because the three faces of one box never overlap each other.
    """
    x1, y1, z1 = x0 + w, y0 + d, z0 + h
    c.poly([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], palette["top"])
    c.poly([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], palette["left"])
    c.poly([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], palette["right"])


def disc(c, cx, cy, z, r, palette, segments=40):
    """A horizontal circle as a filled polygon — a cylinder's cap."""
    c.poly([(cx + r * math.cos(2 * math.pi * i / segments),
             cy + r * math.sin(2 * math.pi * i / segments), z) for i in range(segments)],
           palette["top"])


def hull(points):
    """Convex hull of 2D points, monotone chain. Points are (x, y) in PROJECTED space."""
    pts = sorted(set(points))
    if len(pts) < 3:
        return pts

    def half(seq):
        out = []
        for p in seq:
            while len(out) >= 2:
                (ax, ay), (bx, by) = out[-2], out[-1]
                if (bx - ax) * (p[1] - ay) - (by - ay) * (p[0] - ax) > 0:
                    break
                out.pop()
            out.append(p)
        return out[:-1]

    return half(pts) + half(reversed(pts))


def tube(c, cx, cy, z0, h, r, palette, segments=48):
    """A cylinder: its silhouette, then the top cap.

    The silhouette is the CONVEX HULL of the two projected rings. Walking the bottom ring and then
    back along the top — the obvious construction — produces a self-intersecting polygon whose
    outline renders as a lens with a pinched waist rather than a cylinder. The hull is correct by
    construction at any camera angle, which matters because the camera is shared with every other
    icon and cannot be tuned to make one of them look right.
    """
    ring = [(cx + r * math.cos(2 * math.pi * i / segments),
             cy + r * math.sin(2 * math.pi * i / segments)) for i in range(segments)]
    projected = ([iso(x, y, z0) for x, y in ring] + [iso(x, y, z0 + h) for x, y in ring])
    c.items.append(("poly", hull(projected),
                    {"fill": palette["left"], "outline": True, "dash": None, "width": STROKE}))
    disc(c, cx, cy, z0 + h, r, palette, segments)


def profile(c, pts2, z=0.0, dash="1.4 1.1"):
    """A sketch profile: the thing an operation consumes, drawn as a dashed accent outline."""
    c.polyline([(x, y, z) for x, y in pts2] + [(pts2[0][0], pts2[0][1], z)], GUIDE, dash)


# ── the eight Create icons ────────────────────────────────────────────────────────────────

def icon_sketch():
    """A plane with a profile drawn on it. The plane is grey (it exists), the profile accent."""
    c = Canvas()
    c.poly([(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0)], GREY["top"])
    profile(c, [(2.2, 2.2), (7.8, 2.2), (7.8, 7.8), (2.2, 7.8)], 0.01, dash=None)
    return c


def icon_extrude():
    """A profile on the ground and the solid it became, rising off it."""
    c = Canvas()
    # The profile is wider than the solid on every side, so it survives being stood on. Drawn at
    # the same z, it would be hidden by the solid's own base and the icon would be a blue box.
    profile(c, [(-1.6, -1.6), (9.6, -1.6), (9.6, 9.6), (-1.6, 9.6)])
    solid(c, 0, 0, 0, 8, 8, 8.5, ACCENT)
    return c


def icon_revolve():
    """A section swept about an axis.

    The axis is drawn LAST and runs clear of the solid at both ends. Drawn first it disappears
    behind the result, and the icon becomes a blue cylinder — indistinguishable from the cylinder
    primitive, which is the one thing this icon must not look like. An arc over the top says which
    way the section travelled; without it a viewer sees a shape, not an operation.
    """
    c = Canvas()
    tube(c, 0, 0, 0, 6, 4.0, ACCENT)
    # The section being revolved, standing on the axis.
    profile(c, [(0.3, -0.35), (4.0, -0.35), (4.0, 0.35), (0.3, 0.35)], 6.05, dash=None)
    c.line((0, 0, -2.2), (0, 0, 9.4), GUIDE, dash="1.5 1.2", width=1.0)
    # A quarter arc above the cap, in the plane of the sweep.
    c.polyline([(4.6 * math.cos(math.radians(a)), 4.6 * math.sin(math.radians(a)), 8.0)
                for a in range(0, 105, 15)], GUIDE, width=1.1)
    return c


def icon_sweep():
    """A section carried along a path.

    The path is drawn LAST and extends past the solid at both ends, for the same reason the revolve
    axis is: a guide hidden behind the result stops being a guide. The bend matters too — a straight
    sweep is indistinguishable from an extrude at 24 px.
    """
    c = Canvas()
    solid(c, 3.2, 0.0, 0.6, 3.2, 3.2, 3.2, ACCENT)
    c.polyline([(0.2, 6.4, 2.2), (0.2, 1.6, 2.2), (7.4, 1.6, 2.2)], GUIDE, dash="1.3 1.0",
               width=1.2)
    # The section, square-on at the start of the path.
    profile(c, [(-0.9, 5.5), (1.3, 5.5), (1.3, 7.3), (-0.9, 7.3)], 2.2, dash=None)
    return c


def icon_loft():
    """Two profiles at different heights and the solid blending them.

    The lower profile is wide and the upper narrow, so the taper says "blend between sections"
    rather than "extrude" — the one thing that distinguishes loft at a glance.
    """
    c = Canvas()
    lower = [(0, 0, 0), (9, 0, 0), (9, 9, 0), (0, 9, 0)]
    upper = [(2.6, 2.6, 7), (6.4, 2.6, 7), (6.4, 6.4, 7), (2.6, 6.4, 7)]
    c.poly([lower[0], lower[1], upper[1], upper[0]], ACCENT["right"])
    c.poly([lower[1], lower[2], upper[2], upper[1]], ACCENT["left"])
    c.poly(upper, ACCENT["top"])
    # Last, so the section it lofts FROM reads as a section rather than a buried edge.
    profile(c, [(p[0], p[1]) for p in lower], 0.01)
    return c


def icon_box():
    """The primitive itself: grey, because a primitive is not an operation on anything."""
    c = Canvas()
    solid(c, 0, 0, 0, 8, 8, 8, GREY)
    return c


def icon_cylinder():
    c = Canvas()
    tube(c, 0, 0, 0, 8, 4.4, GREY)
    return c


def icon_hole():
    """A block with a bore through it. The bore is accent — it is what the operation makes."""
    c = Canvas()
    solid(c, 0, 0, 0, 9, 9, 5, GREY)
    # The mouth of the hole, sitting on the top face, plus a hint of its depth.
    disc(c, 4.5, 4.5, 5.02, 2.4, {"top": ACCENT["right"]})
    c.polyline([(4.5 - 2.4, 4.5, 5.02), (4.5 - 2.4, 4.5, 1.2)], ACCENT["left"], width=1.0)
    c.polyline([(4.5 + 2.4, 4.5, 5.02), (4.5 + 2.4, 4.5, 1.2)], ACCENT["left"], width=1.0)
    return c


ICONS = {
    "sketch": icon_sketch,
    "extrude": icon_extrude,
    "revolve": icon_revolve,
    "sweep": icon_sweep,
    "loft": icon_loft,
    "box": icon_box,
    "cylinder": icon_cylinder,
    "hole": icon_hole,
}


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "shell_qt/icons")
    out.mkdir(parents=True, exist_ok=True)
    for name, build in ICONS.items():
        (out / f"{name}.svg").write_text(build().render())
    print(f"wrote {len(ICONS)} icons to {out}")


if __name__ == "__main__":
    main()
