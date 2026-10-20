#!/usr/bin/env python3
"""Summarise a directory of shmipc_bench JSON results as Markdown tables.

    bench/scripts/report.py bench/results/<label> [more dirs...]

Standard library only, so it runs anywhere the benchmark runs.
"""
import json
import pathlib
import sys

ORDER = {"pingpong": 0, "oneway": 1, "throughput": 2}


def load(dirs):
    rows = []
    for d in dirs:
        for f in sorted(pathlib.Path(d).glob("*.json")):
            with open(f) as fh:
                rows.append(json.load(fh))
    return rows


def env_block(env):
    lines = [
        f"- OS: {env['os']}" + (" (container)" if env["in_container"] else ""),
        f"- CPU: {env['cpu_model']}, {env['online_cpus']} logical CPUs",
        f"- Compiler: {env['compiler']} ({env['build']})",
        f"- Counter: {env['counter_hz'] / 1e6:.0f} MHz nominal, "
        f"**{env['counter_resolution_ns']:.1f} ns measured resolution**",
    ]
    if env.get("governor"):
        lines.append(f"- Governor: {env['governor']}")
    if not env["affinity_supported"]:
        lines.append("- Thread pinning: **not supported on this OS**; results are unpinned")
    return "\n".join(lines)


def main(argv):
    rows = load(argv[1:] or ["."])
    if not rows:
        print("no results", file=sys.stderr)
        return 1
    rows.sort(key=lambda r: (ORDER.get(r["benchmark"], 9), r["mode"], r["msg_size"], r["transport"]))

    print("## Environment\n")
    print(env_block(rows[0]["env"]))
    cpus = rows[0]["cpus"]
    print(f"- CPUs: {cpus[0]},{cpus[1]}; pinned: {all(r['pinned'] for r in rows)}\n")

    lat = [r for r in rows if "latency_ns" in r]
    if lat:
        print("## Latency (ns)\n")
        print("pingpong = round trip; oneway = open-loop, scheduled send to receive.\n")
        print("| bench | mode | size | transport | p50 | p99 | p99.9 | p99.99 | max | notes |")
        print("|---|---|---:|---|---:|---:|---:|---:|---:|---|")
        for r in lat:
            l = r["latency_ns"]
            notes = f"rate={r['rate']}/s late={r['late_sends']}" if r["benchmark"] == "oneway" else ""
            print(f"| {r['benchmark']} | {r['mode']} | {r['msg_size']} | {r['transport']} | "
                  f"{l['p50']} | {l['p99']} | {l['p99.9']} | {l['p99.99']} | {l['max']} | {notes} |")
        print()

    tput = [r for r in rows if "throughput" in r]
    if tput:
        print("## Throughput\n")
        print("| mode | size | transport | Mmsg/s | MB/s |")
        print("|---|---:|---|---:|---:|")
        for r in tput:
            t = r["throughput"]
            print(f"| {r['mode']} | {r['msg_size']} | {r['transport']} | "
                  f"{t['msgs_per_sec'] / 1e6:.1f} | {t['mb_per_sec']:.0f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
