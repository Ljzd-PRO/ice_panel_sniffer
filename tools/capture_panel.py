#!/usr/bin/env python3
"""Capture and summarize ESP32-C3 ice-maker panel sniffer output."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import queue
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import serial
from serial.tools import list_ports


DEFAULT_PORT = "/dev/cu.usbmodem1101"
DEFAULT_BAUD = 921600
NODE_COLUMNS = ["p1", "p2", "p3", "p4", "p5"]
PAIR_NETS = [
    ("LED5_ice_full", "P5-P1", "p5", "p1"),
    ("LED4_no_water", "P5-P2", "p5", "p2"),
    ("SW1_power", "P1-P2", "p1", "p2"),
    ("SW2_select", "P1-P3", "p1", "p3"),
    ("LED3_large", "P3-P4", "p3", "p4"),
    ("LED2_small", "P2-P4", "p2", "p4"),
    ("LED1_power", "P1-P4", "p1", "p4"),
]


@dataclass
class CapturePaths:
    out_dir: Path
    raw_csv: Path
    events_csv: Path
    report_md: Path


def timestamp_slug() -> str:
    return dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def make_capture_paths(base_dir: Path, label: str | None) -> CapturePaths:
    slug = timestamp_slug()
    if label:
        safe_label = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in label)
        slug = f"{slug}-{safe_label}"
    out_dir = base_dir / slug
    out_dir.mkdir(parents=True, exist_ok=True)
    return CapturePaths(
        out_dir=out_dir,
        raw_csv=out_dir / "raw.csv",
        events_csv=out_dir / "events.csv",
        report_md=out_dir / "report.md",
    )


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for port in ports:
        detail = " ".join(part for part in [port.description, port.manufacturer] if part)
        print(f"{port.device}\t{detail}")


def input_worker(commands: queue.Queue[str], stop: threading.Event) -> None:
    print("Interactive commands: mark <label>, mode adc, mode edge, adc_us <n>, status, quit")
    print("Active commands: sw1_weak [ms], sw1_od [ms], sw1_hold_od [ms], sw1_pair_od [ms], sw2_weak [ms], sw2_od [ms], sw2_hold_od [ms], sw2_pair_od [ms], release")
    print("Direct-GPIO labels: power_led_on/off, sw1_down/up, sw2_down/up, small_led_on, large_led_on")
    while not stop.is_set():
        try:
            line = sys.stdin.readline()
        except KeyboardInterrupt:
            commands.put("quit")
            return
        if line == "":
            time.sleep(0.05)
            continue
        commands.put(line.strip())


def open_serial(port: str, baud: int) -> serial.Serial:
    return serial.Serial(port=port, baudrate=baud, timeout=0.1, write_timeout=1.0)


def send_command(ser: serial.Serial, command: str) -> None:
    ser.write((command.strip() + "\n").encode("utf-8"))
    ser.flush()


def parse_firmware_line(line: str, host_ts: float) -> dict[str, object]:
    parts = line.split(",")
    record = {
        "type": "",
        "host_ts": host_ts,
        "device_us": "",
        "p1": "",
        "p2": "",
        "p3": "",
        "p4": "",
        "p5": "",
        "mask": "",
        "mode": "",
        "label": "",
        "raw": line,
    }
    if not line:
        return record
    if line.startswith("#"):
        record["type"] = "#"
        return record
    kind = parts[0]
    record["type"] = kind
    try:
        if kind == "A" and len(parts) >= 8:
            record["device_us"] = int(parts[1])
            for col, value in zip(NODE_COLUMNS, parts[2:7], strict=True):
                record[col] = int(value)
            record["mask"] = int(parts[7])
        elif kind == "E" and len(parts) >= 3:
            record["device_us"] = int(parts[1])
            record["mask"] = int(parts[2])
        elif kind == "H" and len(parts) >= 4:
            record["device_us"] = int(parts[1])
            record["mode"] = parts[2]
            record["mask"] = int(parts[3])
        elif kind == "P" and len(parts) >= 4:
            record["device_us"] = int(parts[1])
            record["mode"] = parts[2]
            record["label"] = parts[3]
    except ValueError:
        record["type"] = "?"
    return record


def record_marker(
    raw_writer: csv.writer,
    event_writer: csv.DictWriter,
    marker_type: str,
    label: str,
) -> None:
    host_ts = time.time()
    raw_writer.writerow([marker_type, f"{host_ts:.6f}", label])
    event_writer.writerow(
        {
            "type": marker_type,
            "host_ts": host_ts,
            "device_us": "",
            "p1": "",
            "p2": "",
            "p3": "",
            "p4": "",
            "p5": "",
            "mask": "",
            "mode": "",
            "label": label,
            "raw": f"{marker_type},{host_ts:.6f},{label}",
        }
    )


def capture(args: argparse.Namespace) -> CapturePaths:
    paths = make_capture_paths(args.out_dir, args.label)
    stop = threading.Event()
    commands: queue.Queue[str] = queue.Queue()
    input_thread = threading.Thread(target=input_worker, args=(commands, stop), daemon=True)
    input_thread.start()

    print(f"Opening {args.port} at {args.baud} baud")
    print(f"Writing capture to {paths.out_dir}")

    with open_serial(args.port, args.baud) as ser:
        time.sleep(args.settle_s)
        send_command(ser, "help")
        send_command(ser, f"adc_us {args.adc_us}")
        send_command(ser, f"mode {args.mode}")

        started = time.monotonic()
        with paths.raw_csv.open("w", newline="", encoding="utf-8") as raw_file, paths.events_csv.open(
            "w", newline="", encoding="utf-8"
        ) as event_file:
            raw_writer = csv.writer(raw_file)
            event_writer = csv.DictWriter(
                event_file,
                fieldnames=[
                    "type",
                    "host_ts",
                    "device_us",
                    "p1",
                    "p2",
                    "p3",
                    "p4",
                    "p5",
                    "mask",
                    "mode",
                    "label",
                    "raw",
                ],
            )
            event_writer.writeheader()
            raw_writer.writerow(["# raw firmware lines plus local M/C markers"])
            record_marker(raw_writer, event_writer, "M", "capture_start")

            while not stop.is_set():
                if args.duration_s and (time.monotonic() - started) >= args.duration_s:
                    record_marker(raw_writer, event_writer, "M", "duration_elapsed")
                    break

                while True:
                    try:
                        command = commands.get_nowait()
                    except queue.Empty:
                        break
                    if not command:
                        continue
                    if command == "quit":
                        record_marker(raw_writer, event_writer, "M", "quit")
                        stop.set()
                        break
                    if command.startswith("mark "):
                        label = command[5:].strip() or "unnamed"
                        record_marker(raw_writer, event_writer, "M", label)
                        print(f"MARK {label}")
                        continue
                    send_command(ser, command)
                    record_marker(raw_writer, event_writer, "C", command)

                raw = ser.readline()
                if not raw:
                    continue
                host_ts = time.time()
                line = raw.decode("utf-8", errors="replace").strip()
                raw_writer.writerow([line])
                event_writer.writerow(parse_firmware_line(line, host_ts))

    analyze_events(paths.events_csv, paths.out_dir, no_plot=args.no_plot)
    return paths


def numeric_frame(events: pd.DataFrame, rows: Iterable[str]) -> pd.DataFrame:
    df = events[events["type"].isin(rows)].copy()
    for col in ["device_us", "mask", *NODE_COLUMNS]:
        if col in df:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    return df


def summarize_masks(events: pd.DataFrame) -> pd.DataFrame:
    df = numeric_frame(events, ["A", "E", "H"])
    df = df.dropna(subset=["mask"])
    if df.empty:
        return pd.DataFrame(columns=["mask", "count", "pct", "bits"])
    summary = df["mask"].astype(int).value_counts().rename_axis("mask").reset_index(name="count")
    summary["pct"] = summary["count"] / summary["count"].sum() * 100
    summary["bits"] = summary["mask"].map(lambda value: format(int(value), "05b"))
    return summary


def summarize_pairs(adc: pd.DataFrame) -> pd.DataFrame:
    rows = []
    if adc.empty:
        return pd.DataFrame(columns=["name", "pair", "min", "max", "mean", "span", "abs_max"])
    for name, pair, a, b in PAIR_NETS:
        diff = adc[a] - adc[b]
        rows.append(
            {
                "name": name,
                "pair": pair,
                "min": float(diff.min()),
                "max": float(diff.max()),
                "mean": float(diff.mean()),
                "span": float(diff.max() - diff.min()),
                "abs_max": float(diff.abs().max()),
            }
        )
    return pd.DataFrame(rows).sort_values("span", ascending=False)


def write_table(lines: list[str], title: str, df: pd.DataFrame, max_rows: int = 12) -> None:
    lines.append(f"## {title}")
    if df.empty:
        lines.append("No data.")
        lines.append("")
        return
    lines.append(df.head(max_rows).to_markdown(index=False))
    lines.append("")


def plot_outputs(adc: pd.DataFrame, masks: pd.DataFrame, out_dir: Path) -> list[Path]:
    written: list[Path] = []
    if not adc.empty:
        x = (adc["device_us"] - adc["device_us"].iloc[0]) / 1_000_000.0

        plt.figure(figsize=(12, 6))
        for col in NODE_COLUMNS:
            plt.plot(x, adc[col], linewidth=0.8, label=col.upper())
        plt.xlabel("device time (s)")
        plt.ylabel("ADC raw count")
        plt.title("Panel node ADC samples")
        plt.legend(loc="best")
        plt.tight_layout()
        path = out_dir / "adc_nodes.png"
        plt.savefig(path, dpi=150)
        plt.close()
        written.append(path)

        plt.figure(figsize=(12, 6))
        for name, pair, a, b in PAIR_NETS:
            plt.plot(x, adc[a] - adc[b], linewidth=0.8, label=f"{pair} {name}")
        plt.xlabel("device time (s)")
        plt.ylabel("ADC raw count difference")
        plt.title("Netlist pair differences")
        plt.legend(loc="best", fontsize="small")
        plt.tight_layout()
        path = out_dir / "pair_diffs.png"
        plt.savefig(path, dpi=150)
        plt.close()
        written.append(path)

    if not masks.empty:
        plt.figure(figsize=(10, 5))
        plt.bar(masks["bits"], masks["count"])
        plt.xlabel("mask bits P5..P1")
        plt.ylabel("count")
        plt.title("Digital mask frequency")
        plt.xticks(rotation=45, ha="right")
        plt.tight_layout()
        path = out_dir / "mask_frequency.png"
        plt.savefig(path, dpi=150)
        plt.close()
        written.append(path)
    return written


def analyze_events(events_csv: Path, out_dir: Path | None = None, no_plot: bool = False) -> Path:
    out_dir = out_dir or events_csv.parent
    events = pd.read_csv(events_csv, keep_default_na=False)
    adc = numeric_frame(events, ["A"]).dropna(subset=NODE_COLUMNS, how="any")
    masks = summarize_masks(events)
    pairs = summarize_pairs(adc)
    marks = events[events["type"].isin(["M", "C"])][["type", "host_ts", "label"]].copy()

    lines: list[str] = ["# Ice Panel Capture Report", ""]
    lines.append(f"- Source: `{events_csv}`")
    lines.append(f"- Rows: {len(events)}")
    lines.append(f"- ADC rows: {len(adc)}")
    lines.append("- Direct-floating note: ADC raw counts and pair differences are relative correlation data, not calibrated panel voltages.")
    if not adc.empty:
        duration_s = (adc["device_us"].iloc[-1] - adc["device_us"].iloc[0]) / 1_000_000.0
        lines.append(f"- ADC device-time span: {duration_s:.3f} s")
    lines.append("")

    if not adc.empty:
        node_stats = adc[NODE_COLUMNS].agg(["min", "max", "mean", "std"]).T.reset_index()
        node_stats = node_stats.rename(columns={"index": "node"})
        write_table(lines, "Node ADC Stats", node_stats)
    write_table(lines, "Netlist Pair Difference Stats", pairs)
    write_table(lines, "Digital Mask Frequency", masks)
    write_table(lines, "Markers And Commands", marks, max_rows=40)

    edge = numeric_frame(events, ["E"]).dropna(subset=["device_us"])
    if len(edge) >= 2:
        intervals_us = edge["device_us"].diff().dropna()
        edge_summary = pd.DataFrame(
            [
                {
                    "edge_count": len(edge),
                    "min_interval_us": float(intervals_us.min()),
                    "median_interval_us": float(intervals_us.median()),
                    "max_interval_us": float(intervals_us.max()),
                }
            ]
        )
        write_table(lines, "Edge Timing", edge_summary)

    if not no_plot:
        written = plot_outputs(adc, masks, out_dir)
        if written:
            lines.append("## Plots")
            for path in written:
                lines.append(f"- `{path.name}`")
            lines.append("")

    report = out_dir / "report.md"
    report.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote report: {report}")
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port, default {DEFAULT_PORT}")
    parser.add_argument("--baud", default=DEFAULT_BAUD, type=int, help=f"Serial baud, default {DEFAULT_BAUD}")
    parser.add_argument("--mode", choices=["adc", "edge"], default="adc", help="Initial firmware mode")
    parser.add_argument("--adc-us", default=1000, type=int, help="ADC sampling interval in microseconds")
    parser.add_argument("--duration-s", default=0.0, type=float, help="Capture duration; 0 means until quit")
    parser.add_argument("--settle-s", default=1.0, type=float, help="Wait after opening serial before commands")
    parser.add_argument("--out-dir", default=Path("ice_panel_sniffer/captures"), type=Path)
    parser.add_argument("--label", default=None, help="Optional capture folder suffix")
    parser.add_argument("--no-plot", action="store_true", help="Skip matplotlib plot generation")
    parser.add_argument("--list-ports", action="store_true", help="List serial ports and exit")
    parser.add_argument("--analyze", type=Path, help="Analyze an existing events.csv and exit")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.list_ports:
        list_serial_ports()
        return 0
    if args.analyze:
        analyze_events(args.analyze, args.analyze.parent, no_plot=args.no_plot)
        return 0
    try:
        paths = capture(args)
    except KeyboardInterrupt:
        print("\nCapture interrupted.")
        return 130
    except serial.SerialException as exc:
        print(f"Serial error: {exc}", file=sys.stderr)
        return 2
    print(f"Raw CSV: {paths.raw_csv}")
    print(f"Events CSV: {paths.events_csv}")
    print(f"Report: {paths.report_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
