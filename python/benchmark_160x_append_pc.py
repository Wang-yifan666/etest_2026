#!/usr/bin/env python3
"""
追加 160×160 / 160×224 / 160×640 模型数据到 test_rate_pc.md 的每张表格内部（#18~#23）
"""
import subprocess
import re
import statistics
from pathlib import Path
from datetime import datetime

BENCHMARK = "./build/etest_yolo_benchmark"
CLASSES = "model/onnx_640_640/classes.txt"
WARMUP = 30
ITERATIONS = 200
THREADS = 4
OUTPUT_MD = "model/test_rate_pc.md"

MODELS = [
    ("NCNN 160×160 FP16", "model/ncnn_160x160/best_160x160_fp16.ncnn.param", 160, 160, True, True),
    ("NCNN 160×160 FP32", "model/ncnn_160x160/best_160x160_fp32.ncnn.param", 160, 160, False, False),
    ("NCNN 160×224 FP16", "model/ncnn_160x224/best_160x224_fp16.ncnn.param", 160, 224, True, True),
    ("NCNN 160×224 FP32", "model/ncnn_160x224/best_160x224_fp32.ncnn.param", 160, 224, False, False),
    ("NCNN 160×640 FP16", "model/ncnn_160x640/best_160x640_fp16.ncnn.param", 160, 640, True, True),
    ("NCNN 160×640 FP32", "model/ncnn_160x640/best_160x640_fp32.ncnn.param", 160, 640, False, False),
]


def run_benchmark(args: list[str]) -> dict | None:
    cmd = [BENCHMARK] + args
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT", flush=True)
        return None

    stdout = result.stdout
    stderr = result.stderr
    if result.returncode != 0:
        msg = stderr.strip().split("\n")[-1] if stderr.strip() else ""
        print(f"  FAILED: {msg[:120]}", flush=True)
        return None

    stats = {}
    for line in stdout.splitlines():
        m = re.match(r"(\S+)\s+mean=\s*([0-9.]+).*fps=\s*([0-9.]+)", line.strip())
        if m:
            phase = m.group(1).lower()
            stats[phase] = {"mean_ms": float(m.group(2)), "fps": float(m.group(3))}

    det_m = re.search(r"Average detections/frame:\s*([0-9.]+)", stdout)
    stats["detections"] = float(det_m.group(1)) if det_m else 0.0

    sig_section = stdout.find("─── 首帧检测签名 ───")
    if sig_section >= 0:
        sigs = stdout[sig_section:].splitlines()
        det_list = []
        for line in sigs:
            m2 = re.match(r"class=(\d+)\s+confidence=([0-9.]+)", line)
            if m2:
                det_list.append(f"c{m2.group(1)}@{m2.group(2)}")
        stats["signature"] = "; ".join(det_list) if det_list else "none"
    else:
        stats["signature"] = "none"

    return stats


def discover_images():
    return sorted(Path("data/images").glob("test*.jpg"))


def fmt_row(idx, backend, size, prec, pre, fwd, dec, nms, tot, fps, det, sig):
    return (f"| {idx} | {backend} | {size} | {prec} | "
            f"{pre:.2f} | {fwd:.2f} | {dec:.2f} | {nms:.2f} | "
            f"{tot:.2f} | {fps:.1f} | {det:.1f} | {sig} |")


