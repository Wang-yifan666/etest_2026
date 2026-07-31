#!/usr/bin/env python3
"""
将 160×160 / 160×224 / 160×640（FP16 + FP32）插入到 test_rate_pc.md
的汇总表和每张图片表格中（接在 | 17 | 之后，作为 #18~#23）。
幂等：若 #18~#23 已存在则先删除再插入。
"""
import subprocess
import re
import sys
import argparse
from pathlib import Path

BENCHMARK = "./build/etest_yolo_benchmark"
CLASSES = "model/onnx_640_640/classes.txt"
WARMUP = 30
ITERATIONS = 200
THREADS = 4

MODELS = [
    ("NCNN 160×160 FP16", "model/ncnn_160x160/best_160x160_fp16.ncnn.param", 160, 160, True, True),
    ("NCNN 160×160 FP32", "model/ncnn_160x160/best_160x160_fp32.ncnn.param", 160, 160, False, False),
    ("NCNN 160×224 FP16", "model/ncnn_160x224/best_160x224_fp16.ncnn.param", 160, 224, True, True),
    ("NCNN 160×224 FP32", "model/ncnn_160x224/best_160x224_fp32.ncnn.param", 160, 224, False, False),
    ("NCNN 160×640 FP16", "model/ncnn_160x640/best_160x640_fp16.ncnn.param", 160, 640, True, True),
    ("NCNN 160×640 FP32", "model/ncnn_160x640/best_160x640_fp32.ncnn.param", 160, 640, False, False),
]


def run(args: list[str]) -> dict | None:
    r = subprocess.run([BENCHMARK] + args, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        err = r.stderr.strip().split("\n")[-1] if r.stderr.strip() else ""
        print(f"  FAIL: {err[:120]}", flush=True); return None
    s = {}
    for line in r.stdout.splitlines():
        m = re.match(r"(\S+)\s+mean=\s*([0-9.]+).*fps=\s*([0-9.]+)", line.strip())
        if m: s[m.group(1).lower()] = {"mean_ms": float(m.group(2)), "fps": float(m.group(3))}
    dm = re.search(r"Average detections/frame:\s*([0-9.]+)", r.stdout)
    s["detections"] = float(dm.group(1)) if dm else 0.0
    sig = r.stdout.find("─── 首帧检测签名 ───")
    if sig >= 0:
        dets = []
        for line in r.stdout[sig:].splitlines():
            m2 = re.match(r"class=(\d+)\s+confidence=([0-9.]+)", line)
            if m2: dets.append(f"c{m2.group(1)}@{m2.group(2)}")
        s["signature"] = "; ".join(dets) if dets else "none"
    else:
        s["signature"] = "none"
    return s


def fmt_row(idx, label, entry):
    parts = label.split()
    backend = parts[0]; size = parts[1]; prec = parts[2]
    pre = entry.get("preprocess", {}).get("mean_ms", 0)
    fwd = entry.get("forward", {}).get("mean_ms", 0)
    dec = entry.get("decode", {}).get("mean_ms", 0)
    nms = entry.get("nms", {}).get("mean_ms", 0)
    tot = entry.get("total", {}).get("mean_ms", 0)
    fps = entry.get("total", {}).get("fps", 0)
    det = entry.get("detections", 0)
    sig = entry.get("signature", "-")
    return (f"| {idx} | {backend} | {size} | {prec} | "
            f"{pre:.2f} | {fwd:.2f} | {dec:.2f} | {nms:.2f} | "
            f"{tot:.2f} | {fps:.1f} | {det:.1f} | {sig} |")


def main():
    parser = argparse.ArgumentParser(
        description="将 160× 模型数据插入到 test_rate_pc.md 或 test_rate_pi.md")
    parser.add_argument("--output", "-o", choices=["pc", "pi"], default="pc",
                        help="输出目标: pc -> test_rate_pc.md, pi -> test_rate_pi.md (默认: pc)")
    args = parser.parse_args()
    OUTPUT_MD = f"model/test_rate_{args.output}.md"

    images = sorted(Path("data/images").glob("test*.jpg"))
    img_names = [p.name for p in images]

    # ── 运行 benchmark ──
    all_data = []  # [img_idx][model_idx]
    for img_idx, img_path in enumerate(images):
        print(f"[{img_idx+1}/{len(images)}] {img_path.name}", flush=True)
        row = []
        for label, param, w, h, fp16_s, fp16_a in MODELS:
            print(f"  {label}...", flush=True)
            extra = ["--fp16-storage", "--fp16-arithmetic"] if fp16_s else []
            s = run(["--backend", "ncnn", "--param", param, "--classes", CLASSES,
                     "--image", str(img_path), "--width", str(w), "--height", str(h),
                     "--threads", str(THREADS), "--warmup", str(WARMUP), "--iterations", str(ITERATIONS)] + extra)
            if s: s["_label"] = label
            row.append(s)
        all_data.append(row)

    # ── 汇总表行 ──
    summary_rows = []
    for i in range(len(MODELS)):
        label = MODELS[i][0]
        parts = label.split()
        totals = [row[i].get("total", {}).get("mean_ms", 0) for row in all_data if row[i]]
        fpss   = [row[i].get("total", {}).get("fps", 0) for row in all_data if row[i]]
        dets   = [row[i].get("detections", 0) for row in all_data if row[i]]
        avg_t = sum(totals)/len(totals) if totals else 0
        avg_f = sum(fpss)/len(fpss) if fpss else 0
        avg_d = sum(dets)/len(dets) if dets else 0
        summary_rows.append(f"| {18+i} | {parts[0]} | {parts[1]} | {parts[2]} | {avg_t:.2f} | {avg_f:.1f} | {avg_d:.1f} |")

    # ── 每张图片行 ──
    per_img_rows = []
    for img_idx in range(len(images)):
        lines = []
        row_data = all_data[img_idx]
        for i in range(len(MODELS)):
            if row_data[i] is None: continue
            lines.append(fmt_row(18+i, MODELS[i][0], row_data[i]))
        per_img_rows.append(lines)

    # ── 读取原文件 ──
    with open(OUTPUT_MD, "r") as f:
        original_lines = f.readlines()

    # ── 删除已有的 #18~#23 行（幂等）──
    cleaned = []
    for line in original_lines:
        m = re.match(r'\|\s*(\d+)\s*\|', line)
        if m:
            num = int(m.group(1))
            if 18 <= num <= 23:
                continue  # 跳过已有的 #18~#23
        cleaned.append(line)

    # ── 重新插入 ──
    output = []
    in_summary = False
    summary_done = False
    img_idx = -1
    img_done = [False] * len(images)

    for line in cleaned:
        # 检测段落
        if "## 一、" in line:
            in_summary = True
        elif line.startswith("---") or "## 二." in line:
            in_summary = False

        if in_summary and not summary_done and re.match(r'\|\s*17\s*\|', line):
            output.append(line)
            for sr in summary_rows:
                output.append(sr + "\n")
            summary_done = True
            continue

        # 跟踪图片索引
        m2 = re.match(r'## 二\.(\d+)', line)
        if m2:
            img_idx = int(m2.group(1)) - 1

        # 在每张图片表格中插入
        if img_idx >= 0 and not img_done[img_idx]:
            if re.match(r'\|\s*17\s*\|', line):
                output.append(line)
                for l in per_img_rows[img_idx]:
                    output.append(l + "\n")
                img_done[img_idx] = True
                continue

        output.append(line)

    with open(OUTPUT_MD, "w") as f:
        f.writelines(output)

    print(f"\n✅ #18~#23 已插入 {OUTPUT_MD}", flush=True)


if __name__ == "__main__":
    main()