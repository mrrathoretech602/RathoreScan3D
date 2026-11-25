// RathoreScan 3D Studio — Windows Desktop App Launcher
// Starts RathoreScan3D_Server.exe silently in the background if not already running,
// waits for http://127.0.0.1:8080/api/health, and opens the frameless desktop app window.

package main

import (
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"time"
)

func isServerReady() bool {
	client := http.Client{Timeout: 800 * time.Millisecond}
	resp, err := client.Get("http://127.0.0.1:8080/api/health")
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

func startBackgroundServer(appDir string) {
	serverExe := filepath.Join(appDir, "RathoreScan3D_Server.exe")
	cmd := exec.Command(serverExe)
	cmd.Dir = appDir
	cmd.SysProcAttr = &syscall.SysProcAttr{
		HideWindow:    true,
		CreationFlags: 0x08000000 | 0x00000008, // CREATE_NO_WINDOW | DETACHED_PROCESS
	}
	_ = cmd.Start()
}

func openAppWindow() {
	url := "http://127.0.0.1:8080"
	candidates := []string{
		`C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`,
		`C:\Program Files\Microsoft\Edge\Application\msedge.exe`,
		`C:\Program Files\Google\Chrome\Application\chrome.exe`,
	}
	for _, browser := range candidates {
		if _, err := os.Stat(browser); err == nil {
			cmd := exec.Command(browser, "--app="+url, "--window-size=1360,860")
			if err := cmd.Start(); err == nil {
				return
			}
		}
	}
	// Fallback to default Windows handler
	_ = exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
}

func main() {
	exePath, err := os.Executable()
	appDir := "."
	if err == nil {
		appDir = filepath.Dir(exePath)
	}
	_ = os.Chdir(appDir)

	if !isServerReady() {
		startBackgroundServer(appDir)
		for i := 0; i < 40; i++ {
			if isServerReady() {
				break
			}
			time.Sleep(250 * time.Millisecond)
		}
	}
	openAppWindow()
}
