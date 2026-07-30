#!/usr/bin/env python3
"""
追加测试 160×160 / 160×224 / 160×640 模型（FP16 + FP32）到现有 test_rate_pi.md
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
OUTPUT_MD = "model/test_rate_pi.md"

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


def main():
    images = discover_images()
    img_names = [p.name for p in images]

    print("=" * 60)
    print(f"160×640 追加 Benchmark: {len(images)} images × {len(MODELS)} models")
    print(f"输出: {OUTPUT_MD}")
    print("=" * 60, flush=True)

    # all_data[img_idx][model_idx] = stats dict | None
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

    # ── 读取原文件内容，删除已有的追加段落 ──
    with open(OUTPUT_MD, "r") as f:
        original = f.read()

    # 删除从 "## 追加" 开始的所有内容
    idx = original.find("## 追加")
    if idx >= 0:
        original = original[:idx].rstrip("\n")

    # ── 构建追加内容 ──
    labels = [m[0] for m in MODELS]
    model_count = len(MODELS)

    append_lines = []
    append_lines.append("")
    append_lines.append("---")
    append_lines.append("")
    append_lines.append("## 追加：160×640 模型（本次测试）")
    append_lines.append("")
    append_lines.append(f"**生成时间：** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    append_lines.append("")
    append_lines.append("### 汇总表（全部图片平均值）")
    append_lines.append("")
    append_lines.append("| # | 后端 | 输入尺寸 | 精度 | 平均总耗时 (ms) | 平均 FPS | 平均检测数 |")
    append_lines.append("|---|------|----------|------|----------------|----------|------------|")

    # 重新编号：接在原文 17 个模型之后
    base_idx = 17  # 原文最后一个是 17

    for i, label in enumerate(labels):
        totals, fpss, dets = [], [], []
        for row in all_data:
            entry = row[i] if i < len(row) else None
            if entry:
                totals.append(entry.get("total", {}).get("mean_ms", 0))
                fpss.append(entry.get("total", {}).get("fps", 0))
                dets.append(entry.get("detections", 0))
        avg_tot = statistics.mean(totals) if totals else 0
        avg_fps = statistics.mean(fpss) if fpss else 0
        avg_det = statistics.mean(dets) if dets else 0
        parts = label.split()
        backend = parts[0]
        size = parts[1] if len(parts) > 1 else "-"
        prec = parts[2] if len(parts) > 2 else "-"
        append_lines.append(
            f"| {base_idx + i + 1} | {backend} | {size} | {prec} | "
            f"{avg_tot:.2f} | {avg_fps:.1f} | {avg_det:.1f} |"
        )

    append_lines.append("")
    append_lines.append("---")
    append_lines.append("")

    # 每张图片的明细表
    for img_idx, (img_name, row) in enumerate(zip(img_names, all_data)):
        append_lines.append(f"### 二.{img_idx + 1}　`{img_name}`")
        append_lines.append("")
        append_lines.append("| # | 后端 | 输入尺寸 | 精度 | 预处理 (ms) | 推理 (ms) | "
                            "解码 (ms) | NMS (ms) | 总耗时 (ms) | FPS | 检测数 | 签名 |")
        append_lines.append("|---|------|----------|------|------------|-----------|"
                            "-----------|---------|------------|-----|--------|------|")

        for i, entry in enumerate(row):
            if entry is None:
                continue
            label = entry.get("_label", f"#{i}")
            parts = label.split()
            backend = parts[0]
            size = parts[1] if len(parts) > 1 else "-"
            prec = parts[2] if len(parts) > 2 else "-"
            pre = entry.get("preprocess", {}).get("mean_ms", 0)
            fwd = entry.get("forward", {}).get("mean_ms", 0)
            dec = entry.get("decode", {}).get("mean_ms", 0)
            nms = entry.get("nms", {}).get("mean_ms", 0)
            tot = entry.get("total", {}).get("mean_ms", 0)
            fps = entry.get("total", {}).get("fps", 0)
            det = entry.get("detections", 0)
            sig = entry.get("signature", "-")
            append_lines.append(
                f"| {base_idx + i + 1} | {backend} | {size} | {prec} | "
                f"{pre:.2f} | {fwd:.2f} | {dec:.2f} | {nms:.2f} | "
                f"{tot:.2f} | {fps:.1f} | {det:.1f} | {sig} |"
            )
        append_lines.append("")

    new_content = original.rstrip("\n") + "\n" + "\n".join(append_lines) + "\n"

    with open(OUTPUT_MD, "w") as f:
        f.write(new_content)

    print(f"\n✅ 追加完成 → {OUTPUT_MD}", flush=True)


if __name__ == "__main__":
    main()