#!/usr/bin/env python3
# AI生成
"""
将 YOLOv5 PyTorch 模型 (.pt) 转换为 ONNX 格式 (.onnx)。

用法:
    python3 python/convert_onnx.py [--input model/yolov5s.pt] [--output model/yolov5s.onnx]

依赖:
    pip install torch torchvision onnx
"""

import argparse
import os
import sys


def convert(input_path: str, output_path: str) -> None:
    input_abs = os.path.abspath(input_path)

    if not os.path.isfile(input_abs):
        print(f"错误: 找不到输入文件: {input_abs}")
        sys.exit(1)

    print(f"正在加载模型: {input_abs}")

    import torch

    # 加载 YOLOv5 模型（支持 ultralytics 和旧版 YOLOv5 仓库）
    model = torch.hub.load(
        "ultralytics/yolov5",
        "custom",
        path=input_abs,
        force_reload=False,
    )

    model.eval()

    # 导出为 ONNX。
    # 输入尺寸与 detectNn() 中硬编码的 640x640 保持一致。
    img_size = 640
    dummy_input = torch.randn(1, 3, img_size, img_size)

    print(f"正在导出 ONNX 到: {os.path.abspath(output_path)}")

    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=["images"],
        output_names=["output"],
        opset_version=12,
        dynamic_axes={
            "images": {0: "batch"},
            "output": {0: "batch"},
        },
    )

    print(f"ONNX 模型已保存到: {os.path.abspath(output_path)}")
    print(f"文件大小: {os.path.getsize(output_path) / (1024 * 1024):.1f} MB")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="将 YOLOv5 .pt 模型转换为 ONNX .onnx",
    )

    parser.add_argument(
        "--input",
        type=str,
        default="model/yolov5s.pt",
        help="输入 .pt 文件路径 (默认: model/yolov5s.pt)",
    )

    parser.add_argument(
        "--output",
        type=str,
        default="model/yolov5s.onnx",
        help="输出 .onnx 文件路径 (默认: model/yolov5s.onnx)",
    )

    args = parser.parse_args()
    convert(args.input, args.output)


if __name__ == "__main__":
    main()