"""Estimate exact-content pooling from daily Chatterino logs, using stdlib only.

Reads logs without modifying them. Outputs aggregate statistics, never chat text
or usernames. Memory models are estimates, not process heap measurements.
"""

import argparse
from array import array
from collections import Counter, OrderedDict
from datetime import date, datetime, time, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re


LINE = re.compile(
    r"^\[(\d{2}):(\d{2}):(\d{2})\] ((?:\S+ )?[A-Za-z0-9_]{1,25}): (.*)$"
)
MODEL = {
    "qt_array_header_bytes": 16,
    "allocator_allowance_bytes": 16,
    "allocation_alignment_bytes": 16,
    "pool_entry_bytes": 64,
    "parsed_payload_reference_bytes_per_message": 16,
    "shared_payload_overhead_bytes_per_distinct_body": 80,
    "text_element_bytes": 128,
    # Filled from the local release sizeof probe when using --emote-element-bytes.
    "emote_element_bytes": None,
    "fixed_message_and_one_view_bytes_per_message": 792,
}


def align16(size):
    return (size + 15) // 16 * 16


def units(text):
    return len(text.encode("utf-16-le")) // 2


def string_allocation(text):
    # Tight QString allocation plus a transparent allocator allowance. Real
    # capacities, shared substrings, and allocator size classes can differ.
    return align16(16 + 2 * (units(text) + 1)) + 16 if text else 0


def lru_model(bodies, capacity, min_units):
    pool = OrderedDict()
    allocated = 0
    hits = 0
    for body in bodies:
        if units(body) < min_units:
            allocated += string_allocation(body)
            continue
        if body in pool:
            pool.move_to_end(body)
            hits += 1
        else:
            allocated += string_allocation(body)
            pool[body] = None
            if len(pool) > capacity:
                pool.popitem(last=False)
    baseline = sum(string_allocation(body) for body in bodies)
    return {
        "capacity": capacity,
        "minimum_utf16_units": min_units,
        "hits": hits,
        "entries_at_end": len(pool),
        "gross_saved_bytes": baseline - allocated,
        "index_allowance_bytes": 64 * len(pool),
        "net_saved_bytes": baseline - allocated - 64 * len(pool),
    }


def analyze_window(bodies, timestamps, label):
    counts = Counter(bodies)
    count = len(bodies)
    baseline_chars = sum(2 * units(body) * n for body, n in counts.items())
    unique_chars = sum(2 * units(body) for body in counts)
    baseline_alloc = sum(string_allocation(body) * n for body, n in counts.items())
    unique_alloc = sum(string_allocation(body) for body in counts)
    repeated = [(body, n) for body, n in counts.items() if n > 1]
    char_savings = baseline_chars - unique_chars
    allocation_savings = baseline_alloc - unique_alloc
    result = {
        "label": label,
        "messages": count,
        "first_logged_timestamp": datetime.fromtimestamp(timestamps[0], timezone.utc)
        .replace(tzinfo=None).isoformat(),
        "last_logged_timestamp": datetime.fromtimestamp(timestamps[-1], timezone.utc)
        .replace(tzinfo=None).isoformat(),
        "distinct_bodies": len(counts),
        "duplicate_occurrences": count - len(counts),
        "duplicate_occurrence_fraction": (count - len(counts)) / count,
        "messages_in_repeated_body_groups": sum(n for _, n in repeated),
        "largest_exact_repeat_count": max(counts.values()),
        "mean_body_utf16_units": baseline_chars / 2 / count,
        "body_utf16_bytes": baseline_chars,
        "ideal_body_utf16_saved_bytes": char_savings,
        "ideal_body_utf16_saved_fraction": char_savings / baseline_chars,
        "body_allocation_model_bytes": baseline_alloc,
        "ideal_body_allocation_gross_saved_bytes": allocation_savings,
        "full_pool_index_allowance_bytes": len(counts) * 64,
        "full_pool_body_allocation_net_saved_bytes": allocation_savings - len(counts) * 64,
        "fixed_subtotal_bytes": count * 792,
        "single_token_duplicate_occurrences": sum(
            n - 1 for body, n in repeated if " " not in body
        ),
        "long_body_duplicate_occurrences": sum(
            n - 1 for body, n in repeated if units(body) >= 80
        ),
        "long_body_utf16_saved_bytes": sum(
            2 * units(body) * (n - 1) for body, n in repeated if units(body) >= 80
        ),
    }
    # Treat all body words as text, or all as emotes. These are sensitivity
    # scenarios, not classifications of what the historical client rendered.
    if MODEL["emote_element_bytes"] is not None:
        scenarios = {}
        for kind in ("all_text", "all_emotes"):
            def payload_cost(body):
                words = body.split(" ")
                if kind == "all_text":
                    # One merged TextElement, one QStringList of word handles,
                    # one element-vector slot, and independently allocated words.
                    elements = (128 + 16 + align16(16 + 24 * len(words)) + 16
                                + 8 + sum(string_allocation(word) for word in words))
                else:
                    elements = len(words) * (MODEL["emote_element_bytes"] + 16 + 8)
                return string_allocation(body) + elements

            baseline = sum(payload_cost(body) * n for body, n in counts.items())
            saved = sum(payload_cost(body) * (n - 1) for body, n in counts.items())
            net = saved - len(counts) * (64 + 80) - count * 16
            scenarios[kind] = {
                "baseline_body_model_bytes": baseline,
                "gross_saved_bytes": saved,
                "net_saved_bytes": net,
                "net_fraction_of_fixed_plus_body_model": net / (count * 792 + baseline),
            }
        result["parsed_body_sensitivity_scenarios"] = scenarios
    result["bounded_text_pools"] = [
        lru_model(bodies, 8192, minimum) for minimum in (0, 32, 80)
    ]
    return result


