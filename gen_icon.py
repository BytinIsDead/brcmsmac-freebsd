#!/usr/bin/env python3
"""gen_icon.py -- generate a random generative-art SVG icon.

The composition is procedural: a seeded/unseeded PRNG chooses a colour
scheme, a layout grammar, motif geometry, layering and accents, then renders
a square SVG.  No image assets are used -- everything is derived from
random decisions, so every run with a different seed yields a different icon.

Usage:
    python3 gen_icon.py [SEED] > icon.svg

If SEED is omitted, a random seed is chosen and printed to stderr so the
result is reproducible.  Requires only the Python 3 standard library.
"""
import random
import sys

W = H = 256  # viewport (square). Resizable via the <svg width/height> attr.

# --- colour helpers -----------------------------------------------------------
def hsv2rgb(h, s, v):
    """Convert HSV (h in [0,360), s/v in [0,1]) to an 'rgb(r,g,b)' string."""
    c = v * s
    xx = c * (1 - abs((h / 60.0) % 2 - 1))
    m = v - c
    hp = int(h) % 360
    if hp < 60:
        r, g, b = c, xx, 0
    elif hp < 120:
        r, g, b = xx, c, 0
    elif hp < 180:
        r, g, b = 0, c, xx
    elif hp < 240:
        r, g, b = 0, xx, c
    elif hp < 300:
        r, g, b = xx, 0, c
    else:
        r, g, b = c, 0, xx
    ri, gi, bi = (round((x + m) * 255) for x in (r, g, b))
    return f"rgb({ri},{gi},{bi})"

def hue_range(base, spread, n):
    """Return n hues sampled evenly across [base-spread, base+spread]."""
    return [base - spread + (2 * spread * i) / (max(n - 1, 1)) for i in range(n)]

# --- layout grammars (random motif builders) ---------------------------------
def poly(rng, cx, cy, fill, stroke, sw):
    n = rng.randint(3, 9)
    r0, r1 = 34, 96
    pts = []
    for i in range(n):
        a = i / n * 6.2831 + rng.uniform(-0.05, 0.05)
        r = rng.uniform(r0, r1)
        pts.append(f"{cx + r * __import__('math').cos(a):.1f},"
                   f"{cy + r * __import__('math').sin(a):.1f}")
    return (f'<polygon points="{" ".join(pts)}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}" '
            f'stroke-linejoin="round"/>')

def rings(rng, cx, cy, colors):
    out = []
    for i, c in enumerate(colors):
        r = 22 + i * (86 / len(colors))
        out.append((f'<circle cx="{cx}" cy="{cy}" r="{r:.1f}" fill="none" '
                    f'stroke="{c}" stroke-width="{7 + (i % 3) * 6}"/>'))
    return "\n".join(out)

def rays(rng, cx, cy, colors):
    out = []
    for i, c in enumerate(colors):
        a0 = i * (360 / len(colors))
        a1 = a0 + (360 / len(colors)) * rng.uniform(0.5, 0.9)
        out.append((f'<path d="M {cx} {cy} L {cx + 128 * __import__("math").cos(__import__("math").radians(a0)):.1f} '
                    f'{cy + 128 * __import__("math").sin(__import__("math").radians(a0)):.1f} '
                    f'A 96 96 0 0 1 {cx + 128 * __import__("math").cos(__import__("math").radians(a1)):.1f} '
                    f'{cy + 128 * __import__("math").sin(__import__("math").radians(a1)):.1f} Z" '
                    f'fill="{c}"/>'))
    return "\n".join(out)

def burst(rng, cx, cy, colors):
    n = rng.randint(4, 16) * 2
    out = []
    for i in range(0, n, 2):
        c = colors[(i // 2) % len(colors)]
        a0 = i * (360 / n) + rng.uniform(-4, 4)
        a1 = (i + 1) * (360 / n) + rng.uniform(-4, 4)
        r = rng.uniform(70, 120)
        out.append((f'<path d="M {cx} {cy} L {cx + r * __import__("math").cos(__import__("math").radians(a0)):.1f} '
                    f'{cy + r * __import__("math").sin(__import__("math").radians(a0)):.1f} '
                    f'L {cx + 48 * __import__("math").cos(__import__("math").radians((a0 + a1) / 2)):.1f} '
                    f'{cy + 48 * __import__("math").sin(__import__("math").radians((a0 + a1) / 2)):.1f} Z" '
                    f'fill="{c}" opacity="0.85"/>'))
    return "\n".join(out)

def tiles(rng, cx, cy, colors, step, n):
    out = []
    for i in range(n):
        px = cx - (step * n) / 2 + i * step + rng.uniform(-3, 3)
        py = cy + rng.uniform(-3, 3)
        sz = step * rng.uniform(0.55, 0.95)
        out.append((f'<rect x="{px:.1f}" y="{py:.1f}" width="{sz:.1f}" '
                    f'height="{sz:.1f}" rx="{sz * 0.2:.1f}" fill="{colors[i % len(colors)]}" '
                    f'transform="rotate({rng.uniform(-14, 14)} {px + sz / 2:.1f} {py + sz / 2:.1f})"/>'))
    return "\n".join(out)

GRAMMARS = [poly, rings, rays, burst, tiles]

def main():
    if len(sys.argv) > 1:
        seed = int(sys.argv[1], 0)
    else:
        seed = random.randrange(1 << 31)
    print(f"# seed = {seed}", file=sys.stderr)
    rng = random.Random(seed)

    # random colour scheme
    base = rng.uniform(0, 360)
    spread = rng.choice([30, 60, 90, 120, 180])
    bg = hsv2rgb(base, rng.uniform(0.15, 0.55), rng.uniform(0.05, 0.22))
    fg = hue_range(base, min(spread, 150), 5)
    fg = [hsv2rgb(h, rng.uniform(0.55, 0.95), rng.uniform(0.6, 0.98)) for h in fg]
    accent = hsv2rgb((base + 180) % 360, 0.8, 0.95)
    sw = rng.uniform(1, 4)

    # random motif
    cx, cy = W / 2, H / 2
    grammar = rng.choice(GRAMMARS)
    if grammar is poly:
        motif = grammar(rng, cx, cy, rng.choice(fg), accent, sw)
    elif grammar is rings:
        motif = grammar(rng, cx, cy, fg)
    elif grammar is tiles:
        motif = grammar(rng, cx, cy, fg, rng.choice([22, 30, 38]),
                         rng.randint(3, 6))
    else:  # rays / burst
        motif = grammar(rng, cx, cy, fg)

    n_decor = rng.randint(1, 4)
    decor = []
    for _ in range(n_decor):
        d = rng.randint(2, 6)
        gx = rng.uniform(12, W - 12)
        gy = rng.uniform(12, H - 12)
        if d == 2:
            decor.append(f'<circle cx="{gx:.0f}" cy="{gy:.0f}" r="{rng.uniform(1.5, 3):.1f}" fill="{accent}"/>')
        elif d == 3:
            decor.append(f'<rect x="{gx:.0f}" y="{gy:.0f}" width="{rng.uniform(3, 6):.1f}" height="{rng.uniform(3, 6):.1f}" fill="{rng.choice(fg)}" transform="rotate({rng.uniform(0, 90):.0f} {gx:.0f} {gy:.0f})"/>')

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 {W} {H}">
  <rect width="{W}" height="{H}" fill="{bg}"/>
  <g opacity="0.94">
    {motif}
  </g>
  <g>
    {chr(10).join('    ' + x for x in decor)}
  </g>
</svg>
"""
    sys.stdout.write(svg)

if __name__ == "__main__":
    main()