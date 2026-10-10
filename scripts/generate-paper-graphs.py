#!/usr/bin/env python3
"""Generate reproducible paper figures from Ibuki evaluation CSV files."""

import argparse
import csv
import html
import math
import pathlib
import statistics
from collections import Counter, defaultdict


WIDTH = 960
HEIGHT = 540
MARGIN = 70


def read_csv(filename):
    with pathlib.Path(filename).open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def write_svg(filename, title, labels, values, colors, ylabel, value_format="{:.0f}", deviations=None):
    path = pathlib.Path(filename)
    deviations = deviations or [0.0] * len(values)
    maximum = max((value + deviation for value, deviation in zip(values, deviations)), default=1.0) * 1.15
    maximum = maximum if maximum > 0 else 1.0
    chart_width = WIDTH - MARGIN * 2
    chart_height = HEIGHT - MARGIN * 2 - 35
    bar_width = chart_width / max(len(labels), 1) * 0.65
    baseline = MARGIN + chart_height
    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        '<style>text{font-family: sans-serif; fill: #1f2937} .axis{stroke:#6b7280;stroke-width:1} .grid{stroke:#e5e7eb;stroke-width:1} .value{font-size:13px} .label{font-size:12px}</style>',
        f'<text x="{WIDTH / 2:.1f}" y="32" text-anchor="middle" font-size="21" font-weight="bold">{html.escape(title)}</text>',
        f'<text x="18" y="{MARGIN + chart_height / 2:.1f}" text-anchor="middle" transform="rotate(-90 18 {MARGIN + chart_height / 2:.1f})" font-size="14">{html.escape(ylabel)}</text>',
        f'<line class="axis" x1="{MARGIN}" y1="{baseline}" x2="{WIDTH - MARGIN}" y2="{baseline}"/>',
        f'<line class="axis" x1="{MARGIN}" y1="{MARGIN}" x2="{MARGIN}" y2="{baseline}"/>',
    ]
    for tick in range(5):
        ratio = tick / 4
        y = baseline - chart_height * ratio
        value = maximum * ratio
        elements.append(f'<line class="grid" x1="{MARGIN}" y1="{y:.1f}" x2="{WIDTH - MARGIN}" y2="{y:.1f}"/>')
        elements.append(f'<text x="{MARGIN - 10}" y="{y + 4:.1f}" text-anchor="end" class="label">{value_format.format(value)}</text>')
    for index, (label, value) in enumerate(zip(labels, values)):
        center = MARGIN + chart_width * (index + 0.5) / max(len(labels), 1)
        height = chart_height * value / maximum
        x = center - bar_width / 2
        y = baseline - height
        color = colors[index % len(colors)]
        elements.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_width:.1f}" height="{height:.1f}" fill="{color}" rx="3"/>')
        upper = baseline - chart_height * (value + deviations[index]) / maximum
        lower = baseline - chart_height * max(0, value - deviations[index]) / maximum
        elements.append(f'<path d="M {center:.1f} {upper:.1f} V {lower:.1f} M {center-6:.1f} {upper:.1f} H {center+6:.1f} M {center-6:.1f} {lower:.1f} H {center+6:.1f}" stroke="#111827" fill="none"/>')
        elements.append(f'<text x="{center:.1f}" y="{max(y - 8, MARGIN):.1f}" text-anchor="middle" class="value">{value_format.format(value)}</text>')
        elements.append(f'<text x="{center:.1f}" y="{baseline + 22}" text-anchor="middle" class="label" transform="rotate(-25 {center:.1f} {baseline + 22})">{html.escape(label)}</text>')
    elements.append("</svg>")
    path.write_text("\n".join(elements) + "\n", encoding="utf-8")


def generate_evaluation_graphs(rows, output_dir):
    statuses = Counter(row.get("status", "unknown") for row in rows)
    status_order = [status for status in ("pass", "partial", "skip", "fail") if status in statuses]
    status_colors = {"pass": "#16a34a", "partial": "#d97706", "skip": "#64748b", "fail": "#dc2626"}
    write_svg(
        output_dir / "evaluation-status.svg",
        "Ibuki evaluation result",
        status_order,
        [statuses[status] for status in status_order],
        [status_colors[status] for status in status_order],
        "Cases",
    )

    duration_rows = [row for row in rows if row.get("duration_ms", "").isdigit()]
    if duration_rows:
        write_svg(
            output_dir / "evaluation-duration.svg",
            "Evaluation duration by case",
            [row["case"] for row in duration_rows],
            [float(row["duration_ms"]) for row in duration_rows],
            ["#2563eb"],
            "Milliseconds",
            "{:.0f}",
        )


