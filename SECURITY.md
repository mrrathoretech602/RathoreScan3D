# Security & Responsible Use Policy — RathoreScan 3D Studio

## 1. Responsible & Ethical Use (Safety Guidelines)
Before cloning, deploying, or running **RathoreScan 3D Studio**, all users and contributors must agree to the following safety rules:
- **Consent Required:** Never capture, reconstruct, or distribute 3D scans of people, faces, private residences, restricted facilities, or copyrighted physical products without explicit permission.
- **No Unlawful Usage:** Use for authorized 3D modeling, game asset creation, education, and personal R&D only.

## 2. Deployment Security Warnings
- **Do NOT Expose the Go/C++ Server to the Public Internet:**
  `main.go` and `scan3d_engine.exe` are built for **trusted local Wi-Fi / LAN use only**. Exposing port `8080` or `8443` publicly without authentication, reverse-proxy rate limiting, and strict firewall rules exposes your PC storage and GPU to abuse.
- **Vercel / Static Hosting Mode:**
  Deploying `web/` on Vercel hosts the **UI, PWA, and Three.js 3D Studio (Demo/OBJ viewer)** only. Actual video-to-3D photogrammetry requires running the Go server + C++17 engine (`FFmpeg`, `COLMAP`, `OpenMVS`) locally on a PC with a GPU.
- **Self-Signed TLS (`cert.pem` / `key.pem`):**
  The local server generates self-signed certificates for LAN mobile camera access. Never commit `.pem`, `.key`, or `.env` files to Git.
- **Resource & Disk Safety:**
  Uploads are capped at 2 GB (`MaxBytesReader`), and input parameters (`preset`, `fps`, video extension) are strictly allowlisted to prevent command injection. Monitor the `jobs/` folder disk usage regularly.

## 3. Input Validation & Hardening Checklist
- Video uploads only accept safe container extensions (`.mp4`, `.mov`, `.webm`, `.mkv`, `.avi`).
- `preset` is restricted to `fast`, `balanced`, `ultra`.
- `fps` is restricted to validated numeric values (`1`–`10`).
- HTTP security headers (`X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`) are enforced in both `main.go` and `vercel.json`.

## 4. Reporting a Security Vulnerability
If you discover a vulnerability (command injection, path traversal, XSS, or DoS vector):
1. **Do NOT** open a public issue with exploit payloads.
2. Report privately via GitHub Security Advisories at:
   `https://github.com/mrrathoretech602/RathoreScan3D/security/advisories`
3. Include steps to reproduce, affected file/line, and suggested remediation.