def main():
    images = discover_images()
    img_names = [p.name for p in images]

    print("=" * 60)
    print(f"160× 追加到 PC: {len(images)} images × {len(MODELS)} models")
    print(f"输出: {OUTPUT_MD}")
    print("=" * 60, flush=True)

    # all_data[img_idx][model_idx]
    all_data: list[list[dict | None]] = []

    for img_idx, img_path in enumerate(images):
        img_name = img_path.name
        print(f"\n[{img_idx + 1}/{len(images)}] {img_name}", flush=True)
        row = []
        for label, param_path, w, h, use_fp16_storage, use_fp16_arithmetic in MODELS:
            print(f"  {label}...", flush=True)
            extra = []
            if use_fp16_storage:
                extra = ["--fp16-storage", "--fp16-arithmetic"]
            s = run_benchmark([
                "--backend", "ncnn",
                "--param", param_path,
                "--classes", CLASSES,
                "--image", str(img_path),
                "--width", str(w), "--height", str(h),
                "--threads", str(THREADS),
                "--warmup", str(WARMUP),
                "--iterations", str(ITERATIONS),
            ] + extra)
            if s:
                s["_label"] = label
            row.append(s)
        all_data.append(row)

    # ── 读取原文件 ──
    with open(OUTPUT_MD, "r") as f:
        lines = f.readlines()

    # 删掉可能已有的追加行（以 | 18 | 开头的行）
    lines = [l for l in lines if not re.match(r'\|\s*1[89]\s*\|', l) and not re.match(r'\|\s*2[0-3]\s*\|', l)]

    # ── 为每张图片构建新行 ──
    # 为汇总表构建行
    summary_rows = []
    for i in range(len(MODELS)):
        parts = MODELS[i][0].split()
        backend = parts[0]
        size = parts[1]
        prec = parts[2]
        totals = [row[i].get("total", {}).get("mean_ms", 0) for row in all_data if row[i]]
        fpss = [row[i].get("total", {}).get("fps", 0) for row in all_data if row[i]]
        dets = [row[i].get("detections", 0) for row in all_data if row[i]]
        avg_tot = statistics.mean(totals) if totals else 0
        avg_fps = statistics.mean(fpss) if fpss else 0
        avg_det = statistics.mean(dets) if dets else 0
        summary_rows.append(f"| {18 + i} | {backend} | {size} | {prec} | {avg_tot:.2f} | {avg_fps:.1f} | {avg_det:.1f} |\n")

    # 为每张图片构建行
    per_image_rows = {}  # img_idx -> list of lines
    for img_idx in range(len(images)):
        per_image_rows[img_idx] = []
        row_data = all_data[img_idx]
        for i in range(len(MODELS)):
            entry = row_data[i] if i < len(row_data) else None
            if entry is None:
                continue
            parts = MODELS[i][0].split()
            backend = parts[0]
            size = parts[1]
            prec = parts[2]
            pre = entry.get("preprocess", {}).get("mean_ms", 0)
            fwd = entry.get("forward", {}).get("mean_ms", 0)
            dec = entry.get("decode", {}).get("mean_ms", 0)
            nms = entry.get("nms", {}).get("mean_ms", 0)
            tot = entry.get("total", {}).get("mean_ms", 0)
            fps = entry.get("total", {}).get("fps", 0)
            det = entry.get("detections", 0)
            sig = entry.get("signature", "-")
            per_image_rows[img_idx].append(
                fmt_row(18 + i, backend, size, prec, pre, fwd, dec, nms, tot, fps, det, sig) + "\n"
            )

    # ── 插入到原文件中 ──
    output_lines = []
    # 找到汇总表中 "| 17 |" 行的位置，在其后插入 summary_rows
    in_summary = False
    summary_inserted = False
    # per-image counter: 我们需要知道当前在第几张图片
    img_table_count = 0
    img_inserted = [False] * len(images)

    for line in lines:
        # 检测汇总表开始（"## 一、"）
        if "## 一、" in line:
            in_summary = True
            output_lines.append(line)
            continue

        # 汇总表结束（"---" 或 "## 二."）
        if in_summary and line.startswith("---"):
            in_summary = False
            # 如果还没插入，先插入
            if not summary_inserted:
                output_lines.extend(summary_rows)
                output_lines.append("\n")
                summary_inserted = True
            output_lines.append(line)
            continue

        # 汇总表中 | 17 | 行：插入新行在其后
        if in_summary and re.match(r'\|\s*17\s*\|', line):
            output_lines.append(line)
            if not summary_inserted:
                output_lines.extend(summary_rows)
                summary_inserted = True
            continue

        # 每张图片的表格标题 "## 二.N"
        m = re.match(r'## 二\.(\d+)', line)
        if m:
            img_table_count = int(m.group(1)) - 1  # 0-based
            output_lines.append(line)
            continue

        # 每张图片表格中 | 17 | 行：插入
        if img_table_count < len(images) and not img_inserted[img_table_count]:
            if re.match(r'\|\s*17\s*\|', line):
                output_lines.append(line)
                output_lines.extend(per_image_rows[img_table_count])
                img_inserted[img_table_count] = True
                continue

        output_lines.append(line)

    with open(OUTPUT_MD, "w") as f:
        f.writelines(output_lines)

    print(f"\n✅ 已追加 #18~#23 行到 {OUTPUT_MD} 的汇总表和 {len(images)} 张图片表格中", flush=True)


if __name__ == "__main__":
    main()