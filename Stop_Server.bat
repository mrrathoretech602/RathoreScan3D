@echo off
title RathoreScan 3D Studio - Stop Background Server
taskkill /F /IM RathoreScan3D_Server.exe >nul 2>&1
if errorlevel 1 (
    echo RathoreScan3D background server is not running.
) else (
    echo [OK] RathoreScan3D background server stopped.
)
timeout /t 2 >nul
