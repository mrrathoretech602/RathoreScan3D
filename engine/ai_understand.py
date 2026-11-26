"""
RathoreScan 3D — Local AI Scene & Object Understanding Engine
Answers: WHAT is each part, WHERE is subject vs sky/background, WHY/HOW to reconstruct.
Uses cached local HuggingFace models:
  - nvidia/segformer-b0-finetuned-ade-512-512 (150-class semantic segmentation)
  - depth-anything/Depth-Anything-V2-Small-hf (neural depth estimation)
"""
import json
import os
import sys
import warnings

warnings.filterwarnings("ignore")

import numpy as np
import torch
from PIL import Image, ImageFilter
from transformers import AutoImageProcessor, SegformerForSemanticSegmentation

SEG_MODEL_ID = "nvidia/segformer-b0-finetuned-ade-512-512"

# ADE20K class groups (0..149)
SKY_LABELS = {"sky"}
PERSON_LABELS = {"person"}
BOTANICAL_LABELS = {"tree", "plant", "palm", "flower", "grass"}
ARCH_LABELS = {
    "building", "house", "skyscraper", "hovel", "tower", "booth",
    "awning", "bridge", "fence", "railing", "stairway", "stairs",
    "step", "door", "windowpane", "column", "pillar", "roof"
}
GROUND_LABELS = {"floor", "earth", "ground", "sidewalk", "pavement", "road", "path", "dirt track", "sand", "land"}
BG_NOISE_LABELS = {"sky", "ceiling", "wall", "windowpane"}


def load_segformer():
    proc = AutoImageProcessor.from_pretrained(SEG_MODEL_ID)
    model = SegformerForSemanticSegmentation.from_pretrained(SEG_MODEL_ID)
    model.eval()
    return proc, model


def segment_image(proc, model, img_pil):
    w, h = img_pil.size
    inputs = proc(images=img_pil, return_tensors="pt")
    with torch.no_grad():
        logits = model(**inputs).logits
    upsampled = torch.nn.functional.interpolate(
        logits, size=(h, w), mode="bilinear", align_corners=False
    )
    pred = upsampled.argmax(dim=1)[0].cpu().numpy().astype(np.uint8)
    return pred


def classify_scene(class_pcts, scene_mode):
    person_pct = class_pcts.get("person", 0.0)
    arch_pct = sum(class_pcts.get(k, 0.0) for k in ("building", "house", "hovel", "tower", "skyscraper"))
    tree_pct = sum(class_pcts.get(k, 0.0) for k in ("tree", "palm", "plant"))
    sky_pct = class_pcts.get("sky", 0.0)

    if scene_mode == "tree":
        archetype = "botanical"
    elif scene_mode == "full":
        archetype = "environment"
    elif scene_mode == "subject":
        if person_pct >= 8.0 and person_pct >= arch_pct * 0.6:
            archetype = "character"
        elif arch_pct >= 8.0:
            archetype = "architecture"
        elif tree_pct >= 12.0:
            archetype = "botanical"
        else:
            archetype = "object"
    else:  # auto
        if person_pct >= 12.0 and person_pct >= arch_pct * 0.7:
            archetype = "character"
        elif arch_pct >= 8.0:
            archetype = "architecture"
        elif tree_pct >= 15.0:
            archetype = "botanical"
        elif sky_pct >= 10.0:
            archetype = "object"
        else:
            archetype = "environment"
    return archetype


