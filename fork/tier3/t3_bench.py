"""Tier 3's benchmark scenarios (ADR-103's S1 to S6): kmscon's render-timer
lines on the guest's console, and the figures of one scenario's window.

A benchmark screen script reports its window on the event port as
"T3-BENCH NAME lines=N start=S end=E", S and E on the guest's monotonic clock
(/proc/uptime), the clock that also stamps kmscon's console lines. kmscon,
run with render-timer=log, writes one line per display per second, each
covering the second before its stamp; the scenario's figures come from the
lines whose seconds overlap the window.
"""

import re
import statistics

# "[   12.345678] kmscon[505]: terminal: render-timer: display=... key=value ..."
# A console line can follow other output on the same line, such as a prompt.
_TIMER = re.compile(r"\[\s*(\d+\.\d+)\] kmscon\[(\d+)\]: terminal: render-timer: (.*)$")
_BENCH = re.compile(r"T3-BENCH (\S+) lines=(\d+) start=(\d+\.\d+) end=(\d+\.\d+)")

# kmscon logs this for a key it does not know, as a build without the
# render timer (upstream's) does for render-timer.
_UNKNOWN = "unknown config option 'render-timer'"


def timer_lines(text):
    """The render-timer lines in console text, in order, as dicts: "t" (the
    stamp, in seconds), "pid", "display", and each number by its key. A line
    another writer broke into on the serial console, such as the getty's
    terminal queries at boot, is left out."""
    out = []
    for line in text.splitlines():
        m = _TIMER.search(line.strip())
        if not m:
            continue
        entry = {"t": float(m.group(1)), "pid": int(m.group(2))}
        try:
            for field in m.group(3).split():
                key, _, value = field.partition("=")
                entry[key] = value if key == "display" else float(value)
        except ValueError:
            continue
        out.append(entry)
    return out


def bench_window(line):
    """(name, lines, start, end) from a T3-BENCH event line, or None."""
    m = _BENCH.search(line)
    if not m:
        return None
    return m.group(1), int(m.group(2)), float(m.group(3)), float(m.group(4))


def summarize(lines, count, start, end):
    """The figures of the window [start, end] in which `count` lines of text
    were printed, from the render-timer lines that overlap it.

    Flips and frames are counted over the overlapping seconds and divided by
    the window's length; the screen script pauses before and after the
    window, so those seconds hold no other frames. CPU time and damage are
    means per frame over the window; the median is the median of the
    seconds' medians and the 95th percentile the largest second's, as the
    lines carry no single frames.
    """
    seconds = end - start
    window = [x for x in lines if x["t"] > start and x["t"] - x["interval_ms"] / 1000 < end]
    out = {"lines": count, "seconds": round(seconds, 2),
           "lines_per_s": round(count / seconds) if seconds > 0 else None,
           "intervals": len(window), "displays": sorted({x["display"] for x in window})}
    if not window or seconds <= 0:
        return out
    flips = sum(x["flips"] for x in window)
    frames = sum(x["frames"] for x in window)
    busy = [x for x in window if x["frames"]]
    # the seconds wholly inside the window, while the screen's output flowed
    inside = [x for x in window if x["t"] - x["interval_ms"] / 1000 >= start and x["t"] <= end]
    skipped = sum(x["skipped"] for x in window)
    out.update({
        "flips": int(flips), "frames": int(frames),
        "presented_per_s": round(flips / seconds, 1),
        "skipped": int(skipped), "skipped_per_s": round(skipped / seconds, 1),
        "intervals_inside": len(inside), "flips_inside": int(sum(x["flips"] for x in inside)),
    })
    if frames:
        out.update({
            "cpu_ms_mean": round(sum(x["cpu_ms_mean"] * x["frames"] for x in busy) / frames, 3),
            "cpu_ms_median": round(statistics.median(x["cpu_ms_median"] for x in busy), 3),
            "cpu_ms_p95": max(x["cpu_ms_p95"] for x in busy),
            "cpu_ms_max": max(x["cpu_ms_max"] for x in busy),
            "damage_px_mean": round(sum(x["damage_px_mean"] * x["frames"] for x in busy) / frames),
            "damage_pct": round(sum(x["damage_pct"] * x["frames"] for x in busy) / frames, 2),
        })
    return out


# The flood screen's bounds (#147). A stream of sixel pictures changes the
# screen only at each picture's end, so a draw tried after every pty read
# finds nothing to draw and counts as skipped (#139); kmscon draws once per
# loop turn, and only a screen that changed.
FLOOD_MAX_SKIPPED_PER_S = 20


def flood_verdict(figures):
    """Why the flood screen's figures fail, or an empty list: a frame
    presented in a second wholly inside the window, while the stream flowed,
    and at most FLOOD_MAX_SKIPPED_PER_S passes a second that drew nothing."""
    why = []
    if not figures.get("intervals_inside"):
        why.append("no render-timer second lies wholly inside the window")
    elif not figures.get("flips_inside"):
        why.append("no frame was presented while the stream flowed")
    if figures.get("skipped_per_s", 0) > FLOOD_MAX_SKIPPED_PER_S:
        why.append("%.1f passes a second drew nothing, over %d"
                   % (figures["skipped_per_s"], FLOOD_MAX_SKIPPED_PER_S))
    return why


def measure(serial_text, event_line, config):
    """The report entry of one benchmark screen: its figures, or why there
    are none. "error", which fails the case, is set when a build with the
    render timer configured logged no line for the window, and when the
    flood screen's figures are out of bounds (flood_verdict)."""
    window = bench_window(event_line or "")
    if window is None:
        return {"error": "the screen reported no benchmark window: %r" % event_line}
    name, count, start, end = window
    out = summarize(timer_lines(serial_text), count, start, end)
    if out["intervals"]:
        why = flood_verdict(out) if name == "flood" else []
        if why:
            out["error"] = "; ".join(why)
        return out
    if _UNKNOWN in serial_text:
        out["note"] = "this build has no render timer"
    elif not any(re.fullmatch(r"render-timer=(log|both)", c) for c in config):
        out["note"] = "render-timer is not configured for this variant"
    else:
        out["error"] = "no render-timer line overlaps the window %.2f to %.2f s" % (start, end)
    return out
