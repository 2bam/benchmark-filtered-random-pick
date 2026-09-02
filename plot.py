#!/usr/bin/env python3
"""Plot every slice of the benchmark CSVs that makes sense. No arguments.

Data wrangling is Python's stdlib `csv` (no third-party packages); rendering is
gnuplot, which is already a dependency.
"""

import csv
import pathlib
import re
import shutil
import subprocess
import tempfile

OUT = pathlib.Path("plots")  # rebound in main() to a per-machine subdir
GENERATORS = ("shishua", "lcg", "lcg4", "xorshift")
ALGOS = ("reservoir every", "reservoir skip", "filter cache")


def load(name):
    path = pathlib.Path(f"{name}.csv")
    with path.open() as f:
        # The C side pads fields so the rows read as a table; strip that back off.
        return [{k.strip(): v.strip() for k, v in row.items()}
                for row in csv.DictReader(f)]


def outdir(rows):
    """plots/<arch>-<cpu>-<compiler>, so results from different machines do
    not collide.

    arch/cpu/compiler all come from the run's `info` field, stamped by the C
    binary. SIMD is deliberately left out: it varies per generator, so it is a
    property of a row rather than of the run.
    """
    return pathlib.Path("plots") / re.sub(r"[^A-Za-z0-9._-]+", "-", rows[0]["info"])


def machine(rows):
    """The run's arch / cpu / compiler for the plotted rows, plus the SIMD
    path when they share one.

    A per-generator plot names its own path (neon, scalar); one spanning
    several generators omits it rather than claiming a single value.
    """
    if not rows:
        return ""
    tag = rows[0]["info"]
    paths = {r["simd"] for r in rows}
    return tag + (f" / {paths.pop()}" if len(paths) == 1 else "")


def gnuplot(script):
    subprocess.run(["gnuplot"], input=script, text=True, check=True)


def where(rows, **filters):
    """Rows matching every column=value pair."""
    return [r for r in rows
            if all(str(r[col]) == str(val) for col, val in filters.items())]


def plot(rows, png, title, x, series, y, logx=False, logy=False,
         spread=False, **filters):
    """Line chart: x against y, one line per distinct value of `series`."""
    matching = where(rows, **filters)
    lines = {}
    for r in matching:
        if r[y]:
            lines.setdefault(r[series], []).append((float(r[x]), float(r[y])))
    if not lines:
        return  # nothing matched; skip rather than emit an empty chart

    with tempfile.TemporaryDirectory() as tmp:
        curves = []
        for name in sorted(lines):
            data = pathlib.Path(tmp) / name
            data.write_text("".join(f"{a} {b}\n" for a, b in sorted(lines[name])))
            curves.append(
                f"'{data}' using 1:2 with linespoints pt 7 ps 0.8 title '{name}'")

        if spread:
            # One bar per x from the lowest series to the highest. On a log y
            # axis that vertical distance is a ratio, so label it as one.
            xs = sorted({xv for pts in lines.values() for xv, _ in pts})
            span = pathlib.Path(tmp) / "_spread"
            with span.open("w") as f:
                for xv in xs:
                    ys = [yv for pts in lines.values() for a, yv in pts if a == xv]
                    if len(ys) > 1:
                        f.write(f"{xv} {min(ys)} {max(ys)}\n")
            if span.stat().st_size:
                curves.append(f"'{span}' using 1:2:(0):($3-$2) with vectors "
                              "nohead lw 2 lc rgb 'gray50' notitle")
                curves.append(f'\'{span}\' using 1:3:(sprintf("%.1fx", $3/$2)) '
                              "with labels offset 0,1.2 tc rgb 'gray30' notitle")
        gnuplot(f"""
            set terminal pngcairo noenhanced size 900,560 font ',11'
            set output '{OUT / png}'
            set title "{title}\\n{machine(matching)}\\nLower Y is better."
            set xlabel '{x}'
            set ylabel '{y}'
            set grid
            set key outside right
            {'set logscale x' if logx else ''}
            {'set logscale y' if logy else ''}
            {'set offsets graph 0.04, graph 0.07, graph 0.10, graph 0.02' if spread else ''}
            plot {', '.join(curves)}
        """)
    # print(f"  {OUT / png}")


def main():
    global OUT
    select, speed = load("select"), load("speed")
    OUT = outdir(select)
    # Clear this machine's plots only, so a merged plots/ keeps the others.
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    print(f"writing plots to {OUT}/ ...")

    rates = list(dict.fromkeys(r["rate"] for r in select))

    # metric="ns_best"
    metric="ns_mean"

    for rng in GENERATORS:
        for rt in rates:
            for mode, cache in (("no", "warm"), ("yes", "cold")):
                plot(select, f"size-{cache}-{rng}-{rt.replace('%', 'pct')}.png",
                     f"{rng}: cost vs array size ({rt} match, {cache} caches)",
                     x="n", series="algo", y=metric, logx=True, logy=True,
                     spread=True, rng=rng, rate=rt, thrash=mode)
        plot(select, f"rate-{rng}.png",
             f"{rng}: cost vs match rate (n=10000)",
             x="match_pct", series="algo", y=metric,
             rng=rng, n=10000, thrash="no")

    for algo in ALGOS:
        plot(select, f"rng-{algo}.png",
             f"{algo}: generators compared (47% match)",
             x="n", series="rng", y=metric, logx=True, logy=True,
             spread=True, algo=algo, rate="47%", thrash="no")

    plot(speed, "speed.png", "nanoseconds per random uint64",
         x="batch", series="rng", y=metric, logx=True)

    print('Done.')


if __name__ == "__main__":
    main()
