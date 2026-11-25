// RathoreScan 3D Studio — Self-Hosted Photogrammetry Server & Vercel Mobile Bridge
// Copyright (c) 2026 RathoreTech (https://github.com/mrrathoretech602/RathoreScan3D)
// Licensed under the RathoreScan 3D Source-Available & Protected License (see LICENSE).

package main

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

type JobEvent struct {
	Percent  int    `json:"percent"`
	Message  string `json:"message"`
	Status   string `json:"status"` // running, done, error
	ModelURL string `json:"model_url,omitempty"`
	MtlURL   string `json:"mtl_url,omitempty"`
}

type JobState struct {
	ID          string
	Events      []JobEvent
	Status      string
	ModelURL    string
	MtlURL      string
	Subscribers map[chan JobEvent]bool
	mu          sync.Mutex
}

var (
	jobs   = make(map[string]*JobState)
	jobsMu sync.RWMutex
	progRe = regexp.MustCompile(`\[PROGRESS:(\d+)\]\s*(.*)`)
)

func getLocalIP() string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return "127.0.0.1"
	}
	for _, a := range addrs {
		if ipnet, ok := a.(*net.IPNet); ok && !ipnet.IP.IsLoopback() {
			if ip4 := ipnet.IP.To4(); ip4 != nil {
				s := ip4.String()
				if strings.HasPrefix(s, "192.168.") || strings.HasPrefix(s, "10.") || strings.HasPrefix(s, "172.") {
					return s
				}
			}
		}
	}
	return "127.0.0.1"
}

func ensureCert(lanIP string) (string, string) {
	certFile, keyFile := "cert.pem", "key.pem"
	if _, err := os.Stat(certFile); err == nil {
		if _, err := os.Stat(keyFile); err == nil {
			return certFile, keyFile
		}
	}

	priv, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	tmpl := x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()),
		Subject:      pkix.Name{CommonName: "RathoreScan3D Local"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageKeyEncipherment | x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP(lanIP)},
		DNSNames:     []string{"localhost"},
	}
	derBytes, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &priv.PublicKey, priv)
	if err != nil {
		log.Fatal(err)
	}
	cOut, err := os.Create(certFile)
	if err != nil {
		log.Fatal(err)
	}
	pem.Encode(cOut, &pem.Block{Type: "CERTIFICATE", Bytes: derBytes})
	cOut.Close()

	b, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		log.Fatal(err)
	}
	kOut, err := os.Create(keyFile)
	if err != nil {
		log.Fatal(err)
	}
	pem.Encode(kOut, &pem.Block{Type: "EC PRIVATE KEY", Bytes: b})
	kOut.Close()
	return certFile, keyFile
}

func (j *JobState) emit(ev JobEvent) {
	j.mu.Lock()
	j.Events = append(j.Events, ev)
	j.Status = ev.Status
	if ev.ModelURL != "" {
		j.ModelURL = ev.ModelURL
		j.MtlURL = ev.MtlURL
	}
	for ch := range j.Subscribers {
		select {
		case ch <- ev:
		default:
		}
	}
	j.mu.Unlock()
}

func readToolPaths() map[string]string {
	res := map[string]string{}
	f, err := os.Open("tools_config.txt")
	if err != nil {
		return res
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) == 2 {
			res[strings.TrimSpace(parts[0])] = strings.TrimSpace(parts[1])
		}
	}
	return res
}

func toolExists(pathVal, fallbackCmd string) bool {
	if pathVal != "" {
		if _, err := os.Stat(pathVal); err == nil {
			return true
		}
	}
	if fallbackCmd != "" {
		if _, err := exec.LookPath(fallbackCmd); err == nil {
			return true
		}
	}
	return false
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	exe := filepath.Join("engine", "scan3d_engine")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	_, exeErr := os.Stat(exe)
	cfg := readToolPaths()
	openmvsOk := false
	if dir := cfg["OPENMVS_DIR"]; dir != "" {
		if st, err := os.Stat(dir); err == nil && st.IsDir() {
			openmvsOk = true
		}
	}

	lanIP := getLocalIP()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"status":            "online",
		"lan_ip":            lanIP,
		"mobile_lan_url":    fmt.Sprintf("https://%s:8443", lanIP),
		"vercel_bridge_url": fmt.Sprintf("https://rathorescan3d.vercel.app/?server=https://%s:8443", lanIP),
		"engine_ready":      exeErr == nil,
		"ffmpeg_ready":      toolExists(cfg["FFMPEG_BIN"], "ffmpeg"),
		"colmap_ready":      toolExists(cfg["COLMAP_BIN"], "colmap"),
		"openmvs_ready":     openmvsOk,
	})
}