def build_foreground_mask(pred, id2label, archetype, img_rgb_np):
    h, w = pred.shape
    keep = np.zeros((h, w), dtype=bool)

    if archetype == "character":
        target_set = PERSON_LABELS
        for cid, label in id2label.items():
            if label in target_set:
                keep |= (pred == int(cid))
        # Fallback to center ellipse if person mask is tiny
        if keep.mean() < 0.03:
            yy, xx = np.ogrid[:h, :w]
            keep = (((xx - w * 0.5) / (w * 0.28)) ** 2 + ((yy - h * 0.5) / (h * 0.48)) ** 2) <= 1.0
    elif archetype == "architecture":
        target_set = ARCH_LABELS | BOTANICAL_LABELS | {"sidewalk", "floor", "earth", "pot", "flower", "lamp", "light", "sconce", "step", "stairs"}
        for cid, label in id2label.items():
            if label in target_set:
                keep |= (pred == int(cid))
        # Also remove dark outer vignette / glowing text near top sky border
        sky_ids = [int(cid) for cid, lbl in id2label.items() if lbl in SKY_LABELS]
        for sid in sky_ids:
            keep &= (pred != sid)
    elif archetype == "botanical":
        for cid, label in id2label.items():
            if label in BOTANICAL_LABELS:
                keep |= (pred == int(cid))
    elif archetype == "environment":
        keep[:] = True
        for cid, label in id2label.items():
            if label in SKY_LABELS:
                keep &= (pred != int(cid))
    else:  # generic object
        for cid, label in id2label.items():
            if label not in (SKY_LABELS | {"wall", "floor", "ceiling", "road", "sidewalk"}):
                keep |= (pred == int(cid))
        if keep.mean() < 0.05:
            keep[:] = True
            for cid, label in id2label.items():
                if label in SKY_LABELS:
                    keep &= (pred != int(cid))

    # Dilate mask slightly (11px) so object edges & SIFT keypoints around silhouettes are preserved
    mask_img = Image.fromarray((keep.astype(np.uint8) * 255), mode="L")
    mask_img = mask_img.filter(ImageFilter.MaxFilter(11))
    # Suppress watermark/logo in bottom-right corner and top-10% sky text margin if sky dominates top
    arr = np.array(mask_img)
    top_sky = sum((pred[: max(1, int(h * 0.18)), :] == int(cid)).mean() for cid, lbl in id2label.items() if lbl in SKY_LABELS)
    if top_sky > 0.5:
        arr[: int(h * 0.14), :] = 0
    arr[int(h * 0.88):, int(w * 0.88):] = 0
    return Image.fromarray(arr, mode="L")


def cmd_analyze_batch(images_dir, masks_dir, scene_mode, out_json):
    os.makedirs(masks_dir, exist_ok=True)
    files = sorted(
        f for f in os.listdir(images_dir)
        if f.lower().endswith((".jpg", ".jpeg", ".png", ".webp", ".bmp"))
    )
    if not files:
        print("No images found", file=sys.stderr)
        return 1

    proc, model = load_segformer()
    id2label = model.config.id2label

    # Step 1: Sample up to 6 evenly spaced frames to understand overall scene & subject
    sample_idx = np.linspace(0, len(files) - 1, min(6, len(files)), dtype=int)
    agg_counts = {}
    total_px = 0
    for idx in sample_idx:
        p = os.path.join(images_dir, files[int(idx)])
        img = Image.open(p).convert("RGB")
        pred = segment_image(proc, model, img)
        vals, counts = np.unique(pred, return_counts=True)
        for v, c in zip(vals, counts):
            lbl = id2label[int(v)]
            agg_counts[lbl] = agg_counts.get(lbl, 0) + int(c)
        total_px += pred.size

    class_pcts = {k: round(100.0 * v / max(1, total_px), 1) for k, v in agg_counts.items()}
    archetype = classify_scene(class_pcts, scene_mode)
    top_parts = sorted(class_pcts.items(), key=lambda kv: kv[1], reverse=True)[:6]

    # Step 2: Generate per-frame semantic masks so COLMAP ignores sky/watermarks/contradictory BG
    kept_ratios = []
    for fname in files:
        p = os.path.join(images_dir, fname)
        img = Image.open(p).convert("RGB")
        pred = segment_image(proc, model, img)
        mask_img = build_foreground_mask(pred, id2label, archetype, np.array(img))
        kept_ratios.append(float((np.array(mask_img) > 127).mean()))
        mask_img.save(os.path.join(masks_dir, fname + ".png"))

    avg_kept = round(100.0 * sum(kept_ratios) / max(1, len(kept_ratios)), 1)
    parts_str = ", ".join(f"{k} ({v}%)" for k, v in top_parts)
    report = {
        "archetype": archetype,
        "scene_mode_requested": scene_mode,
        "frames_analyzed": len(files),
        "top_components": [{"label": k, "percent": v} for k, v in top_parts],
        "foreground_kept_percent": avg_kept,
        "masked_out_percent": round(max(0.0, 100.0 - avg_kept), 1),
        "what": f"{archetype.title()} scene: {parts_str}",
        "where": f"Subject isolated in {avg_kept}% active frame; {round(max(0.0, 100.0 - avg_kept), 1)}% sky/background/watermarks masked out.",
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_json)), exist_ok=True)
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    print(f"[AI-WHAT] {report['what']}")
    print(f"[AI-WHERE] {report['where']}")
    return 0