def generate_metric_graphs(rows, output_dir):
    if not rows or "scenario" not in rows[0]:
        return False
    grouped = defaultdict(lambda: defaultdict(list))
    for row in rows:
        if row.get("status", "pass") != "pass":
            continue
        label = row["scenario"] + "/" + row.get("strategy", "default")
        for name, raw in row.items():
            if name in ("repetition", "scenario", "strategy", "status"):
                continue
            try:
                value = float(raw)
            except (ValueError, TypeError):
                continue
            if math.isfinite(value):
                grouped[name][label].append(value)
    if not grouped:
        return False
    summary_file = output_dir / "metrics-summary.csv"
    with summary_file.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["metric", "scenario_strategy", "samples", "mean", "median", "stdev", "minimum", "maximum", "p95"])
        for metric, cases in sorted(grouped.items()):
            labels = list(cases)
            means = [statistics.fmean(cases[label]) for label in labels]
            deviations = [statistics.stdev(cases[label]) if len(cases[label]) > 1 else 0 for label in labels]
            for label, mean, deviation in zip(labels, means, deviations):
                values = sorted(cases[label])
                writer.writerow([metric, label, len(values), mean, statistics.median(values), deviation,
                                 values[0], values[-1], values[math.ceil(0.95 * len(values)) - 1]])
            unit = "Milliseconds" if metric.endswith("_ms") else ("Percent" if "percent" in metric or metric.endswith("packet_loss") else ("Mbit/s" if metric.endswith("_mbps") else ("MiB" if "memory_mb" in metric else "Count")))
            filename = {"transition_ms": "transition-time", "packet_loss": "packet-loss"}.get(metric, metric)
            write_svg(output_dir / (filename + ".svg"), metric + " (mean +/- sample SD)", labels, means,
                      ["#2563eb", "#7c3aed"], unit, "{:.2f}", deviations)
    return True


def publication_figures(output_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    selected = {"detection_ms", "transition_ms", "max_reply_gap_ms", "packet_loss",
                "throughput_mbps", "cpu_percent", "memory_mb", "tcp_retransmissions"}
    grouped = defaultdict(list)
    for row in read_csv(output_dir / "metrics-summary.csv"):
        if row["metric"] in selected:
            grouped[row["metric"]].append(row)
    for metric, cases in grouped.items():
        labels = [row["scenario_strategy"].replace("live-fallback-recovery-rollback/", "switch/") for row in cases]
        means = [float(row["mean"]) for row in cases]
        deviations = [float(row["stdev"]) for row in cases]
        unit = "ms" if metric.endswith("_ms") else ("%" if metric in ("packet_loss", "cpu_percent") else ("Mbit/s" if metric.endswith("_mbps") else ("MiB" if metric == "memory_mb" else "count")))
        figure, axes = plt.subplots(figsize=(7, 4.5))
        axes.bar(labels, means, yerr=deviations, capsize=5, color="#2563eb", edgecolor="#1e3a8a")
        axes.set_ylabel(unit)
        axes.set_title(metric.replace("_", " ") + " (mean +/- sample SD)")
        axes.set_ylim(bottom=0)
        axes.tick_params(axis="x", labelrotation=25)
        axes.grid(axis="y", alpha=0.25)
        axes.set_axisbelow(True)
        for position, row in enumerate(cases):
            axes.annotate("n=" + row["samples"], (position, means[position]), xytext=(0, 4),
                          textcoords="offset points", ha="center", fontsize=9)
        figure.tight_layout()
        for extension in ("png", "pdf"):
            figure.savefig(output_dir / (metric + "." + extension), dpi=200)
        plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description="Generate paper figures from Ibuki CSV results")
    parser.add_argument("--summary-csv", help="vm-evaluate.sh summary.csv")
    parser.add_argument("--metrics-csv", help="measured scenario CSV with scenario,transition_ms,packet_loss")
    parser.add_argument("--out-dir", required=True, help="figure output directory")
    parser.add_argument("--publication", action="store_true", help="also generate selected PNG/PDF figures; requires matplotlib")
    args = parser.parse_args()
    output_dir = pathlib.Path(args.out_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if not args.summary_csv and not args.metrics_csv:
        parser.error("provide --summary-csv or --metrics-csv")
    if args.summary_csv:
        generate_evaluation_graphs(read_csv(args.summary_csv), output_dir)
    metric_graphs = generate_metric_graphs(read_csv(args.metrics_csv), output_dir) if args.metrics_csv else False
    if args.summary_csv:
        print(f"generated: {output_dir / 'evaluation-status.svg'}")
    if (output_dir / "evaluation-duration.svg").exists():
        print(f"generated: {output_dir / 'evaluation-duration.svg'}")
    if metric_graphs:
        if args.publication:
            publication_figures(output_dir)
        print(f"generated: {output_dir / 'metrics-summary.csv'}")
        for filename in sorted(output_dir.glob("*.svg")):
            print(f"generated: {filename}")
    else:
        print("measured metrics: not supplied; only evaluation summary figures generated")


if __name__ == "__main__":
    main()
