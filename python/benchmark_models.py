#!/usr/bin/env python3
"""
批量测试 YOLO 后端性能并将结果写入 Markdown 文件。

用法:
    python3 python/benchmark_models.py                        # 写入 model/test_rate_pc.md
    python3 python/benchmark_models.py --output model/test_rate_pi.md   # 写入指定文件

扫描 data/images/test*.jpg，对每张图片运行所有模型（OpenCV + NCNN），
生成 1 张汇总表 + N 张独立明细表。
"""

import subprocess
import sys
import re
import statistics
from pathlib import Path
from datetime import datetime

BENCHMARK = "./build/etest_yolo_benchmark"
CLASSES = "model/onnx_640_640/classes.txt"
ONNX_MODEL = "model/onnx_640_640/best.onnx"
WARMUP = 30
ITERATIONS = 200
THREADS = 4


def run_benchmark(args: list[str]) -> dict | None:
    """运行一次 benchmark，从 stdout 解析各阶段数据。"""
    cmd = [BENCHMARK] + args
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=600)
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
        m = re.match(
            r"(\S+)\s+mean=\s*([0-9.]+).*fps=\s*([0-9.]+)",
            line.strip())
        if m:
            phase = m.group(1).lower()
            stats[phase] = {
                "mean_ms": float(m.group(2)),
                "fps": float(m.group(3)),
            }

    det_m = re.search(r"Average detections/frame:\s*([0-9.]+)", stdout)
    if det_m:
        stats["detections"] = float(det_m.group(1))
    else:
        stats["detections"] = 0.0

    # 检测签名
    sig_section = stdout.find("─── 首帧检测签名 ───")
    if sig_section >= 0:
        sigs = stdout[sig_section:].splitlines()
        det_list = []
        for line in sigs:
            m2 = re.match(r"class=(\d+)\s+confidence=([0-9.]+)", line)
            if m2:
                det_list.append(f"c{m2.group(1)}@{m2.group(2)}")
        stats["signature"] = "; ".join(det_list) if det_list else "none"

    return stats


def discover_images() -> list[str]:
    """返回 data/images/ 下所有 test*.jpg 的排序列表。"""
    images = sorted(Path("data/images").glob("test*.jpg"))
    return [str(p) for p in images]


def discover_ncnn_models() -> list[dict]:
    """扫描 model/ncnn_*x*/ 下 best_*.ncnn.param，返回模型列表。"""
    models = []
    for d in sorted(Path("model").glob("ncnn_*x*")):
        for p in sorted(d.glob("best_*.ncnn.param")):
            m = re.match(r"best_(\d+)x(\d+)_(fp\d+)\.ncnn\.param", p.name)
            if not m:
                continue
            models.append({
                "param": str(p),
                "width": int(m.group(1)),
                "height": int(m.group(2)),
                "precision": m.group(3).upper(),
            })
    return models


def bench_all_images(output_md: str):
    """主函数：对所有图片跑所有模型，生成 Markdown。"""
    images = discover_images()
    ncnn_models = discover_ncnn_models()

    print("=" * 60)
    print("YOLO 多图 Benchmark Runner")
    print(f"图片数: {len(images)}")
    print(f"NCNN 模型数: {len(ncnn_models)}")
    print(f"总任务数: {(1 + len(ncnn_models)) * len(images)}")
    print(f"输出文件: {output_md}")
    print(f"Binary:   {BENCHMARK}")
    print("=" * 60, flush=True)

    # 三维结构: image_row[model_idx] = stats dict
    all_data: list[list[dict | None]] = []

    for img_idx, img_path in enumerate(images):
        img_name = Path(img_path).name
        print(f"\n{'='*50}", flush=True)
        print(f"[{img_idx + 1}/{len(images)}] {img_name}", flush=True)
        print(f"{'='*50}", flush=True)

        row = []

        # ── OpenCV 基线 ──
        print(f"  OpenCV 640x640...", flush=True)
        ocv = run_benchmark([
            "--backend", "opencv",
            "--model", ONNX_MODEL,
            "--classes", CLASSES,
            "--image", img_path,
            "--width", "640", "--height", "640",
            "--threads", str(THREADS),
            "--warmup", str(WARMUP),
            "--iterations", str(ITERATIONS),
        ])
        if ocv:
            ocv["_label"] = "OpenCV 640×640 FP32"
        row.append(ocv)

        # ── NCNN 模型 ──
        for model in ncnn_models:
            w, h = model["width"], model["height"]
            prec = model["precision"]
            label = f"NCNN {w}×{h} {prec}"
            print(f"  {label}...", flush=True)

            extra = []
            if prec == "FP16":
                extra = ["--fp16-storage", "--fp16-arithmetic"]

            s = run_benchmark([
                "--backend", "ncnn",
                "--param", model["param"],
                "--classes", CLASSES,
                "--image", img_path,
                "--width", str(w), "--height", str(h),
                "--threads", str(THREADS),
                "--warmup", str(WARMUP),
                "--iterations", str(ITERATIONS),
            ] + extra)
            if s:
                s["_label"] = label
            row.append(s)

        all_data.append(row)

    # ── 写入 Markdown ──
    model_labels = (
        [r["_label"] for r in all_data[0] if r]
        if all_data else []
    )
    write_markdown(images, all_data, model_labels, output_md)
    print(f"\n✅ 完成！结果写入 {output_md}", flush=True)


