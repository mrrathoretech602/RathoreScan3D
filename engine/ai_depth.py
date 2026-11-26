"""
RathoreScan 3D — Local Neural Monocular Depth Estimator (Depth-Anything-V2-Small)
Outputs a 16-bit binary PGM (P5) depth map where higher values = closer to camera.
"""
import sys
import os
import warnings
warnings.filterwarnings("ignore")

def main():
    if len(sys.argv) < 5:
        print("Usage: ai_depth.py <input_image> <output_pgm> <width> <height>", file=sys.stderr)
        return 1

    in_path = sys.argv[1]
    out_path = sys.argv[2]
    target_w = int(sys.argv[3])
    target_h = int(sys.argv[4])

    import numpy as np
    from PIL import Image
    import torch
    from transformers import AutoImageProcessor, AutoModelForDepthEstimation

    model_id = "depth-anything/Depth-Anything-V2-Small-hf"
    processor = AutoImageProcessor.from_pretrained(model_id)
    model = AutoModelForDepthEstimation.from_pretrained(model_id)
    model.eval()

    image = Image.open(in_path).convert("RGB")
    inputs = processor(images=image, return_tensors="pt")

    with torch.no_grad():
        outputs = model(**inputs)
        predicted_depth = outputs.predicted_depth

    prediction = torch.nn.functional.interpolate(
        predicted_depth.unsqueeze(1),
        size=(target_h, target_w),
        mode="bicubic",
        align_corners=False,
    ).squeeze().cpu().numpy()

    d_min = float(prediction.min())
    d_max = float(prediction.max())
    if d_max - d_min > 1e-6:
        norm = (prediction - d_min) / (d_max - d_min)
    else:
        norm = np.zeros_like(prediction)

    depth_u16 = np.clip(np.round(norm * 65535.0), 0, 65535).astype(">u2")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        header = f"P5\n{target_w} {target_h}\n65535\n".encode("ascii")
        f.write(header)
        f.write(depth_u16.tobytes())

    return 0

if __name__ == "__main__":
    sys.exit(main())
