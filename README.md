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

## Run on PC, Tablet & Android
- Double-click **`build_and_run.bat`** (or run `g++ -O3 -std=c++17 engine/scan3d_engine.cpp -o engine/scan3d_engine.exe` and then `go run main.go`).
- **PC Browser:** Open `http://localhost:8080`
- **Android Phone / Tablet (Same Wi-Fi / Hotspot):**
  - Open the HTTPS URL printed in the terminal (e.g., `https://192.168.1.x:8443`), accept the local self-signed certificate once, and tap **Install App / Add to Home Screen**.
  - Or open `http://192.168.1.x:8080` and use the **Record Native 4K Video / Pick File** button.

## Deploying Frontend to Vercel
`vercel.json` automatically configures Vercel to serve the responsive PWA & 3D Studio (`web/`) with strict HTTP security headers:
- **Note:** Vercel hosts the **UI, PWA, 3D Studio, Test Mesh, and GLB/OBJ Exporter** only. Full video-to-3D reconstruction requires running the Go + C++17 engine (`FFmpeg`, `COLMAP`, `OpenMVS`) locally on a PC.

## Unity & Unreal Engine 5 Import
- **Unity:** Install `com.unity.cloud.gltfast` via Package Manager (if needed), then drag-and-drop `RathoreScan_Unity.glb` into `Assets/`.
- **Unreal Engine 5:** Drag-and-drop `RathoreScan_Unreal5.glb` directly into the Content Browser.

## License & Security
- **License:** Copyright © 2026 RathoreTech ([mrrathoretech602](https://github.com/mrrathoretech602)). See [LICENSE](./LICENSE).
- **Security:** Never expose the local Go/C++ server to the public internet. See [SECURITY.md](./SECURITY.md).
