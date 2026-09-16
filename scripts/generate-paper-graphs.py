#!/usr/bin/env python3
"""Generate reproducible paper figures from Ibuki evaluation CSV files."""

import argparse
import csv
import html
import pathlib
import statistics
from collections import Counter, defaultdict


WIDTH = 960
HEIGHT = 540
MARGIN = 70


def read_csv(filename):
    with pathlib.Path(filename).open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def write_svg(filename, title, labels, values, colors, ylabel, value_format="{:.0f}"):
    path = pathlib.Path(filename)
    maximum = max(values, default=1.0)
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
    required = {"scenario", "transition_ms", "packet_loss"}
    if not rows or not required.issubset(rows[0]):
        return False
    grouped = defaultdict(list)
    for row in rows:
        try:
            transition_ms = float(row["transition_ms"])
            packet_loss = float(row["packet_loss"])
        except (KeyError, ValueError):
            continue
        grouped[row["scenario"]].append((transition_ms, packet_loss))
    if not grouped:
        return False
    labels = list(grouped)
    transition_means = [statistics.fmean(item[0] for item in grouped[label]) for label in labels]
    loss_means = [statistics.fmean(item[1] for item in grouped[label]) for label in labels]
    write_svg(output_dir / "transition-time.svg", "Transition time by scenario", labels, transition_means, ["#7c3aed"], "Milliseconds")
    write_svg(output_dir / "packet-loss.svg", "Packet loss by scenario", labels, loss_means, ["#0891b2"], "Packets", "{:.2f}")
    summary_file = output_dir / "metrics-summary.csv"
    with summary_file.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["scenario", "samples", "mean_transition_ms", "mean_packet_loss", "stdev_transition_ms"])
        for label in labels:
            transitions = [item[0] for item in grouped[label]]
            losses = [item[1] for item in grouped[label]]
            deviation = statistics.stdev(transitions) if len(transitions) > 1 else 0.0
            writer.writerow([label, len(transitions), f"{statistics.fmean(transitions):.3f}", f"{statistics.fmean(losses):.3f}", f"{deviation:.3f}"])
    return True


def main():
    parser = argparse.ArgumentParser(description="Generate paper figures from Ibuki CSV results")
    parser.add_argument("--summary-csv", required=True, help="vm-evaluate.sh summary.csv")
    parser.add_argument("--metrics-csv", help="measured scenario CSV with scenario,transition_ms,packet_loss")
    parser.add_argument("--out-dir", required=True, help="figure output directory")
    args = parser.parse_args()
    output_dir = pathlib.Path(args.out_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    generate_evaluation_graphs(read_csv(args.summary_csv), output_dir)
    metric_graphs = generate_metric_graphs(read_csv(args.metrics_csv), output_dir) if args.metrics_csv else False
    print(f"generated: {output_dir / 'evaluation-status.svg'}")
    if (output_dir / "evaluation-duration.svg").exists():
        print(f"generated: {output_dir / 'evaluation-duration.svg'}")
    if metric_graphs:
        print(f"generated: {output_dir / 'transition-time.svg'}")
        print(f"generated: {output_dir / 'packet-loss.svg'}")
    else:
        print("measured metrics: not supplied; only evaluation summary figures generated")


if __name__ == "__main__":
    main()
