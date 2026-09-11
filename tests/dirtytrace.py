#!/usr/bin/env python3
"""dirtytrace.py -- analyse an [dirty] trace from a desktop FSCI_PROBE_DIRTY build.

Purpose: settle Pico SINGLE-BUFFER render questions offline. The rect bookkeeping
(_gfxop_draw_cel_buffer's static_buf skip, _gfxop_add_dirty,
_gfxop_buffer_propagate_box) is SHARED code, so a desktop run executes the exact
same sequence the Pico driver does -- desktop just cannot show the artifact,
because its GFX_BUFFER_STATIC draws land in the off-screen visual[2] while Pico
writes the displayed visual[0].

Capture:
  cmake -B build-dirty -DPLATFORM=desktop -DFSCI_PROBE_DIRTY=ON -DFSCI_SIM_PICO_STATIC=ON
  cmake --build build-dirty -j$(nproc)
  SDL_VIDEODRIVER=dummy FREESCI_DIRTYPROBE=1 \
    ./build-dirty/src/freesci --gamedir <game> --graphics sdl --disable-mouse --run 2> dirty.log
  tests/dirtytrace.py dirty.log

Reports, per static (GFX_BUFFER_STATIC) cel draw:
  paired   -- a non-static draw of the SAME cel at the SAME rect follows it in the
              same frame. Both widget paths (_gfxwop_pic_view_draw and the Pico
              stopUpd route in _gfxwop_dyn_view_draw) fall through to gfxop_draw_cel,
              so this is expected to be ~100%.
  covered  -- the static draw's rect is covered by a +dirty rect in the same frame.
              _gfxop_add_dirty uses the UNCLIPPED cel rect, so a paired draw alone
              already registers it -- which is the point: it means the region is
              NOT untracked, and adding a dirty rect for static_buf would be a no-op.
"""
import re
import sys
from collections import defaultdict

CEL = re.compile(r"\[dirty\] cel   static=(\d) view=(\S+) rect=\((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)")
BOX = re.compile(r"\[dirty\] box   \((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)")
DIRTY = re.compile(r"\[dirty\] \+dirty \((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)(.*)")
PROP = re.compile(r"\[dirty\] prop  buf=(\w+) \((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)")
FRAME = re.compile(r"\[dirty\] === update \(frame (\d+)\)")


def covers(outer, inner):
    ox, oy, ow, oh = outer
    ix, iy, iw, ih = inner
    return ox <= ix and oy <= iy and ox + ow >= ix + iw and oy + oh >= iy + ih


def overlaps(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def main(path):
    def newframe():
        return {"static": [], "normal": [], "dirty": [], "prop": [], "seq": []}

    frames = []          # list of dicts per frame
    cur = newframe()
    suppressed = 0

    for line in open(path, errors="replace"):
        if FRAME.search(line):
            frames.append(cur)
            cur = newframe()
            continue
        m = BOX.search(line)
        if m:
            cur["seq"].append(("box", tuple(int(m.group(i)) for i in (1, 2, 3, 4))))
            continue
        m = CEL.search(line)
        if m:
            rect = tuple(int(m.group(i)) for i in (3, 4, 5, 6))
            cur["static" if m.group(1) == "1" else "normal"].append((m.group(2), rect))
            if m.group(1) == "1":
                cur["seq"].append(("static", rect, m.group(2)))
            continue
        m = DIRTY.search(line)
        if m:
            if "SUPPRESSED" in m.group(5):
                suppressed += 1
            else:
                cur["dirty"].append(tuple(int(m.group(i)) for i in (1, 2, 3, 4)))
            continue
        m = PROP.search(line)
        if m:
            cur["prop"].append((m.group(1),
                                tuple(int(m.group(i)) for i in (2, 3, 4, 5))))
    frames.append(cur)

    n_static = n_paired = n_covered = 0
    unpaired = defaultdict(int)
    uncovered = defaultdict(int)

    for f in frames:
        normals = set(f["normal"])
        for view, rect in f["static"]:
            n_static += 1
            if (view, rect) in normals:
                n_paired += 1
            else:
                unpaired[view] += 1
            if any(covers(d, rect) for d in f["dirty"]):
                n_covered += 1
            else:
                uncovered[view] += 1

    # The bleed signature: a STATIC cel painted AFTER, and overlapping, a box
    # drawn earlier in the SAME frame. On desktop that lands in the off-screen
    # visual[2] and is harmless; on Pico it lands in the displayed visual[0] and
    # overpaints the dialog. Draw ORDER is what protects the box, not the clip.
    bleeds, bleed_frames, examples = 0, 0, []
    for fi, f in enumerate(frames):
        boxes, hit = [], False
        for item in f["seq"]:
            if item[0] == "box":
                boxes.append(item[1])
            else:
                for b in boxes:
                    if overlaps(item[1], b):
                        bleeds += 1
                        hit = True
                        if len(examples) < 8:
                            examples.append((fi, item[2], item[1], b))
                        break
        if hit:
            bleed_frames += 1

    back = sum(1 for f in frames for b, _ in f["prop"] if b == "BACK")
    front = sum(1 for f in frames for b, _ in f["prop"] if b == "FRONT")

    print(f"frames                      {len(frames)}")
    print(f"static (GFX_BUFFER_STATIC)  {n_static}")
    print(f"  paired with a normal draw {n_paired}"
          + (f"  ({100.0*n_paired/n_static:.1f}%)" if n_static else ""))
    print(f"  rect covered by a +dirty  {n_covered}"
          + (f"  ({100.0*n_covered/n_static:.1f}%)" if n_static else ""))
    print(f"dirty rects suppressed      {suppressed} (disable_dirty)")
    print(f"buffer propagates           BACK {back} / FRONT {front}")
    print(f"BLEED: static drawn after an overlapping box"
          f"  {bleeds}  (in {bleed_frames} frames)")
    for fi, view, r, b in examples:
        print(f"   frame {fi}: static view {view} {r} over box {b}")

    if unpaired:
        print("\nstatic draws with NO paired normal draw (genuinely untracked):")
        for v, c in sorted(unpaired.items(), key=lambda kv: -kv[1])[:15]:
            print(f"  view {v:<12} {c}")
    if uncovered:
        print("\nstatic draws whose rect NO +dirty covers:")
        for v, c in sorted(uncovered.items(), key=lambda kv: -kv[1])[:15]:
            print(f"  view {v:<12} {c}")
    if not unpaired and not uncovered and n_static:
        print("\nEvery static draw is paired AND dirty-covered: the region is already"
              "\ntracked, so registering a dirty rect at the static_buf skip would be"
              "\na no-op. The bleed must have another mechanism.")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "dirty.log")
