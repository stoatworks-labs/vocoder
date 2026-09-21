#!/usr/bin/env python3
"""Every control must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range and report any that made no
difference at all.

    python3 tools/sweep.py [--binary build/vctest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**This plugin's null is the identity, so half the controls are conditional on
another one.** With the defaults -- every band at 1x -- the output is the input,
to the bit. That is the whole point of the design, and it means:

- **Mix** blends the input with a reconstruction that IS the input, so it moves
  nothing until some band is not at 1x;
- **Carrier** picks whether the bands act on RGB or on luma, and both are exact
  identities until some band is not at 1x;
- **Master** scales a reconstruction that is the input, so it is live on the
  defaults -- it is the one output control that is.

**The Audio group needs audio.** Nothing in this repo can route a spectrum: the
host is the only thing that ever fills an FFT buffer parameter. `vctest --feed`
writes a synthetic one, and every Audio control carries it in its context. Drive,
Floor and Mapping also need the spectrum to be UNEVEN across the bands, which
the feed's is.

**Attack and Release need a transient, and opposite sides of it.** A follower
has one coefficient at a time, and the sweep compares the last frame of a run:
sampled in the decay, Attack provably does nothing, because the envelope has
long since converged on the falling signal. It was reported DEAD for exactly
that reason and the sweep was right. The feed's kick retriggers every half
second, so Attack is swept one frame past a rising edge and Release deep in a
decay, each with the other set fast.

**Audio itself is skipped.** It is the FFT buffer: its scalar value is
meaningless, sweeping it proves nothing, and the plugin reads its *elements*.
`--feed` is what proves those reach the picture.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
# tools/verify.sh builds into build-verify, so this is settable. Default is the
# dev build, which is what a person at a terminal has.
BIN = str(ROOT / "build" / "vctest")
SCRATCH = tempfile.mkdtemp(prefix="vcsweep")

WIDTH, HEIGHT = 640, 360
FRAMES = 2

def active_levels(width, height):
    """How many pyramid levels this raster has.

    The same rule as ActiveLevels in source/Pyramid.cpp: halve until a side
    would go below two pixels. Kept in step with it by hand, which is cheap
    because the rule is three lines and has no reason to move.
    """
    levels = 0
    w, h = width, height
    for _ in range(8):
        w, h = (w + 1) // 2, (h + 1) // 2
        if min(w, h) < 2:
            break
        levels += 1
    return levels


def band_level(name):
    """The level a 'Band N (… px)' slider drives, or None for anything else."""
    if not name.startswith("Band "):
        return None
    try:
        return int(name.split()[1])
    except (IndexError, ValueError):
        return None


# Parameters that cannot be swept, with the reason.
SKIP = {
    "Audio": "the FFT buffer: its scalar value is meaningless and the plugin "
             "reads its elements. --feed is what proves those reach the picture",
}

# A band away from unity, so that a reconstruction differs from the input at
# all. Band 3 is the 4 px band, which the card's noise patch and every hard
# edge in it have plenty of.
CUT = {"Band 3 (4 px)": 0.0}

# Audio: a synthetic spectrum, and long enough to straddle a beat.
FED = {"_feed": 1.0, "_frames": 20}

CONTEXT = {
    # The output controls, against a reconstruction that is not the input.
    "Mix": CUT,
    "Carrier": CUT,
    # Tilt moves every band about the middle of the bank; on the defaults that
    # is already a change, so it needs nothing.
    # The Audio group. Floor and Drive scale a gain that is only interesting
    # when the bands are not all reading the same level, which the feed sees to.
    "Drive": dict(FED, **{"Floor": 0.0}),
    "Floor": FED,
    "Mapping": dict(FED, **{"Floor": 0.0}),
    "Sidechain Mode": dict(FED, **{"Floor": 0.0}),
    # Both time constants need the transient, and they need OPPOSITE SIDES of
    # it, because the sweep compares the last frame of the run and a follower
    # only has one coefficient at a time.
    #
    # The feed's kick retriggers every half second and decays in between, so
    # frame 31 (t = 0.517 s) is one frame past a rising edge and frame 19
    # (t = 0.317 s) is deep in a decay. Attack sampled in the decay is DEAD and
    # the sweep said so, correctly: by then the envelope has converged on the
    # falling signal and how fast it rose two hundred milliseconds earlier
    # leaves no trace. Each is swept with the other set fast, so the state it
    # inherits is the same in both runs and the only difference is its own.
    "Attack": dict(FED, **{"Floor": 0.0, "Release": 0.0, "_frames": 32}),
    "Release": dict(FED, **{"Floor": 0.0, "Attack": 0.0, "_frames": 20}),
}


def parameters():
    """id, name, default from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines()[1:]:
        m = re.match(r"\s*(\d+)\s+(.*?)\s+([\d.eE+-]+)\s*$", line)
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), float(m.group(3))))
    return found


def render(path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    if "_feed" in overrides:
        args += ["--feed", str(overrides["_feed"])]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]

    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, context = job

    low = dict(context)
    high = dict(context)
    low[name] = context.get("_low", 0.0)
    high[name] = context.get("_high", 1.0)

    a = render(f"{SCRATCH}/{pid}_lo.png", low)
    b = render(f"{SCRATCH}/{pid}_hi.png", high)
    fraction, count = difference(a, b)
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=BIN)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)
    BIN = args.binary

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built -- cmake --build build")
        return 1

    skipped = []
    work = []
    for pid, name, _default in parameters():
        # The About block is declared last: a text line and then one browser
        # button each. Everything from there down is skipped.
        if name == "About":
            skipped.append((name, "the About block: a text line and browser buttons"))
            break
        if name in SKIP:
            skipped.append((name, SKIP[name]))
            continue
        # A band slider for a level this raster is too small to have is not a
        # dead control, it is an absent one. The pyramid stops halving when a
        # side would fall below two pixels, so the number of bands is a property
        # of the picture size: eight at 640x360 and above, seven at 320x180.
        # CI sweeps small to stay inside a GPU-less runner's patience, so
        # without this the coarsest slider reads DEAD there and nowhere else.
        band = band_level(name)
        if band is not None and band > active_levels(WIDTH, HEIGHT):
            skipped.append((name, "no such band at %dx%d: the pyramid has %d "
                                  "levels here, and this is level %d"
                                  % (WIDTH, HEIGHT, active_levels(WIDTH, HEIGHT), band)))
            continue
        work.append((pid, name, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