func sanitizeVideoExt(name string) string {
	switch strings.ToLower(filepath.Ext(name)) {
	case ".mp4", ".mov", ".webm", ".mkv", ".avi":
		return strings.ToLower(filepath.Ext(name))
	default:
		return ".mp4"
	}
}

func sanitizeImageExt(name string) string {
	switch strings.ToLower(filepath.Ext(name)) {
	case ".jpg", ".jpeg", ".png", ".webp", ".bmp":
		return strings.ToLower(filepath.Ext(name))
	default:
		return ""
	}
}

func handleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	// Allow up to 4GB combined video + multi-photo upload
	r.Body = http.MaxBytesReader(w, r.Body, 4<<30)
	if err := r.ParseMultipartForm(128 << 20); err != nil {
		http.Error(w, "Upload error: "+err.Error(), http.StatusBadRequest)
		return
	}

	preset := r.FormValue("preset")
	switch preset {
	case "lowpoly", "balanced", "highpoly", "ultra":
	default:
		preset = "balanced"
	}

	fpsInt, err := strconv.Atoi(r.FormValue("fps"))
	if err != nil || fpsInt < 1 || fpsInt > 10 {
		fpsInt = 2
	}
	fps := strconv.Itoa(fpsInt)

	jobID := fmt.Sprintf("scan_%d", time.Now().Unix())
	jobDir := filepath.Join("jobs", jobID)
	if err := os.MkdirAll(jobDir, 0755); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	hasVideo := false
	videoArg := "none"
	if file, header, err := r.FormFile("video"); err == nil {
		defer file.Close()
		ext := sanitizeVideoExt(header.Filename)
		videoPath := filepath.Join(jobDir, "input"+ext)
		dst, err := os.Create(videoPath)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		if _, err := io.Copy(dst, file); err != nil {
			dst.Close()
			http.Error(w, "Failed to save video", http.StatusInternalServerError)
			return
		}
		dst.Close()
		hasVideo = true
		videoArg = videoPath
	}

	photoCount := 0
	if r.MultipartForm != nil && len(r.MultipartForm.File["photos"]) > 0 {
		rawPhotoDir := filepath.Join(jobDir, "raw_photos")
		os.MkdirAll(rawPhotoDir, 0755)
		for i, fh := range r.MultipartForm.File["photos"] {
			ext := sanitizeImageExt(fh.Filename)
			if ext == "" {
				continue
			}
			src, err := fh.Open()
			if err != nil {
				continue
			}
			dstPath := filepath.Join(rawPhotoDir, fmt.Sprintf("photo_%04d%s", i+1, ext))
			dst, err := os.Create(dstPath)
			if err != nil {
				src.Close()
				continue
			}
			io.Copy(dst, src)
			dst.Close()
			src.Close()
			photoCount++
		}
	}

	if !hasVideo && photoCount == 0 {
		os.RemoveAll(jobDir)
		http.Error(w, "Please upload a 360° video or at least 3 photos.", http.StatusBadRequest)
		return
	}

	mode := "video"
	if hasVideo && photoCount > 0 {
		mode = "hybrid"
	} else if photoCount > 0 {
		mode = "photos"
	}

	st := &JobState{
		ID:          jobID,
		Status:      "running",
		Subscribers: make(map[chan JobEvent]bool),
	}
	jobsMu.Lock()
	jobs[jobID] = st
	jobsMu.Unlock()

	go runPipeline(st, jobDir, mode, videoArg, preset, fps, photoCount)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"job_id":      jobID,
		"mode":        mode,
		"photo_count": photoCount,
	})
}

func runPipeline(st *JobState, jobDir, mode, videoArg, preset, fps string, photoCount int) {
	st.emit(JobEvent{
		Percent: 2,
		Message: fmt.Sprintf("Input received (mode=%s, photos=%d, preset=%s). Starting C++17 Engine...", mode, photoCount, preset),
		Status:  "running",
	})

	exe := filepath.Join("engine", "scan3d_engine")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}

	cmd := exec.Command(exe, jobDir, mode, videoArg, preset, fps)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		st.emit(JobEvent{Percent: 0, Message: "Engine pipe error: " + err.Error(), Status: "error"})
		return
	}
	cmd.Stderr = cmd.Stdout

	if err := cmd.Start(); err != nil {
		st.emit(JobEvent{Percent: 0, Message: "Failed to start C++ engine: " + err.Error(), Status: "error"})
		return
	}

	scanner := bufio.NewScanner(stdout)
	lastPct := 5
	for scanner.Scan() {
		line := scanner.Text()
		if m := progRe.FindStringSubmatch(line); m != nil {
			p, _ := strconv.Atoi(m[1])
			lastPct = p
			st.emit(JobEvent{Percent: p, Message: m[2], Status: "running"})
		} else if strings.Contains(line, "[ERROR]") {
			st.emit(JobEvent{Percent: lastPct, Message: line, Status: "error"})
		} else {
			st.emit(JobEvent{Percent: lastPct, Message: line, Status: "running"})
		}
	}

	if err := cmd.Wait(); err != nil {
		st.emit(JobEvent{Percent: lastPct, Message: "Pipeline exited with error: " + err.Error(), Status: "error"})
		return
	}

	modelURL := fmt.Sprintf("/jobs/%s/output/model.obj", st.ID)
	mtlURL := fmt.Sprintf("/jobs/%s/output/model.mtl", st.ID)
	st.emit(JobEvent{
		Percent:  100,
		Message:  "3D Reconstruction Complete!",
		Status:   "done",
		ModelURL: modelURL,
		MtlURL:   mtlURL,
	})
}