def cmd_deep_analyze(image_path, out_seg_pgm, out_depth_pgm, target_w, target_h):
    from transformers import AutoModelForDepthEstimation

    target_w = int(target_w)
    target_h = int(target_h)
    img = Image.open(image_path).convert("RGB")

    # 1. SegFormer semantic class map: 0=sky/bg, 1=solid/arch/object, 2=person, 3=tree/foliage, 4=ground
    proc, seg_model = load_segformer()
    id2label = seg_model.config.id2label
    pred = segment_image(proc, seg_model, img.resize((target_w, target_h), Image.BILINEAR))

    sem_map = np.ones((target_h, target_w), dtype=np.uint8)
    for cid, lbl in id2label.items():
        m = (pred == int(cid))
        if lbl in SKY_LABELS:
            sem_map[m] = 0
        elif lbl in PERSON_LABELS:
            sem_map[m] = 2
        elif lbl in BOTANICAL_LABELS:
            sem_map[m] = 3
        elif lbl in GROUND_LABELS:
            sem_map[m] = 4

    os.makedirs(os.path.dirname(os.path.abspath(out_seg_pgm)), exist_ok=True)
    with open(out_seg_pgm, "wb") as f:
        f.write(f"P5\n{target_w} {target_h}\n255\n".encode("ascii"))
        f.write(sem_map.tobytes())

    # 2. Depth-Anything-V2-Small neural depth
    depth_id = "depth-anything/Depth-Anything-V2-Small-hf"
    d_proc = AutoImageProcessor.from_pretrained(depth_id)
    d_mod = AutoModelForDepthEstimation.from_pretrained(depth_id)
    d_mod.eval()
    d_in = d_proc(images=img, return_tensors="pt")
    with torch.no_grad():
        d_out = d_mod(**d_in).predicted_depth
    d_pred = torch.nn.functional.interpolate(
        d_out.unsqueeze(1), size=(target_h, target_w), mode="bicubic", align_corners=False
    ).squeeze().cpu().numpy()
    d_min, d_max = float(d_pred.min()), float(d_pred.max())
    norm = (d_pred - d_min) / (d_max - d_min) if (d_max - d_min) > 1e-6 else np.zeros_like(d_pred)
    depth_u16 = np.clip(np.round(norm * 65535.0), 0, 65535).astype(">u2")
    with open(out_depth_pgm, "wb") as f:
        f.write(f"P5\n{target_w} {target_h}\n65535\n".encode("ascii"))
        f.write(depth_u16.tobytes())
    return 0


def main():
    if len(sys.argv) < 2:
        return 1
    cmd = sys.argv[1]
    if cmd == "analyze-batch" and len(sys.argv) >= 6:
        return cmd_analyze_batch(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    if cmd == "deep-analyze" and len(sys.argv) >= 7:
        return cmd_deep_analyze(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
    print("Invalid arguments", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