def get_val(row, key, default=0.0):
    """安全获取 row[key]['mean_ms'] 或 row[key]"""
    if row is None:
        return default
    phase = row.get(key)
    if phase is None:
        return default
    if isinstance(phase, dict):
        return phase.get("mean_ms", default)
    return phase


def write_markdown(images, all_data, labels, filepath):
    """写入完整的 Markdown 文件：汇总 + 每张图片独立表。"""
    with open(filepath, "w") as f:
        f.write("# YOLO 后端性能基准测试\n\n")
        f.write(f"**生成时间：** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        f.write(f"**测试图片数：** {len(images)}\n\n")
        f.write(f"**预热次数：** {WARMUP}　|　**测试次数：** {ITERATIONS}　|　**线程数：** {THREADS}\n\n")
        f.write(f"**图片列表：** ")
        f.write("、".join(Path(p).name for p in images))
        f.write("\n\n---\n\n")

        # ── 表 1：汇总表 ──
        f.write("## 一、汇总表（全部图片平均值）\n\n")
        f.write("| # | 后端 | 输入尺寸 | 精度 | 平均总耗时 (ms) | 平均 FPS | 平均检测数 |\n")
        f.write("|---|------|----------|------|----------------|----------|------------|\n")

        for i, label in enumerate(labels):
            totals = []
            fpss = []
            dets = []
            for row in all_data:
                entry = row[i] if i < len(row) else None
                if entry:
                    totals.append(get_val(entry, "total"))
                    fpss.append(get_val(entry, "total", 0.0))
                    if entry.get("total"):
                        fpss[-1] = entry["total"].get("fps", 0.0)
                    dets.append(entry.get("detections", 0.0))

            avg_tot = statistics.mean(totals) if totals else 0
            avg_fps = statistics.mean(fpss) if fpss else 0
            avg_det = statistics.mean(dets) if dets else 0

            parts = label.split()
            backend = parts[0]
            size = parts[1] if len(parts) > 1 else "-"
            prec = parts[2] if len(parts) > 2 else "-"

            f.write(f"| {i + 1} | {backend} | {size} | {prec} | "
                    f"{avg_tot:.2f} | {avg_fps:.1f} | {avg_det:.1f} |\n")

        f.write("\n---\n\n")

        # ── 每张图片独立表 ──
        for img_idx, (img_path, row) in enumerate(zip(images, all_data)):
            img_name = Path(img_path).name
            f.write(f"## 二.{img_idx + 1}　`{img_name}`\n\n")
            f.write("| # | 后端 | 输入尺寸 | 精度 | 预处理 (ms) | 推理 (ms) | "
                    "解码 (ms) | NMS (ms) | 总耗时 (ms) | FPS | 检测数 | 签名 |\n")
            f.write("|---|------|----------|------|------------|-----------|"
                    "-----------|---------|------------|-----|--------|------|\n")

            for i, entry in enumerate(row):
                if entry is None:
                    continue
                label = entry.get("_label", f"#{i}")
                parts = label.split()
                backend = parts[0]
                size = parts[1] if len(parts) > 1 else "-"
                prec = parts[2] if len(parts) > 2 else "-"

                pre = get_val(entry, "preprocess")
                fwd = get_val(entry, "forward")
                dec = get_val(entry, "decode")
                nms = get_val(entry, "nms")
                tot = get_val(entry, "total")
                fps = entry.get("total", {}).get("fps", 0) if entry.get("total") else 0
                det = entry.get("detections", 0)
                sig = entry.get("signature", "-")

                f.write(f"| {i + 1} | {backend} | {size} | {prec} | "
                        f"{pre:.2f} | {fwd:.2f} | {dec:.2f} | {nms:.2f} | "
                        f"{tot:.2f} | {fps:.1f} | {det:.1f} | {sig} |\n")

            f.write("\n")

    print(f"\n已写入: {filepath}", flush=True)


def main():
    """解析命令行参数并运行基准测试。"""
    output_md = "model/test_rate_pc.md"  # 默认 PC
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--output" and i + 1 < len(args):
            output_md = args[i + 1]
            i += 2
        else:
            i += 1

    bench_all_images(output_md)


if __name__ == "__main__":
    main()