func handleEvents(w http.ResponseWriter, r *http.Request) {
	jobID := r.URL.Query().Get("job")
	jobsMu.RLock()
	st, ok := jobs[jobID]
	jobsMu.RUnlock()
	if !ok {
		http.Error(w, "Job not found", http.StatusNotFound)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	flusher, ok := w.(http.Flusher)
	if !ok {
		return
	}

	ch := make(chan JobEvent, 64)
	st.mu.Lock()
	history := append([]JobEvent(nil), st.Events...)
	st.Subscribers[ch] = true
	st.mu.Unlock()

	defer func() {
		st.mu.Lock()
		delete(st.Subscribers, ch)
		st.mu.Unlock()
	}()

	for _, ev := range history {
		b, _ := json.Marshal(ev)
		fmt.Fprintf(w, "data: %s\n\n", b)
	}
	flusher.Flush()

	for {
		select {
		case ev := <-ch:
			b, _ := json.Marshal(ev)
			fmt.Fprintf(w, "data: %s\n\n", b)
			flusher.Flush()
			if ev.Status == "done" || ev.Status == "error" {
				return
			}
		case <-r.Context().Done():
			return
		}
	}
}

func handleListJobs(w http.ResponseWriter, r *http.Request) {
	type ScanItem struct {
		ID       string `json:"id"`
		ModelURL string `json:"model_url"`
		MtlURL   string `json:"mtl_url"`
	}
	var list []ScanItem
	entries, _ := os.ReadDir("jobs")
	for _, e := range entries {
		if e.IsDir() {
			objPath := filepath.Join("jobs", e.Name(), "output", "model.obj")
			if _, err := os.Stat(objPath); err == nil {
				list = append(list, ScanItem{
					ID:       e.Name(),
					ModelURL: "/jobs/" + e.Name() + "/output/model.obj",
					MtlURL:   "/jobs/" + e.Name() + "/output/model.mtl",
				})
			}
		}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(list)
}

func withSecurityAndCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "SAMEORIGIN")
		w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")

		origin := r.Header.Get("Origin")
		if origin != "" {
			w.Header().Set("Access-Control-Allow-Origin", origin)
			w.Header().Set("Vary", "Origin")
		} else {
			w.Header().Set("Access-Control-Allow-Origin", "*")
		}
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Accept")
		w.Header().Set("Access-Control-Allow-Private-Network", "true")

		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func main() {
	if logFile, err := os.OpenFile("server.log", os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); err == nil {
		log.SetOutput(io.MultiWriter(os.Stdout, logFile))
	}
	os.MkdirAll("jobs", 0755)
	lanIP := getLocalIP()
	certFile, keyFile := ensureCert(lanIP)

	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.Dir("web")))
	mux.Handle("/jobs/", http.StripPrefix("/jobs/", http.FileServer(http.Dir("jobs"))))
	mux.HandleFunc("/api/health", handleHealth)
	mux.HandleFunc("/api/upload", handleUpload)
	mux.HandleFunc("/api/events", handleEvents)
	mux.HandleFunc("/api/jobs", handleListJobs)
	handler := withSecurityAndCORS(mux)

	fmt.Println("================================================================")
	fmt.Println("   RATHORESCAN 3D STUDIO - PC APP + LIVE VERCEL MOBILE BRIDGE")
	fmt.Println("================================================================")
	fmt.Printf(" PC Desktop App : http://localhost:8080\n")
	fmt.Printf(" Mobile Wi-Fi   : https://%s:8443\n", lanIP)
	fmt.Printf(" Vercel Bridge  : https://rathorescan3d.vercel.app/?server=https://%s:8443\n", lanIP)
	fmt.Println(" Tip (4G/5G): Run `cloudflared tunnel --url http://localhost:8080`")
	fmt.Println("              and open https://rathorescan3d.vercel.app/?server=<tunnel-url>")
	fmt.Println("================================================================")

	go func() {
		log.Fatal(http.ListenAndServe(":8080", handler))
	}()
	log.Fatal(http.ListenAndServeTLS(":8443", certFile, keyFile, handler))
}
