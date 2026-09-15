"""Build web/data/tube-directions.json: the direction names TfL uses on each Tube
line at each station.

The Tube picker has to offer names the firmware will match, and the only place
TfL publishes them is the live arrivals feed ("Eastbound - Platform 1"). Sampling
that feed at the moment someone picks a station misses any direction with no
train due right then (#8). A fixed pair per line does not work either: more than
50 station-and-line pairs use other names - Inner and Outer Rail on the Hainault
loop, Eastbound and Westbound on the Jubilee east of Westminster, "Platform 2" at
Edgware Road. So the whole network is swept several times, the union of what each
station reports is kept, and both pickers merge this table with a live sample.

Sweeping still only sees directions that had a train due at the time, so a through
station seen with a single name gets its counterpart added: every Tube line runs
both ways through a station that is not the end of a route. Acton Town, during a
District gap, was seen as Eastbound only - exactly #8. Route ends, and stations
already seen with two or more names, are kept exactly as observed.

A station never seen on one of the Circle, District and Hammersmith & City borrows
the names seen there on the others: those three agree at every station they share
except Edgware Road, Baker Street and Aldgate, which the sweeps see anyway. So the
District at Bayswater gets the Circle's Inner and Outer Rail rather than a guess.

    python web/build-tube-directions.py                        8 sweeps, 90 s apart
    python web/build-tube-directions.py --sweeps 1 --interval 0 --out x.json

Runs are additive: names already in the file are kept, so a direction that was
quiet during one run is not dropped by it.
"""
import argparse
import datetime
import json
import os
import re
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "data", "tube-directions.json")
API = "https://api.tfl.gov.uk"
HEADERS = {"User-Agent": "DepartureBuddy-build"}   # TfL answers 403 without one


def get(path, attempts=6):
    """GET a Unified API path as JSON.

    Without an app key TfL allows about 50 requests a minute and answers the
    rest with HTTP 429. A sweep is a burst of them, so a 429 is waited out
    rather than letting the line drop out of the table."""
    for attempt in range(attempts):
        req = urllib.request.Request(API + path, headers=HEADERS)
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code != 429 or attempt == attempts - 1:
                raise
            try:
                wait = int(e.headers.get("Retry-After") or 0)
            except ValueError:
                wait = 0
            wait = wait or 15 * (attempt + 1)
            print("  rate limited on %s; waiting %ds" % (path, wait))
            time.sleep(wait)


def token(platform_name):
    """The part of a platformName before " - ", as tube_api.cpp matches it."""
    s = str(platform_name or "")
    return (s.split(" - ", 1)[0] if " - " in s else s).strip()


COMPASS = {"North": "South", "South": "North", "East": "West", "West": "East"}


def counterpart(name):
    """The opposite direction, spelled the way TfL spelled this one, or None:
    "Eastbound" -> "Westbound", "EastBound" -> "WestBound",
    "Northbound Fast" -> "Southbound Fast", "Inner Rail" -> "Outer Rail"."""
    m = re.fullmatch(r"(North|South|East|West)(bound|Bound)( Fast)?", name)
    if m:
        return COMPASS[m.group(1)] + m.group(2) + (m.group(3) or "")
    m = re.fullmatch(r"(Inner|Outer) Rail", name)
    if m:
        return ("Outer" if m.group(1) == "Inner" else "Inner") + " Rail"
    return None


def route_ends(line):
    """Stations at either end of any of the line's routes, from TfL's own
    route sequences. Heathrow T4, on the Piccadilly's one-way loop, is one."""
    ends = set()
    for route in get("/Line/%s/Route/Sequence/all" % line).get("orderedLineRoutes", []):
        ids = route.get("naptanIds") or []
        if ids:
            ends.update((ids[0], ids[-1]))
    return ends


def main():
    ap = argparse.ArgumentParser(description="Sweep TfL for Tube direction names.")
    ap.add_argument("--sweeps", type=int, default=8)
    ap.add_argument("--interval", type=int, default=90, help="seconds between sweeps")
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()

    lines = sorted(l["id"] for l in get("/Line/Mode/tube") if l.get("id"))
    stations = {line: {sp["id"] for sp in get("/Line/%s/StopPoints" % line)
                       if sp.get("stopType") == "NaptanMetroStation" and sp.get("id")}
                for line in lines}

    try:
        with open(args.out, encoding="utf-8") as f:
            found = {line: {st: set(names) for st, names in table.items()}
                     for line, table in json.load(f).get("lines", {}).items()}
    except (OSError, ValueError):
        found = {}

    for n in range(args.sweeps):
        if n:
            time.sleep(args.interval)
        seen = 0
        for line in lines:
            try:
                predictions = get("/Line/%s/Arrivals" % line)
            except Exception as e:
                print("  sweep %d: %s failed: %s" % (n + 1, line, e))
                continue
            for p in predictions:
                st, name = p.get("naptanId"), token(p.get("platformName"))
                if not name or st not in stations[line]:
                    continue
                names = found.setdefault(line, {}).setdefault(st, set())
                # One spelling per direction: TfL writes "EastBound" at some
                # stations, and the firmware matches either case.
                if name.lower() not in {x.lower() for x in names}:
                    names.add(name)
                seen += 1
        print("sweep %d/%d: %d predictions" % (n + 1, args.sweeps, seen))

    completed = 0
    for line in lines:
        ends = route_ends(line)
        for st, names in found.get(line, {}).items():
            if st in ends or len(names) != 1:
                continue
            other = counterpart(next(iter(names)))
            if other:
                names.add(other)
                completed += 1
    print("completed %d through station(s) seen in one direction only" % completed)

    subsurface = ("circle", "district", "hammersmith-city")
    borrowed = 0
    for line in subsurface:
        for st in stations.get(line, set()) - set(found.get(line, {})):
            names = set()
            for other in subsurface:
                if other != line:
                    names |= found.get(other, {}).get(st, set())
            if names:
                found.setdefault(line, {})[st] = names
                borrowed += 1
    print("borrowed names for %d station(s) never seen on their own line" % borrowed)

    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": "TfL Unified API /Line/{id}/Arrivals: platformName before ' - '",
        "lines": {line: {st: sorted(found[line][st]) for st in sorted(found[line])}
                  for line in lines if found.get(line)},
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
        f.write("\n")

    total = sum(len(stations[line]) for line in lines)
    covered = sum(len(set(found.get(line, {})) & stations[line]) for line in lines)
    print("wrote %s: %d of %d line-station pairs have directions" % (args.out, covered, total))
    for line in lines:
        never = sorted(stations[line] - set(found.get(line, {})))
        if never:
            print("  %s: nothing seen at %s" % (line, ", ".join(never)))


if __name__ == "__main__":
    main()
