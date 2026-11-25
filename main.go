// RathoreScan 3D Studio — Self-Hosted Photogrammetry Server
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

func handleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	// Allow up to 2GB video stream
	r.Body = http.MaxBytesReader(w, r.Body, 2<<30)
	if err := r.ParseMultipartForm(64 << 20); err != nil {
		http.Error(w, "Upload error: "+err.Error(), http.StatusBadRequest)
		return
	}

	file, header, err := r.FormFile("video")
	if err != nil {
		http.Error(w, "Video file missing", http.StatusBadRequest)
		return
	}
	defer file.Close()

	preset := r.FormValue("preset")
	switch preset {
	case "fast", "balanced", "ultra":
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

	ext := strings.ToLower(filepath.Ext(header.Filename))
	switch ext {
	case ".mp4", ".mov", ".webm", ".mkv", ".avi":
	default:
		ext = ".mp4"
	}
	videoPath := filepath.Join(jobDir, "input"+ext)
	dst, err := os.Create(videoPath)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if _, err := io.Copy(dst, file); err != nil {
		dst.Close()
		http.Error(w, "Failed to save uploaded video", http.StatusInternalServerError)
		return
	}
	dst.Close()

	st := &JobState{
		ID:          jobID,
		Status:      "running",
		Subscribers: make(map[chan JobEvent]bool),
	}
	jobsMu.Lock()
	jobs[jobID] = st
	jobsMu.Unlock()

	go runPipeline(st, jobDir, videoPath, preset, fps)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"job_id": jobID})
}

func runPipeline(st *JobState, jobDir, videoPath, preset, fps string) {
	st.emit(JobEvent{Percent: 2, Message: "Video received! Launching C++17 3D Engine...", Status: "running"})

	exe := filepath.Join("engine", "scan3d_engine")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}

	cmd := exec.Command(exe, jobDir, videoPath, preset, fps)
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

func withSecurityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "SAMEORIGIN")
		w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")
		next.ServeHTTP(w, r)
	})
}

func main() {
	os.MkdirAll("jobs", 0755)
	lanIP := getLocalIP()
	certFile, keyFile := ensureCert(lanIP)

	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.Dir("web")))
	mux.Handle("/jobs/", http.StripPrefix("/jobs/", http.FileServer(http.Dir("jobs"))))
	mux.HandleFunc("/api/upload", handleUpload)
	mux.HandleFunc("/api/events", handleEvents)
	mux.HandleFunc("/api/jobs", handleListJobs)
	handler := withSecurityHeaders(mux)

	fmt.Println("====================================================")
	fmt.Println("   RATHORESCAN 3D STUDIO - ANDROID + PC SERVER")
	fmt.Println("====================================================")
	fmt.Printf(" PC Local URL   : http://localhost:8080\n")
	fmt.Printf(" Android Camera : https://%s:8443  (Accept self-signed warning once)\n", lanIP)
	fmt.Printf(" Android Simple : http://%s:8080   (Use Native 4K Camera button)\n", lanIP)
	fmt.Println("====================================================")

	go func() {
		log.Fatal(http.ListenAndServe(":8080", handler))
	}()
	log.Fatal(http.ListenAndServeTLS(":8443", certFile, keyFile, handler))
}
