// RathoreScan 3D Studio — Local PC Photogrammetry Server
// Copyright (c) 2026 RathoreTech (https://github.com/mrrathoretech602/RathoreScan3D)
// Licensed under the RathoreScan 3D Source-Available & Protected License (see LICENSE).

package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
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

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"status":        "online",
		"engine_ready":  exeErr == nil,
		"ffmpeg_ready":  toolExists(cfg["FFMPEG_BIN"], "ffmpeg"),
		"colmap_ready":  toolExists(cfg["COLMAP_BIN"], "colmap"),
		"openmvs_ready": openmvsOk,
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
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
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
		http.Error(w, "Please upload a video or at least 3 photos.", http.StatusBadRequest)
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

func withHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "SAMEORIGIN")
		w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")
		if r.URL.Path == "/" || r.URL.Path == "/index.html" {
			w.Header().Set("Cache-Control", "no-store")
		}
		next.ServeHTTP(w, r)
	})
}

func main() {
	if logFile, err := os.OpenFile("server.log", os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); err == nil {
		log.SetOutput(io.MultiWriter(os.Stdout, logFile))
	}
	os.MkdirAll("jobs", 0755)

	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.Dir("web")))
	mux.Handle("/jobs/", http.StripPrefix("/jobs/", http.FileServer(http.Dir("jobs"))))
	mux.HandleFunc("/api/health", handleHealth)
	mux.HandleFunc("/api/upload", handleUpload)
	mux.HandleFunc("/api/events", handleEvents)
	mux.HandleFunc("/api/jobs", handleListJobs)

	fmt.Println("====================================================")
	fmt.Println("   RATHORESCAN 3D STUDIO - LOCAL PC APP SERVER")
	fmt.Println("====================================================")
	fmt.Println(" Local PC URL : http://127.0.0.1:8080")
	fmt.Println("====================================================")

	log.Fatal(http.ListenAndServe("127.0.0.1:8080", withHeaders(mux)))
}
