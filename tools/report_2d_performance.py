#!/usr/bin/env python3
"""Show human-readable 2D frame timings from AOWIS_MAP_RENDER_DIAGNOSTICS logs.

All values are interval averages, not GPU timings. bgfx::frame() may wait for
VSync or a compositor; do not mistake that wall time for CPU or GPU workload.
"""
import argparse
import json
from pathlib import Path


def diagnostic_snapshots(text):
    snapshots = []
    for line in text.splitlines():
        start = line.find('{"')
        if start < 0:
            continue
        try:
            obj = json.loads(line[start:])
        except json.JSONDecodeError:
            continue
        if obj.get("kind") == "aowis_map_render_diagnostics":
            snapshots.append(obj)
    return snapshots


def difference(current, previous, key, group):
    a = current.get(group, {}).get(key, 0)
    b = previous.get(group, {}).get(key, 0) if previous else 0
    return max(0, a - b)


def intervals(snapshots):
    previous = None
    for snapshot in snapshots:
        if previous is not None and snapshot.get("elapsed_ms", 0) <= previous.get("elapsed_ms", 0):
            previous = None
        elapsed_ms = snapshot["elapsed_ms"] - (previous["elapsed_ms"] if previous else 0)
        frames = difference(snapshot, previous, "frames", "counts")
        if elapsed_ms <= 0 or frames <= 0:
            previous = snapshot
            continue

        def ms_per_frame(name):
            return difference(snapshot, previous, name, "total_duration_ms") / frames

        yield {
            "elapsed_s": snapshot["elapsed_ms"] / 1000.0,
            "fps": frames * 1000.0 / elapsed_ms,
            "frames": frames,
            "camera_changes": difference(snapshot, previous, "center_change", "counts")
                              + difference(snapshot, previous, "zoom_change", "counts"),
            "frame_ms": ms_per_frame("frames"),
            "present_ms": ms_per_frame("bgfx_present"),
            "imagery_ms": ms_per_frame("bgfx_2d_imagery"),
            "network_ms": ms_per_frame("bgfx_2d_network_draw"),
            "mesh_ms": ms_per_frame("bgfx_2d_mesh_sync"),
            "styles_ms": ms_per_frame("bgfx_2d_style_sync"),
            "labels_ms": ms_per_frame("bgfx_2d_labels"),
            "hud_ms": ms_per_frame("bgfx_hud"),
        }
        previous = snapshot


def make_report(snapshots):
    if not snapshots:
        return "No AOWIS map diagnostics found. Set AOWIS_MAP_RENDER_DIAGNOSTICS=1."
    if not any("bgfx_present" in s.get("counts", {}) for s in snapshots):
        return ("This log predates detailed 2D phase timers. Rebuild the patched GUI "
                "and capture another run with AOWIS_BGFX_DEBUG=0.")
    rows = list(intervals(snapshots))
    if not rows:
        return "No completed frame intervals found."
    lines = [
        "2D frame timing (wall clock; BGFX presentation includes VSync wait)",
        " end(s)    FPS    cameraΔ   frame ms   present ms    imagery ms   mesh ms   network ms   style ms  labels ms  HUD ms",
    ]
    for r in rows:
        lines.append(
            f"{r['elapsed_s']:7.1f}  {r['fps']:6.1f}  {r['camera_changes']:9d}  "
            f"{r['frame_ms']:9.2f}  {r['present_ms']:11.2f}  "
            f"{r['imagery_ms']:11.2f}  {r['mesh_ms']:8.2f}  "
            f"{r['network_ms']:10.2f}  {r['styles_ms']:9.2f}  "
            f"{r['labels_ms']:9.2f}  {r['hud_ms']:6.2f}"
        )
    maximum = snapshots[-1].get("max_duration_ms", {}).get("frames")
    if maximum is not None:
        lines.append(f"Worst reported frame (cumulative): {maximum:.2f} ms")
    lines.append("Note: `frame ms` is not CPU rendering time; includes bgfx::frame and possible compositor waits.")
    lines.append("Capture both a stationary map and active pan/zoom; use a representative network.")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    print(make_report(diagnostic_snapshots(args.log.read_text(errors="replace"))))


if __name__ == "__main__":
    main()
