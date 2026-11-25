@echo off
title RathoreScan 3D Studio - PC Desktop App & Server
echo ====================================================
echo   RATHORESCAN 3D STUDIO - WINDOWS DESKTOP LAUNCHER
echo ====================================================

echo [1/3] Checking C++17 Photogrammetry Engine...
where g++ >nul 2>&1
if not errorlevel 1 (
    g++ -O3 -std=c++17 engine\scan3d_engine.cpp -o engine\scan3d_engine.exe
    if errorlevel 1 (
        echo [WARN] g++ compile failed! Check engine\scan3d_engine.cpp
    ) else (
        echo [OK] scan3d_engine.exe compiled successfully!
    )
) else (
    if not exist "engine\scan3d_engine.exe" (
        echo [WARN] MinGW g++ not found in PATH. Install MinGW-w64 to compile C++17 engine.
    ) else (
        echo [OK] Using existing engine\scan3d_engine.exe
    )
)

echo [2/3] Launching Windows Desktop App Window...
start "" cmd /C "timeout /t 2 >nul && (msedge --app=http://localhost:8080 || chrome --app=http://localhost:8080 || start http://localhost:8080)"

echo [3/3] Starting Go Photogrammetry Server (PC + Mobile + Vercel Bridge)...
where go >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Go is not installed or not in PATH. Install Go 1.21+ from https://go.dev/dl/
    pause
    exit /b 1
)
go run main.go
pause