def analyze_channel(root, channel, days):
    directory = root / channel
    file_pattern = re.compile(re.escape(channel) + r"-(\d{4}-\d{2}-\d{2})\.log$")
    files = []
    for path in directory.iterdir():
        match = file_pattern.fullmatch(path.name)
        if match:
            files.append((date.fromisoformat(match[1]), path))
    files.sort()
    last_date = files[-1][0]
    first_date = last_date - timedelta(days=days - 1)
    files = [(day, path) for day, path in files if day >= first_date]
    bodies = []
    timestamps = array("q")
    manifest = []
    by_minute = Counter()
    malformed = 0
    backwards = 0
    for day, path in files:
        hasher = hashlib.sha256()
        accepted = 0
        excluded = 0
        start = int(datetime.combine(day, time(), timezone.utc).timestamp())
        with path.open("rb") as stream:
            for raw in stream:
                hasher.update(raw)
                line = raw.decode("utf-8-sig").rstrip("\r\n")
                match = LINE.fullmatch(line)
                if match is None:
                    excluded += 1
                    if line.startswith("["):
                        malformed += 1
                    continue
                h, m, s = map(int, match.group(1, 2, 3))
                stamp = start + h * 3600 + m * 60 + s
                if timestamps and stamp < timestamps[-1]:
                    backwards += 1
                timestamps.append(stamp)
                bodies.append(match[5])
                by_minute[stamp // 60] += 1
                accepted += 1
        manifest.append({
            "file": path.name, "bytes": path.stat().st_size,
            "sha256": hasher.hexdigest(), "accepted_user_lines": accepted,
            "excluded_lines": excluded,
        })
    windows = []
    for size in (100000, 250000, 500000, 1000000):
        if len(bodies) >= size:
            windows.append(analyze_window(bodies[-size:], timestamps[-size:], f"latest_{size}"))
    for size in (1000, 10000, 100000):
        if len(bodies) < size:
            continue
        stride = max(1, size // 100)
        # Search every 1000 messages, avoiding a quadratic counter calculation.
        candidates = range(0, len(bodies) - size + 1, stride)
        best = min(candidates, key=lambda i: timestamps[i + size - 1] - timestamps[i])
        windows.append(analyze_window(bodies[best:best + size], timestamps[best:best + size],
                                      f"fastest_{size}_stride_{stride}"))
    if len(bodies) < 1000:
        windows.append(analyze_window(bodies, timestamps, "all_available"))
    return {
        "channel": channel,
        "first_date": first_date.isoformat(), "last_date": last_date.isoformat(),
        "messages": len(bodies), "timestamp_regressions": backwards,
        "unparsed_timestamped_lines": malformed,
        "peak_messages_in_calendar_minute": max(by_minute.values()),
        "daily_files": manifest, "windows": windows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-root", type=Path, required=True)
    parser.add_argument("--channels", nargs="+", required=True)
    parser.add_argument("--days", type=int, default=30)
    parser.add_argument("--emote-element-bytes", type=int)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    MODEL["emote_element_bytes"] = args.emote_element_bytes
    results = {
        "method": "Exact body equality, preserving case, whitespace and invisible characters.",
        "timestamp_note": "Logged local date/time stored as naive time; no timezone conversion.",
        "models": MODEL, "channels": [],
    }
    for channel in args.channels:
        result = analyze_channel(args.log_root, channel, args.days)
        results["channels"].append(result)
        print(f"{channel}: {result['messages']:,} user lines, {len(result['daily_files'])} daily logs",
              flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
