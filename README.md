# RathoreScan 3D Studio (Mobile, Tablet & PC)

100% Self-Hosted Video-to-3D Photogrammetry Studio built with **C++17**, **Go**, and **Three.js (PWA)**.
Shoot a 360-degree video from your Android phone, tablet, or PC, reconstruct a textured 3D mesh locally on your GPU, trim floor/background geometry, and export directly for **Unity** and **Unreal Engine 5**.

> [!IMPORTANT]
> **Notice Before Using This Code (`LICENSE` & `SECURITY.md`):**
> This repository is protected under a **Source-Available & Protected License** ([LICENSE](./LICENSE)) with strict security & ethical-use rules ([SECURITY.md](./SECURITY.md)).
> Commercial monetization, rebranding, or removing attribution without written permission is **prohibited**. Read both files before cloning, forking, or deploying.

## Folder Structure
```text
RathoreScan3D/
├── LICENSE                    # Protected Source-Available License (All Rights Reserved)
├── SECURITY.md                # Security Hardening & Responsible Use Policy
├── vercel.json                # Static UI/PWA Vercel Config + Security Headers
├── .gitignore                 # Blocks certs, binaries, and scan media from Git
├── build_and_run.bat          # 1-Click Windows compile & launch script
├── main.go                    # Cross-platform Go Server (HTTP + Auto-HTTPS + SSE)
├── go.mod                     # Go module manifest
├── tools_config.txt           # Paths for FFmpeg, COLMAP, OpenMVS
├── engine/
│   └── scan3d_engine.cpp      # C++17 Photogrammetry Pipeline Engine
├── web/
│   ├── manifest.json          # Installable Mobile/Tablet/PC PWA Manifest
│   ├── sw.js                  # PWA Service Worker
│   ├── icon-192.png           # App Icon (192x192)
│   ├── icon-512.png           # App Icon (512x512)
│   └── index.html             # Responsive App UI + Build Monitor + 3D Studio
└── jobs/                      # Generated 3D scans & outputs
```

## Quick Setup (Local PC Engine)
1. Install **MinGW-w64 (g++)** and **Go** (if not already installed).
2. Download free open-source native binaries:
   - **FFmpeg** (Windows build)
   - **COLMAP** (Windows CUDA release from GitHub `colmap/colmap`)
   - **OpenMVS** (Windows binaries from GitHub `cdcseacave/openMVS`)
3. Open `tools_config.txt` and update the 3 paths to match your PC folders.

## Run on PC Desktop App, Tablet & Mobile (Vercel Bridge)
- Double-click **`build_and_run.bat`** — it compiles `engine/scan3d_engine.exe`, starts the Go Photogrammetry Server, and opens a **Windows Desktop App** window automatically.
- **Input Modes:** Shoot a 360° video, snap/upload **100+ multi-angle photos**, or combine both (Hybrid mode) for higher accuracy.
- **Poly-Budget Presets:** Choose **Low-Poly Fast** (`1024px`, tiny file size), **Balanced Game-Ready** (`1440px`), **High-Poly Detail** (`1920px`), or **Ultra Photorealistic** (`2400px`), plus live client-side **High-Poly ↔ Low-Poly Simplifier** in 3D Studio.
- **Multi-Format Exports:** `.GLB` (Unity / Unreal Engine 5), `.GLTF`, `.OBJ`, `.STL` (3D Print), `.PLY`, `.USDZ` (iOS AR), and `.JSON`.
- **Live Mobile on Vercel (`rathorescan3d.vercel.app`):**
  - Open `https://rathorescan3d.vercel.app/?server=https://<YOUR_PC_LAN_IP>:8443` (same Wi-Fi) or use a Cloudflare HTTPS tunnel (`cloudflared tunnel --url http://localhost:8080`) for 4G/5G mobile uploads directly to your laptop GPU!

## Deploying Frontend to Vercel
`vercel.json` automatically configures Vercel to serve the responsive PWA & 3D Studio (`web/`) with strict HTTP security headers:
- **Note:** Vercel hosts the **UI, PWA, 3D Studio, Test Mesh, and GLB/OBJ Exporter** only. Full video-to-3D reconstruction requires running the Go + C++17 engine (`FFmpeg`, `COLMAP`, `OpenMVS`) locally on a PC.

## Unity & Unreal Engine 5 Import
- **Unity:** Install `com.unity.cloud.gltfast` via Package Manager (if needed), then drag-and-drop `RathoreScan_Unity.glb` into `Assets/`.
- **Unreal Engine 5:** Drag-and-drop `RathoreScan_Unreal5.glb` directly into the Content Browser.

## License & Security
- **License:** Copyright © 2026 RathoreTech ([mrrathoretech602](https://github.com/mrrathoretech602)). See [LICENSE](./LICENSE).
- **Security:** Never expose the local Go/C++ server to the public internet. See [SECURITY.md](./SECURITY.md).
