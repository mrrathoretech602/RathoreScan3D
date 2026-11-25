@echo off
title RathoreScan 3D Studio - Build & Launch
echo ====================================================
echo   RATHORESCAN 3D STUDIO - WINDOWS LAUNCHER
echo ====================================================

if not exist "engine\scan3d_engine.exe" (
    echo [1/2] Compiling C++17 Photogrammetry Engine...
    g++ -O3 -std=c++17 engine\scan3d_engine.cpp -o engine\scan3d_engine.exe
    if errorlevel 1 (
        echo [ERROR] g++ compile failed! Make sure MinGW-w64 g++ is in your PATH.
        pause
        exit /b 1
    )
    echo [OK] scan3d_engine.exe compiled successfully!
) else (
    echo [1/2] C++17 Engine already compiled.
)

echo [2/2] Starting Cross-Platform Go Server (PC + Android Wi-Fi)...
go run main.go
pause
