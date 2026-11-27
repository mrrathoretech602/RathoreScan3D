// RathoreScan 3D Studio — C++17 Photogrammetry & 1-2 Photo AI+Volumetric 3D Engine
// Copyright (c) 2026 RathoreTech (https://github.com/mrrathoretech602/RathoreScan3D)
// Licensed under the RathoreScan 3D Source-Available & Protected License (see LICENSE).

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <unordered_map>

namespace fs = std::filesystem;

struct ToolConfig {
    std::string ffmpeg = "ffmpeg";
    std::string colmap = "colmap";
    std::string openmvsDir = "";
    std::string pythonBin = "python";
};

ToolConfig loadConfig(const std::string& path) {
    ToolConfig cfg;
    std::ifstream file(path);
    if (!file.is_open()) return cfg;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        auto last = val.find_last_not_of(" \r\n\t");
        if (last != std::string::npos) val.erase(last + 1);
        else val.clear();
        if (key == "FFMPEG_BIN") cfg.ffmpeg = val;
        else if (key == "COLMAP_BIN") cfg.colmap = val;
        else if (key == "OPENMVS_DIR") cfg.openmvsDir = val;
        else if (key == "PYTHON_BIN") cfg.pythonBin = val;
    }
    return cfg;
}

void logProgress(int pct, const std::string& msg) {
    std::cout << "[PROGRESS:" << pct << "] " << msg << std::endl;
    std::cout.flush();
}

void logError(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
    std::cerr.flush();
    std::cout.flush();
}

int runCmd(const std::string& cmd, const std::string& workDir = "") {
    std::string fullCmd;
#ifdef _WIN32
    if (!workDir.empty()) {
        fullCmd = "cmd /S /C \"cd /D \"" + workDir + "\" && " + cmd + "\"";
    } else {
        fullCmd = "cmd /S /C \"" + cmd + "\"";
    }
#else
    if (!workDir.empty()) {
        fullCmd = "cd \"" + workDir + "\" && " + cmd;
    } else {
        fullCmd = cmd;
    }
#endif
    std::cout << "[EXEC] " << cmd << std::endl;
    std::cout.flush();
    return std::system(fullCmd.c_str());
}

std::string mvsExe(const ToolConfig& cfg, const std::string& name) {
    if (cfg.openmvsDir.empty()) return name;
#ifdef _WIN32
    fs::path p = fs::path(cfg.openmvsDir) / (name + ".exe");
#else
    fs::path p = fs::path(cfg.openmvsDir) / name;
#endif
    return "\"" + p.string() + "\"";
}

bool isImageExt(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp";
}

struct RGBImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb; // size = width * height * 3
};

bool loadPPM(const fs::path& ppmPath, RGBImage& img) {
    std::ifstream in(ppmPath, std::ios::binary);
    if (!in.is_open()) return false;
    auto nextToken = [&]() -> std::string {
        std::string tok;
        char c;
        while (in.get(c)) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            if (c == '#') {
                std::string dummy;
                std::getline(in, dummy);
                continue;
            }
            tok.push_back(c);
            while (in.get(c) && !std::isspace(static_cast<unsigned char>(c))) tok.push_back(c);
            break;
        }
        return tok;
    };
    if (nextToken() != "P6") return false;
    int w = std::stoi(nextToken());
    int h = std::stoi(nextToken());
    int maxVal = std::stoi(nextToken());
    if (w <= 0 || h <= 0 || maxVal <= 0) return false;
    img.width = w;
    img.height = h;
    img.rgb.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    in.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    return static_cast<size_t>(in.gcount()) == img.rgb.size();
}

bool loadPGM16(const fs::path& pgmPath, int expectedW, int expectedH, std::vector<float>& outDepth) {
    std::ifstream in(pgmPath, std::ios::binary);
    if (!in.is_open()) return false;
    auto nextToken = [&]() -> std::string {
        std::string tok;
        char c;
        while (in.get(c)) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            if (c == '#') {
                std::string dummy;
                std::getline(in, dummy);
                continue;
            }
            tok.push_back(c);
            while (in.get(c) && !std::isspace(static_cast<unsigned char>(c))) tok.push_back(c);
            break;
        }
        return tok;
    };
    if (nextToken() != "P5") return false;
    int w = std::stoi(nextToken());
    int h = std::stoi(nextToken());
    int maxVal = std::stoi(nextToken());
    if (w != expectedW || h != expectedH || maxVal <= 0) return false;

    size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> raw(n * 2);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (static_cast<size_t>(in.gcount()) != raw.size()) return false;

    outDepth.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint16_t val = (static_cast<uint16_t>(raw[i * 2]) << 8) | static_cast<uint16_t>(raw[i * 2 + 1]);
        outDepth[i] = static_cast<float>(val) / static_cast<float>(maxVal);
    }
    return true;
}

bool decodeImageToPPM(const ToolConfig& cfg, const fs::path& srcImg, const fs::path& dstPpm, int maxDim) {
    std::string cmd = "\"" + cfg.ffmpeg + "\" -v error -y -i \"" + srcImg.string() +
        "\" -vf \"scale='if(gt(iw,ih),min(" + std::to_string(maxDim) + ",iw),-2)':'if(gt(iw,ih),-2,min(" +
        std::to_string(maxDim) + ",ih))':flags=lanczos\" -pix_fmt rgb24 -f image2 -vcodec ppm \"" +
        dstPpm.string() + "\"";
    return runCmd(cmd) == 0 && fs::exists(dstPpm);
}

bool loadPGM8(const fs::path& pgmPath, int expectedW, int expectedH, std::vector<uint8_t>& outData) {
    std::ifstream in(pgmPath, std::ios::binary);
    if (!in.is_open()) return false;
    auto nextToken = [&]() -> std::string {
        std::string tok;
        char c;
        while (in.get(c)) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            if (c == '#') { std::string d; std::getline(in, d); continue; }
            tok.push_back(c);
            while (in.get(c) && !std::isspace(static_cast<unsigned char>(c))) tok.push_back(c);
            break;
        }
        return tok;
    };
    if (nextToken() != "P5") return false;
    int w = std::stoi(nextToken());
    int h = std::stoi(nextToken());
    int maxVal = std::stoi(nextToken());
    if (w != expectedW || h != expectedH || maxVal <= 0) return false;
    size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    outData.resize(n);
    in.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(n));
    return static_cast<size_t>(in.gcount()) == n;
}

bool tryEstimateAiDepth(const ToolConfig& cfg, const fs::path& srcImg, const fs::path& outPgm, int w, int h, std::vector<float>& aiDepth) {
    fs::path script = fs::exists("engine/ai_depth.py")
        ? fs::path("engine/ai_depth.py")
        : fs::path("../engine/ai_depth.py");
    if (!fs::exists(script)) return false;

    std::string py = cfg.pythonBin.empty() ? "python" : ("\"" + cfg.pythonBin + "\"");
    std::string cmd = py + " \"" + fs::absolute(script).string() + "\" \"" + srcImg.string() + "\" \"" +
        outPgm.string() + "\" " + std::to_string(w) + " " + std::to_string(h);
    if (runCmd(cmd) != 0 || !fs::exists(outPgm)) return false;
    return loadPGM16(outPgm, w, h, aiDepth);
}

bool tryAiSemanticAndDepth(
    const ToolConfig& cfg,
    const fs::path& srcImg,
    const fs::path& outSegPgm,
    const fs::path& outDepthPgm,
    int w, int h,
    std::vector<uint8_t>& outSeg,
    std::vector<float>& outDepth
) {
    fs::path script = fs::exists("engine/ai_understand.py")
        ? fs::path("engine/ai_understand.py")
        : fs::path("../engine/ai_understand.py");
    if (fs::exists(script)) {
        std::string py = cfg.pythonBin.empty() ? "python" : ("\"" + cfg.pythonBin + "\"");
        std::string cmd = py + " \"" + fs::absolute(script).string() + "\" deep-analyze \"" + srcImg.string() +
            "\" \"" + outSegPgm.string() + "\" \"" + outDepthPgm.string() + "\" " +
            std::to_string(w) + " " + std::to_string(h);
        if (runCmd(cmd) == 0 && loadPGM8(outSegPgm, w, h, outSeg) && loadPGM16(outDepthPgm, w, h, outDepth)) {
            return true;
        }
    }
    outSeg.clear();
    return tryEstimateAiDepth(cfg, srcImg, outDepthPgm, w, h, outDepth);
}

struct AiSceneInfo {
    bool valid = false;
    std::string archetype = "object";
    std::string what = "";
    std::string where = "";
};

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

std::string extractJsonStringField(const std::string& jsonText, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto p = jsonText.find(pat);
    if (p == std::string::npos) return "";
    p = jsonText.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p = jsonText.find('"', p);
    if (p == std::string::npos) return "";
    auto end = jsonText.find('"', p + 1);
    while (end != std::string::npos && jsonText[end - 1] == '\\') {
        end = jsonText.find('"', end + 1);
    }
    return (end != std::string::npos) ? jsonText.substr(p + 1, end - p - 1) : "";
}

AiSceneInfo runAiUnderstandBatch(
    const ToolConfig& cfg,
    const fs::path& imgDir,
    const fs::path& masksDir,
    const std::string& sceneMode,
    const fs::path& outJson
) {
    AiSceneInfo info;
    fs::path script = fs::exists("engine/ai_understand.py")
        ? fs::path("engine/ai_understand.py")
        : fs::path("../engine/ai_understand.py");
    if (!fs::exists(script)) return info;

    std::string py = cfg.pythonBin.empty() ? "python" : ("\"" + cfg.pythonBin + "\"");
    std::string cmd = py + " \"" + fs::absolute(script).string() + "\" analyze-batch \"" +
        imgDir.string() + "\" \"" + masksDir.string() + "\" " + sceneMode + " \"" + outJson.string() + "\"";
    if (runCmd(cmd) != 0 || !fs::exists(outJson)) return info;

    std::ifstream in(outJson);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string txt = ss.str();
    info.archetype = extractJsonStringField(txt, "archetype");
    info.what = extractJsonStringField(txt, "what");
    info.where = extractJsonStringField(txt, "where");
    if (info.archetype.empty()) info.archetype = "object";
    info.valid = true;
    return info;
}

double computeOrbitSpanDeg(const fs::path& imagesTxtPath) {
    std::ifstream in(imagesTxtPath);
    if (!in.is_open()) return 0.0;
    struct CamPos { double x, y, z; };
    std::vector<CamPos> cams;
    std::string line;
    bool expectPointsLine = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (expectPointsLine) { expectPointsLine = false; continue; }
        std::istringstream iss(line);
        int id = 0;
        double qw, qx, qy, qz, tx, ty, tz;
        if (!(iss >> id >> qw >> qx >> qy >> qz >> tx >> ty >> tz)) continue;
        expectPointsLine = true;
        // Camera center C = -R^T * t
        double r00 = 1 - 2*qy*qy - 2*qz*qz, r01 = 2*qx*qy - 2*qz*qw, r02 = 2*qx*qz + 2*qy*qw;
        double r10 = 2*qx*qy + 2*qz*qw,     r11 = 1 - 2*qx*qx - 2*qz*qz, r12 = 2*qy*qz - 2*qx*qw;
        double r20 = 2*qx*qz - 2*qy*qw,     r21 = 2*qy*qz + 2*qx*qw,     r22 = 1 - 2*qx*qx - 2*qy*qy;
        double cx = -(r00*tx + r10*ty + r20*tz);
        double cy = -(r01*tx + r11*ty + r21*tz);
        double cz = -(r02*tx + r12*ty + r22*tz);
        cams.push_back({cx, cy, cz});
    }
    if (cams.size() < 2) return 0.0;
    double mx = 0, mz = 0;
    for (const auto& c : cams) { mx += c.x; mz += c.z; }
    mx /= cams.size(); mz /= cams.size();
    std::vector<double> angles;
    for (const auto& c : cams) {
        angles.push_back(std::atan2(c.z - mz, c.x - mx));
    }
    std::sort(angles.begin(), angles.end());
    double maxGap = 0.0;
    const double TWO_PI = 6.283185307179586;
    for (size_t i = 0; i + 1 < angles.size(); ++i) {
        maxGap = std::max(maxGap, angles[i + 1] - angles[i]);
    }
    maxGap = std::max(maxGap, (angles.front() + TWO_PI) - angles.back());
    double spanRad = std::clamp(TWO_PI - maxGap, 0.0, TWO_PI);
    return spanRad * 180.0 / 3.141592653589793;
}

void writeSceneReportJson(
    const fs::path& outPath,
    const AiSceneInfo& ai,
    const std::string& why,
    const std::string& how,
    double orbitDeg,
    int regViews,
    int totalViews
) {
    std::ofstream out(outPath, std::ios::trunc);
    out << "{\n"
        << "  \"archetype\": \"" << jsonEscape(ai.archetype) << "\",\n"
        << "  \"what\": \"" << jsonEscape(ai.what.empty() ? "3D Subject / Scene" : ai.what) << "\",\n"
        << "  \"where\": \"" << jsonEscape(ai.where.empty() ? "Active frame region" : ai.where) << "\",\n"
        << "  \"why\": \"" << jsonEscape(why) << "\",\n"
        << "  \"how\": \"" << jsonEscape(how) << "\",\n"
        << "  \"orbit_deg\": " << static_cast<int>(std::round(orbitDeg)) << ",\n"
        << "  \"views_registered\": " << regViews << ",\n"
        << "  \"total_views\": " << totalViews << "\n"
        << "}\n";
}

std::vector<fs::path> deduplicateViews(const ToolConfig& cfg, const fs::path& imgDir, const fs::path& tmpDir) {
    std::vector<fs::path> allFiles;
    for (const auto& entry : fs::directory_iterator(imgDir)) {
        if (entry.is_regular_file() && isImageExt(entry.path().extension().string())) {
            allFiles.push_back(entry.path());
        }
    }
    std::sort(allFiles.begin(), allFiles.end());

    std::vector<fs::path> uniqueFiles;
    std::vector<std::vector<uint8_t>> uniqueThumbs;
    fs::create_directories(tmpDir);

    for (size_t i = 0; i < allFiles.size(); ++i) {
        fs::path thumbPpm = tmpDir / ("thumb_" + std::to_string(i) + ".ppm");
        std::string cmd = "\"" + cfg.ffmpeg + "\" -v error -y -i \"" + allFiles[i].string() +
            "\" -vf \"scale=48:48:flags=area\" -pix_fmt rgb24 -f image2 -vcodec ppm \"" + thumbPpm.string() + "\"";
        RGBImage thumb;
        if (std::system(("cmd /S /C \"" + cmd + "\"").c_str()) != 0 || !loadPPM(thumbPpm, thumb)) {
            uniqueFiles.push_back(allFiles[i]);
            continue;
        }
        bool isDup = false;
        for (const auto& prev : uniqueThumbs) {
            if (prev.size() != thumb.rgb.size()) continue;
            double diffSum = 0.0;
            for (size_t k = 0; k < prev.size(); ++k) {
                diffSum += std::abs(static_cast<int>(prev[k]) - static_cast<int>(thumb.rgb[k]));
            }
            if ((diffSum / static_cast<double>(prev.size())) < 4.5) {
                isDup = true;
                break;
            }
        }
        if (!isDup) {
            uniqueFiles.push_back(allFiles[i]);
            uniqueThumbs.push_back(std::move(thumb.rgb));
        }
    }
    return uniqueFiles;
}

struct SubjectAnalysis {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> fg;      // 1 = foreground, 0 = background
    std::vector<uint8_t> foliage; // 1 = leaf/foliage pixel, 0 = wood/solid
    std::vector<uint8_t> alpha;   // 0..255 cutout alpha
    std::vector<float> lum;       // 0..1 luminance
    std::vector<float> depth;     // >= 0 half-thickness (isolated) or Z coord (full scene)
    std::vector<float> zCenter;   // organic Z offset (for botanical canopy clusters)
    std::vector<float> aiNorm;    // normalized 0..1 AI depth inside foreground
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
    float trunkCenterX = 0.0f;
    int trunkTopY = 0;
    int fgCount = 0;
    bool hasIsolatedSubject = false;
    bool isBotanicalTree = false;
    bool hasAiDepth = false;
};

SubjectAnalysis analyzeSubject(
    const RGBImage& img,
    const std::string& sceneMode,
    float worldScale,
    const std::vector<float>* aiDepthRaw,
    const std::vector<uint8_t>* aiSegRaw = nullptr
) {
    SubjectAnalysis out;
    int w = img.width;
    int h = img.height;
    int n = w * h;
    out.width = w;
    out.height = h;
    out.fg.assign(n, 1);
    out.foliage.assign(n, 0);
    out.alpha.assign(n, 255);
    out.lum.assign(n, 0.0f);
    out.depth.assign(n, 0.0f);
    out.zCenter.assign(n, 0.0f);
    out.aiNorm.assign(n, 0.5f);
    out.hasAiDepth = (aiDepthRaw != nullptr && static_cast<int>(aiDepthRaw->size()) == n);
    bool hasSeg = (aiSegRaw != nullptr && static_cast<int>(aiSegRaw->size()) == n);

    for (int i = 0; i < n; ++i) {
        float r = img.rgb[i * 3 + 0] / 255.0f;
        float g = img.rgb[i * 3 + 1] / 255.0f;
        float b = img.rgb[i * 3 + 2] / 255.0f;
        out.lum[i] = 0.299f * r + 0.587f * g + 0.114f * b;
    }

    if (hasSeg && sceneMode != "full") {
        // Use neural semantic map directly to strip sky (0) and separate foliage (3)
        int skyCount = 0;
        for (int i = 0; i < n; ++i) {
            if ((*aiSegRaw)[i] == 0) { out.fg[i] = 0; skyCount++; }
            else if ((*aiSegRaw)[i] == 3) out.foliage[i] = 1;
        }
        if (skyCount > n * 95 / 100) {
            std::fill(out.fg.begin(), out.fg.end(), 1);
        }
    }

    // 1. Border background detection
    double sumR = 0, sumG = 0, sumB = 0;
    int borderCount = 0;
    auto samplePixel = [&](int x, int y) {
        size_t idx = (static_cast<size_t>(y) * w + x) * 3;
        sumR += img.rgb[idx];
        sumG += img.rgb[idx + 1];
        sumB += img.rgb[idx + 2];
        borderCount++;
    };
    for (int x = 0; x < w; ++x) {
        samplePixel(x, 0);
        if (h > 1) samplePixel(x, h - 1);
    }
    for (int y = 1; y < h - 1; ++y) {
        samplePixel(0, y);
        if (w > 1) samplePixel(w - 1, y);
    }
    double bgR = sumR / std::max(1, borderCount);
    double bgG = sumG / std::max(1, borderCount);
    double bgB = sumB / std::max(1, borderCount);

    double varSum = 0.0;
    auto accumVar = [&](int x, int y) {
        size_t idx = (static_cast<size_t>(y) * w + x) * 3;
        double dr = img.rgb[idx] - bgR;
        double dg = img.rgb[idx + 1] - bgG;
        double db = img.rgb[idx + 2] - bgB;
        varSum += std::sqrt((dr * dr + dg * dg + db * db) / 3.0);
    };
    for (int x = 0; x < w; ++x) {
        accumVar(x, 0);
        if (h > 1) accumVar(x, h - 1);
    }
    for (int y = 1; y < h - 1; ++y) {
        accumVar(0, y);
        if (w > 1) accumVar(w - 1, y);
    }
    double borderStd = varSum / std::max(1, borderCount);

    // Only isolate background in 'auto' mode when the image genuinely has a uniform studio backdrop (borderStd < 16.0).
    // Complex scenes/portraits with dark/textured corners (borderStd >= 16) stay intact as full-frame AI-depth 3D scenes!
    bool tryIsolate = (sceneMode != "full") && (borderStd < 16.0 || sceneMode == "subject" || sceneMode == "tree");
    if (tryIsolate) {
        double thresh = std::clamp(26.0 + borderStd * 1.4, 26.0, 62.0);
        auto bgScoreAt = [&](int idx) -> double {
            int r = img.rgb[idx * 3 + 0];
            int g = img.rgb[idx * 3 + 1];
            int b = img.rgb[idx * 3 + 2];
            double dr = std::abs(r - bgR);
            double dg = std::abs(g - bgG);
            double db = std::abs(b - bgB);
            return 0.55 * std::max({dr, dg, db}) + 0.45 * ((dr + dg + db) / 3.0);
        };

        std::vector<uint8_t> visited(n, 0);
        std::queue<int> q;
        for (int x = 0; x < w; ++x) {
            int topIdx = x, botIdx = (h - 1) * w + x;
            if (bgScoreAt(topIdx) < thresh * 1.2) { visited[topIdx] = 1; out.fg[topIdx] = 0; q.push(topIdx); }
            if (bgScoreAt(botIdx) < thresh * 1.2) { visited[botIdx] = 1; out.fg[botIdx] = 0; q.push(botIdx); }
        }
        for (int y = 1; y < h - 1; ++y) {
            int leftIdx = y * w, rightIdx = y * w + (w - 1);
            if (bgScoreAt(leftIdx) < thresh * 1.2) { visited[leftIdx] = 1; out.fg[leftIdx] = 0; q.push(leftIdx); }
            if (bgScoreAt(rightIdx) < thresh * 1.2) { visited[rightIdx] = 1; out.fg[rightIdx] = 0; q.push(rightIdx); }
        }

        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        while (!q.empty()) {
            int cur = q.front();
            q.pop();
            int cx = cur % w, cy = cur / w;
            for (int d = 0; d < 4; ++d) {
                int nx = cx + dx[d], ny = cy + dy[d];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                int ni = ny * w + nx;
                if (!visited[ni] && bgScoreAt(ni) < thresh) {
                    visited[ni] = 1;
                    out.fg[ni] = 0;
                    q.push(ni);
                }
            }
        }

        bool isBrightWhiteBg = (bgR > 230 && bgG > 230 && bgB > 230 && borderStd < 22.0);
        bool isPureBlackBg = (bgR < 22 && bgG < 22 && bgB < 22 && borderStd < 22.0);
        if (isBrightWhiteBg || isPureBlackBg) {
            for (int i = 0; i < n; ++i) {
                if (out.fg[i] == 0) continue;
                int r = img.rgb[i * 3 + 0], g = img.rgb[i * 3 + 1], b = img.rgb[i * 3 + 2];
                int chroma = std::max({r, g, b}) - std::min({r, g, b});
                if (isBrightWhiteBg && r > 224 && g > 224 && b > 224 && chroma < 24) out.fg[i] = 0;
                else if (isPureBlackBg && r < 22 && g < 22 && b < 22 && chroma < 14) out.fg[i] = 0;
            }
        }

        // 2-pass fringe defringing
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<uint8_t> nextFg = out.fg;
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    int i = y * w + x;
                    if (!out.fg[i]) continue;
                    bool adjBg = !out.fg[i - 1] || !out.fg[i + 1] || !out.fg[i - w] || !out.fg[i + w];
                    if (!adjBg) continue;
                    int r = img.rgb[i * 3 + 0], g = img.rgb[i * 3 + 1], b = img.rgb[i * 3 + 2];
                    int chroma = std::max({r, g, b}) - std::min({r, g, b});
                    if (bgScoreAt(i) < thresh * 1.85 || (isBrightWhiteBg && out.lum[i] > 0.72f && chroma < 28)) {
                        nextFg[i] = 0;
                    }
                }
            }
            out.fg.swap(nextFg);
        }

        std::vector<uint8_t> cleaned = out.fg;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int idx = y * w + x;
                if (out.fg[idx] && (out.fg[idx - 1] + out.fg[idx + 1] + out.fg[idx - w] + out.fg[idx + w] == 0)) {
                    cleaned[idx] = 0;
                }
            }
        }
        out.fg.swap(cleaned);
    }

    int fgCount = 0;
    int greenCount = 0;
    out.minX = w; out.maxX = -1; out.minY = h; out.maxY = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            if (out.fg[i]) {
                fgCount++;
                out.minX = std::min(out.minX, x);
                out.maxX = std::max(out.maxX, x);
                out.minY = std::min(out.minY, y);
                out.maxY = std::max(out.maxY, y);
                int r = img.rgb[i * 3 + 0], g = img.rgb[i * 3 + 1], b = img.rgb[i * 3 + 2];
                if (g > r + 6 && g > b + 5) {
                    greenCount++;
                    out.foliage[i] = 1;
                }
            } else {
                out.alpha[i] = 0;
            }
        }
    }
    if (fgCount < (n / 200)) {
        std::fill(out.fg.begin(), out.fg.end(), 1);
        std::fill(out.alpha.begin(), out.alpha.end(), 255);
        fgCount = n;
        out.minX = 0; out.maxX = w - 1; out.minY = 0; out.maxY = h - 1;
    }
    out.fgCount = fgCount;
    out.hasIsolatedSubject = (fgCount < static_cast<int>(n * 0.94));

    int subjH = std::max(1, out.maxY - out.minY);
    int trunkBottomZoneY = out.maxY - std::max(4, subjH * 24 / 100);
    double trunkXSum = 0.0;
    int trunkPxCount = 0;
    for (int y = trunkBottomZoneY; y <= out.maxY; ++y) {
        for (int x = out.minX; x <= out.maxX; ++x) {
            if (out.fg[y * w + x]) { trunkXSum += x; trunkPxCount++; }
        }
    }
    out.trunkCenterX = (trunkPxCount > 0)
        ? static_cast<float>(trunkXSum / trunkPxCount)
        : (out.minX + out.maxX) * 0.5f;

    int topMinX = w, topMaxX = -1, botMinX = w, botMaxX = -1;
    int midY = out.minY + subjH * 55 / 100;
    for (int y = out.minY; y <= out.maxY; ++y) {
        for (int x = out.minX; x <= out.maxX; ++x) {
            if (!out.fg[y * w + x]) continue;
            if (y < midY) { topMinX = std::min(topMinX, x); topMaxX = std::max(topMaxX, x); }
            else { botMinX = std::min(botMinX, x); botMaxX = std::max(botMaxX, x); }
        }
    }
    int topSpan = (topMaxX >= topMinX) ? (topMaxX - topMinX) : 0;
    int botSpan = (botMaxX >= botMinX) ? (botMaxX - botMinX) : 0;
    float greenRatio = static_cast<float>(greenCount) / static_cast<float>(std::max(1, fgCount));
    out.isBotanicalTree = (sceneMode == "tree") ||
        (out.hasIsolatedSubject && (greenRatio > 0.10f || (topSpan > botSpan * 1.65f && subjH > 40)));

    // Find trunk-to-canopy transition height (where green foliage or wide branching begins)
    out.trunkTopY = out.minY + subjH * 55 / 100;
    if (out.isBotanicalTree) {
        for (int y = out.maxY; y >= out.minY; --y) {
            int rowSpanMin = w, rowSpanMax = -1, rowGreen = 0, rowFg = 0;
            for (int x = out.minX; x <= out.maxX; ++x) {
                int i = y * w + x;
                if (out.fg[i]) {
                    rowFg++;
                    rowSpanMin = std::min(rowSpanMin, x);
                    rowSpanMax = std::max(rowSpanMax, x);
                    if (out.foliage[i]) rowGreen++;
                }
            }
            if (rowFg > 0 && (rowGreen > rowFg / 3 || (rowSpanMax - rowSpanMin) > std::max(28, topSpan / 4))) {
                out.trunkTopY = y;
                break;
            }
        }
        // Clear accidental foliage flag inside trunk zone
        for (int y = out.trunkTopY; y <= out.maxY; ++y) {
            for (int x = out.minX; x <= out.maxX; ++x) {
                out.foliage[y * w + x] = 0;
            }
        }
    }

    // Normalize AI depth strictly over foreground pixels (robust p02..p98 percentile)
    if (out.hasAiDepth) {
        std::vector<float> fgVals;
        fgVals.reserve(fgCount);
        for (int i = 0; i < n; ++i) {
            if (out.fg[i]) fgVals.push_back((*aiDepthRaw)[i]);
        }
        if (!fgVals.empty()) {
            std::sort(fgVals.begin(), fgVals.end());
            float dLo = fgVals[fgVals.size() * 2 / 100];
            float dHi = fgVals[fgVals.size() * 98 / 100];
            float span = std::max(1e-4f, dHi - dLo);
            for (int i = 0; i < n; ++i) {
                out.aiNorm[i] = std::clamp(((*aiDepthRaw)[i] - dLo) / span, 0.0f, 1.0f);
            }
        }
    }

    // Foreground-only box blur helper
    auto fgBoxBlur = [&](const std::vector<float>& src, int radius) {
        std::vector<float> horiz(n, 0.0f), hcnt(n, 0.0f), res(n, 0.0f);
        for (int y = 0; y < h; ++y) {
            float sum = 0.0f, cnt = 0.0f;
            for (int x = 0; x <= std::min(w - 1, radius); ++x) {
                int i = y * w + x;
                if (out.fg[i]) { sum += src[i]; cnt += 1.0f; }
            }
            for (int x = 0; x < w; ++x) {
                int i = y * w + x;
                if (cnt > 0.0f) { horiz[i] = sum / cnt; hcnt[i] = 1.0f; }
                int addX = x + radius + 1, remX = x - radius;
                if (addX < w && out.fg[y * w + addX]) { sum += src[y * w + addX]; cnt += 1.0f; }
                if (remX >= 0 && out.fg[y * w + remX]) { sum -= src[y * w + remX]; cnt -= 1.0f; }
            }
        }
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f, cnt = 0.0f;
            for (int y = 0; y <= std::min(h - 1, radius); ++y) {
                int i = y * w + x;
                if (hcnt[i] > 0.0f) { sum += horiz[i]; cnt += 1.0f; }
            }
            for (int y = 0; y < h; ++y) {
                int i = y * w + x;
                if (out.fg[i]) res[i] = (cnt > 0.0f) ? (sum / cnt) : src[i];
                int addY = y + radius + 1, remY = y - radius;
                if (addY < h && hcnt[addY * w + x] > 0.0f) { sum += horiz[addY * w + x]; cnt += 1.0f; }
                if (remY >= 0 && hcnt[remY * w + x] > 0.0f) { sum -= horiz[remY * w + x]; cnt -= 1.0f; }
            }
        }
        return res;
    };

    std::vector<float> blurSmall = fgBoxBlur(out.lum, 2);
    std::vector<float> blurWide = fgBoxBlur(out.lum, 14);

    // FULL SCENE / ENVIRONMENT MODE (uses AI Depth directly for real perspective relief)
    if (!out.hasIsolatedSubject) {
        for (int i = 0; i < n; ++i) {
            float baseZ = out.hasAiDepth ? ((out.aiNorm[i] - 0.35f) * 0.72f) : ((out.lum[i] - 0.5f) * 0.22f);
            float micro = (out.lum[i] - blurSmall[i]) * 0.025f;
            out.depth[i] = baseZ + micro;
        }
        // Softly limit extreme single-pixel Z cliffs so foreground objects pop out cleanly without stretching or tearing
        float sceneMaxStep = 1.65f * worldScale;
        for (int iter = 0; iter < 5; ++iter) {
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    int i = y * w + x;
                    float nbAvg = 0.25f * (out.depth[i - 1] + out.depth[i + 1] + out.depth[i - w] + out.depth[i + w]);
                    if (out.depth[i] > nbAvg + sceneMaxStep) out.depth[i] = nbAvg + sceneMaxStep;
                    else if (out.depth[i] < nbAvg - sceneMaxStep) out.depth[i] = nbAvg - sceneMaxStep;
                }
            }
        }
        return out;
    }

    // ISOLATED SUBJECT: Two-pass Chamfer Distance Transform
    std::vector<float> dist(n, 0.0f);
    const float INF = 1e6f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            dist[i] = out.fg[i] ? INF : 0.0f;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            if (dist[i] == 0.0f) continue;
            float d = dist[i];
            if (x > 0) d = std::min(d, dist[i - 1] + 1.0f);
            if (y > 0) d = std::min(d, dist[i - w] + 1.0f);
            if (x > 0 && y > 0) d = std::min(d, dist[i - w - 1] + 1.4142f);
            if (x + 1 < w && y > 0) d = std::min(d, dist[i - w + 1] + 1.4142f);
            dist[i] = d;
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            int i = y * w + x;
            if (dist[i] == 0.0f) continue;
            float d = dist[i];
            if (x + 1 < w) d = std::min(d, dist[i + 1] + 1.0f);
            if (y + 1 < h) d = std::min(d, dist[i + w] + 1.0f);
            if (x + 1 < w && y + 1 < h) d = std::min(d, dist[i + w + 1] + 1.4142f);
            if (x > 0 && y + 1 < h) d = std::min(d, dist[i + w - 1] + 1.4142f);
            dist[i] = d;
        }
    }

    float maxDist = 1.0f;
    for (int i = 0; i < n; ++i) {
        if (out.fg[i] && dist[i] < INF * 0.5f) maxDist = std::max(maxDist, dist[i]);
    }

    // Local ridge radius R(x,y)
    std::vector<float> ridgeR = dist;
    int ridgeRad = std::max(4, std::min(w, h) / 48);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> tmp = ridgeR;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int i = y * w + x;
                if (!out.fg[i]) continue;
                float best = ridgeR[i];
                for (int kx = std::max(0, x - ridgeRad); kx <= std::min(w - 1, x + ridgeRad); ++kx)
                    best = std::max(best, ridgeR[y * w + kx]);
                tmp[i] = best;
            }
        }
        ridgeR = tmp;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int i = y * w + x;
                if (!out.fg[i]) continue;
                float best = ridgeR[i];
                for (int ky = std::max(0, y - ridgeRad); ky <= std::min(h - 1, y + ridgeRad); ++ky)
                    best = std::max(best, ridgeR[ky * w + x]);
                tmp[i] = best;
            }
        }
        ridgeR.swap(tmp);
    }

    float subjHalfWidthWorld = std::max(0.15f, (out.maxX - out.minX) * 0.5f * worldScale);
    float targetObjDepth = std::clamp(subjHalfWidthWorld * 0.42f, 0.12f, 0.28f);
    std::vector<float> aiSmooth = out.hasAiDepth ? fgBoxBlur(out.aiNorm, 6) : std::vector<float>(n, 0.5f);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            if (!out.fg[i]) { out.depth[i] = 0.0f; continue; }
            bool isRim = (x == 0 || y == 0 || x == w - 1 || y == h - 1 ||
                          !out.fg[i - 1] || !out.fg[i + 1] || !out.fg[i - w] || !out.fg[i + w]);
            if (isRim) { out.depth[i] = 0.0f; continue; }

            float d = dist[i];
            if (out.isBotanicalTree) {
                if (out.foliage[i]) {
                    // Keep foliage crisp & thin (<= 0.005) so ZERO fat horizontal stripes appear!
                    float leafRamp = std::min(1.0f, d / 3.0f);
                    float leafMicro = (out.lum[i] - blurSmall[i]) * 0.003f;
                    out.depth[i] = std::max(0.0008f, 0.0045f * leafRamp + leafMicro);

                    // Real 3D canopy Z distribution using AI depth + organic branch waves
                    float relY = static_cast<float>(out.maxY - y) / static_cast<float>(subjH);
                    float offX = (static_cast<float>(x) - out.trunkCenterX) * worldScale;
                    float aiShift = out.hasAiDepth ? ((aiSmooth[i] - 0.5f) * 0.26f) : 0.0f;
                    float organicWave = std::sin(offX * 4.5f + relY * 5.5f) * 0.065f + (blurWide[i] - 0.45f) * 0.08f;
                    out.zCenter[i] = aiShift + organicWave;
                } else {
                    // Woody branches/trunk: true 1:1 round cylindrical cross-section
                    float R = std::max(d, std::min(ridgeR[i], 12.0f));
                    float cylPx = std::sqrt(std::max(0.0f, R * R - (R - d) * (R - d)));
                    float branchThickness = std::min(0.032f, cylPx * worldScale * 1.0f);
                    float barkMicro = (out.lum[i] - blurSmall[i]) * 0.003f;
                    out.depth[i] = std::max(0.0008f, branchThickness + barkMicro);

                    float relY = static_cast<float>(out.maxY - y) / static_cast<float>(subjH);
                    float branchWeight = std::clamp((relY - 0.30f) / 0.45f, 0.0f, 1.0f);
                    float aiShift = out.hasAiDepth ? ((aiSmooth[i] - 0.5f) * 0.14f) : 0.0f;
                    out.zCenter[i] = branchWeight * aiShift;
                }
            } else {
                // General object / product / statue / person / car:
                // Combine AI Semantic Depth + Smooth Ellipsoidal Volume + Micro Surface Relief
                float R = std::max(d, ridgeR[i]);
                float cylNorm = std::sqrt(std::max(0.0f, R * R - (R - d) * (R - d))) / std::max(1.0f, maxDist);
                float rimTaper = std::min(1.0f, std::sqrt(d / std::max(3.0f, maxDist * 0.18f)));
                float aiPart = out.hasAiDepth ? (0.45f + 0.55f * out.aiNorm[i]) : 0.70f;
                float volPart = (0.45f + 0.55f * cylNorm);
                float baseDepth = targetObjDepth * rimTaper * (out.hasAiDepth ? (0.42f * aiPart + 0.58f * volPart) : volPart);
                float relief = ((blurSmall[i] - blurWide[i]) * 0.25f + (out.lum[i] - blurSmall[i]) * 0.18f) * targetObjDepth;
                out.depth[i] = std::max(0.0015f, baseDepth + rimTaper * relief);
            }
        }
    }

    // CRITICAL FIX: Iterative Slope-Limiter (|dz/dxy| <= 1.15 * worldScale) eliminates ALL barcode texture-smear stripes!
    float maxStep = (out.isBotanicalTree ? 0.95f : 1.15f) * worldScale;
    for (int iter = 0; iter < 6; ++iter) {
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int i = y * w + x;
                if (out.depth[i] <= 0.0f) continue;
                float nbMin = std::min({out.depth[i - 1], out.depth[i + 1], out.depth[i - w], out.depth[i + w]});
                if (out.depth[i] > nbMin + maxStep) out.depth[i] = nbMin + maxStep;
            }
        }
        for (int y = h - 2; y >= 1; --y) {
            for (int x = w - 2; x >= 1; --x) {
                int i = y * w + x;
                if (out.depth[i] <= 0.0f) continue;
                float nbMin = std::min({out.depth[i - 1], out.depth[i + 1], out.depth[i - w], out.depth[i + w]});
                if (out.depth[i] > nbMin + maxStep) out.depth[i] = nbMin + maxStep;
            }
        }
    }

    // 4-pass Laplacian smoothing on interior depth after slope-limiting for silky-smooth organic 3D curvature (no pyramid facets!)
    int smoothPasses = out.isBotanicalTree ? 2 : 5;
    for (int sp = 0; sp < smoothPasses; ++sp) {
        std::vector<float> smoothD = out.depth;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int i = y * w + x;
                if (out.depth[i] <= 0.0f) continue;
                smoothD[i] = (out.depth[i] * 4.0f + out.depth[i - 1] + out.depth[i + 1] + out.depth[i - w] + out.depth[i + w]) / 8.0f;
            }
        }
        out.depth.swap(smoothD);
    }

    if (out.isBotanicalTree) {
        float maxZCenterStep = 0.70f * worldScale;
        for (int iter = 0; iter < 6; ++iter) {
            std::vector<float> nextZc = out.zCenter;
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    int i = y * w + x;
                    if (!out.fg[i]) continue;
                    float sum = 0.0f;
                    int cnt = 0;
                    const int nbs[4] = {i - 1, i + 1, i - w, i + w};
                    for (int nb : nbs) {
                        if (out.fg[nb]) { sum += out.zCenter[nb]; cnt++; }
                    }
                    if (cnt > 0) {
                        float avg = sum / static_cast<float>(cnt);
                        float diff = out.zCenter[i] - avg;
                        if (diff > maxZCenterStep) nextZc[i] = avg + maxZCenterStep;
                        else if (diff < -maxZCenterStep) nextZc[i] = avg - maxZCenterStep;
                        else nextZc[i] = 0.5f * out.zCenter[i] + 0.5f * avg;
                    }
                }
            }
            out.zCenter.swap(nextZc);
        }
    }
    return out;
}

bool writeDilatedRGBATexture(
    const ToolConfig& cfg,
    const RGBImage& img,
    const SubjectAnalysis& subj,
    const fs::path& tmpDir,
    const fs::path& dstPng
) {
    int w = img.width, h = img.height, n = w * h;
    std::vector<uint8_t> rgbOut = img.rgb;
    std::vector<uint8_t> filled = subj.fg;
    const int dx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
    const int dy[8] = {0, 0, 1, -1, 1, 1, -1, -1};

    for (int pass = 0; pass < 10; ++pass) {
        std::vector<uint8_t> nextRgb = rgbOut;
        std::vector<uint8_t> nextFilled = filled;
        bool changed = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int i = y * w + x;
                if (filled[i]) continue;
                int sumR = 0, sumG = 0, sumB = 0, cnt = 0;
                for (int d = 0; d < 8; ++d) {
                    int nx = x + dx[d], ny = y + dy[d];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    int ni = ny * w + nx;
                    if (!filled[ni]) continue;
                    sumR += rgbOut[ni * 3 + 0];
                    sumG += rgbOut[ni * 3 + 1];
                    sumB += rgbOut[ni * 3 + 2];
                    cnt++;
                }
                if (cnt > 0) {
                    nextRgb[i * 3 + 0] = static_cast<uint8_t>(sumR / cnt);
                    nextRgb[i * 3 + 1] = static_cast<uint8_t>(sumG / cnt);
                    nextRgb[i * 3 + 2] = static_cast<uint8_t>(sumB / cnt);
                    nextFilled[i] = 1;
                    changed = true;
                }
            }
        }
        rgbOut.swap(nextRgb);
        filled.swap(nextFilled);
        if (!changed) break;
    }

    fs::path pamPath = tmpDir / (dstPng.stem().string() + ".pam");
    {
        std::ofstream pam(pamPath, std::ios::binary | std::ios::trunc);
        if (!pam.is_open()) return false;
        pam << "P7\nWIDTH " << w << "\nHEIGHT " << h << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        std::vector<uint8_t> rgba(static_cast<size_t>(n) * 4);
        for (int i = 0; i < n; ++i) {
            rgba[i * 4 + 0] = rgbOut[i * 3 + 0];
            rgba[i * 4 + 1] = rgbOut[i * 3 + 1];
            rgba[i * 4 + 2] = rgbOut[i * 3 + 2];
            rgba[i * 4 + 3] = subj.alpha[i];
        }
        pam.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    }

    std::string cmd = "\"" + cfg.ffmpeg + "\" -v error -y -i \"" + pamPath.string() + "\" \"" + dstPng.string() + "\"";
    return runCmd(cmd) == 0 && fs::exists(dstPng);
}

bool reconstructDeepScan(
    const ToolConfig& cfg,
    const fs::path& jobDir,
    const fs::path& frontImgPath,
    const fs::path& backImgPath,
    const std::string& preset,
    const std::string& sceneMode
) {
    int gridMaxDim = 340;
    int texMaxDim = 1024;
    if (preset == "lowpoly") { gridMaxDim = 220; texMaxDim = 720; }
    else if (preset == "balanced") { gridMaxDim = 340; texMaxDim = 1024; }
    else if (preset == "highpoly") { gridMaxDim = 440; texMaxDim = 1440; }
    else if (preset == "ultra") { gridMaxDim = 540; texMaxDim = 1800; }

    fs::path outDir = jobDir / "output";
    fs::path tmpDir = jobDir / "deep_tmp";
    fs::create_directories(outDir);
    fs::create_directories(tmpDir);

    bool hasSecond = !backImgPath.empty() && fs::exists(backImgPath);
    logProgress(26, "Deep-Scan: Decoding & running Neural Depth + Silhouette Analysis...");

    fs::path frontPpm = tmpDir / "front_work.ppm";
    if (!decodeImageToPPM(cfg, frontImgPath, frontPpm, gridMaxDim)) {
        std::cerr << "[ERROR] Failed to decode front image.\n";
        return false;
    }
    RGBImage frontImg;
    if (!loadPPM(frontPpm, frontImg)) {
        std::cerr << "[ERROR] Failed to load front working PPM.\n";
        return false;
    }

    int w = frontImg.width, h = frontImg.height;
    float maxSide = static_cast<float>(std::max(w, h));
    float worldScale = 2.4f / maxSide;

    std::vector<uint8_t> frontSegRaw;
    std::vector<float> frontAiRaw;
    bool hasFrontAi = tryAiSemanticAndDepth(cfg, frontImgPath, tmpDir / "front_seg.pgm", tmpDir / "front_depth.pgm", w, h, frontSegRaw, frontAiRaw);
    if (hasFrontAi) {
        logProgress(36, "Neural Semantic + AI Depth Map computed (" + std::to_string(w) + "x" + std::to_string(h) + ").");
    }

    SubjectAnalysis frontA = analyzeSubject(
        frontImg, sceneMode, worldScale,
        hasFrontAi ? &frontAiRaw : nullptr,
        frontSegRaw.empty() ? nullptr : &frontSegRaw
    );

    fs::path frontTexPpm = tmpDir / "front_tex.ppm";
    RGBImage frontTexImg;
    if (texMaxDim > gridMaxDim && decodeImageToPPM(cfg, frontImgPath, frontTexPpm, texMaxDim) && loadPPM(frontTexPpm, frontTexImg)) {
        // ok
    } else {
        frontTexImg = frontImg;
    }
    SubjectAnalysis frontTexA = (frontTexImg.width == w && frontTexImg.height == h)
        ? frontA
        : analyzeSubject(frontTexImg, sceneMode, 2.4f / static_cast<float>(std::max(frontTexImg.width, frontTexImg.height)), nullptr);
    if (!writeDilatedRGBATexture(cfg, frontTexImg, frontTexA, tmpDir, outDir / "texture_front.png")) {
        std::cerr << "[ERROR] Failed to generate texture_front.png.\n";
        return false;
    }

    RGBImage secondImg;
    SubjectAnalysis secondA;
    bool useSecond = false;
    bool secondMatchesFront = false;

    if (hasSecond) {
        logProgress(50, "Analyzing 2nd photo with Neural Depth & Silhouette Alignment...");
        fs::path secondPpm = tmpDir / "second_work.ppm";
        if (decodeImageToPPM(cfg, backImgPath, secondPpm, gridMaxDim) && loadPPM(secondPpm, secondImg)) {
            int sw = secondImg.width, sh = secondImg.height;
            float secWorldScale = 2.4f / static_cast<float>(std::max(sw, sh));
            std::vector<float> secAiRaw;
            bool hasSecAi = tryEstimateAiDepth(cfg, backImgPath, tmpDir / "second_depth.pgm", sw, sh, secAiRaw);
            secondA = analyzeSubject(secondImg, sceneMode, secWorldScale, hasSecAi ? &secAiRaw : nullptr);

            fs::path secondTexPpm = tmpDir / "second_tex.ppm";
            RGBImage secondTexImg;
            if (texMaxDim > gridMaxDim && decodeImageToPPM(cfg, backImgPath, secondTexPpm, texMaxDim) && loadPPM(secondTexPpm, secondTexImg)) {
                // ok
            } else {
                secondTexImg = secondImg;
            }
            SubjectAnalysis secondTexA = (secondTexImg.width == sw && secondTexImg.height == sh)
                ? secondA
                : analyzeSubject(secondTexImg, sceneMode, 2.4f / static_cast<float>(std::max(secondTexImg.width, secondTexImg.height)), nullptr);

            if (writeDilatedRGBATexture(cfg, secondTexImg, secondTexA, tmpDir, outDir / "texture_back.png")) {
                useSecond = true;
                int fSpanX = std::max(1, frontA.maxX - frontA.minX);
                int fSpanY = std::max(1, frontA.maxY - frontA.minY);
                int sSpanX = std::max(1, secondA.maxX - secondA.minX);
                int sSpanY = std::max(1, secondA.maxY - secondA.minY);
                int inter = 0, uni = 0;
                const int S = 64;
                for (int sy = 0; sy < S; ++sy) {
                    for (int sx = 0; sx < S; ++sx) {
                        int fx = std::clamp(frontA.minX + (sx * fSpanX) / S, 0, frontA.width - 1);
                        int fy = std::clamp(frontA.minY + (sy * fSpanY) / S, 0, frontA.height - 1);
                        int bx = std::clamp(secondA.maxX - (sx * sSpanX) / S, 0, secondA.width - 1);
                        int by = std::clamp(secondA.minY + (sy * sSpanY) / S, 0, secondA.height - 1);
                        bool ff = frontA.fg[fy * frontA.width + fx] != 0;
                        bool bf = secondA.fg[by * secondA.width + bx] != 0;
                        if (ff && bf) inter++;
                        if (ff || bf) uni++;
                    }
                }
                float iou = uni > 0 ? static_cast<float>(inter) / static_cast<float>(uni) : 0.0f;
                secondMatchesFront = (iou >= 0.48f);
            }
        }
    }

    struct Vec3 { float x, y, z; };
    struct Vec2 { float u, v; };
    struct Tri { int a, b, c; int mat; };

    std::vector<Vec3> verts;
    std::vector<Vec2> uvs;
    std::vector<Tri> tris;

    // Helper: Full-scene perspective surface reconstruction (with depth-jump tear guard)
    if (!frontA.hasIsolatedSubject) {
        logProgress(72, "Synthesizing 3D Scene Geometry from Neural AI Depth...");
        std::vector<int> idxGrid(w * h, -1);
        float cx = w * 0.5f, cy = h * 0.5f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int i = y * w + x;
                float z = frontA.depth[i];
                float perspScale = 1.0f + std::max(0.0f, -z * 0.35f);
                float wx = (static_cast<float>(x) - cx) * worldScale * perspScale;
                float wy = (cy - static_cast<float>(y)) * worldScale * perspScale;
                float u = static_cast<float>(x) / static_cast<float>(std::max(1, w - 1));
                float v = 1.0f - (static_cast<float>(y) / static_cast<float>(std::max(1, h - 1)));
                idxGrid[i] = static_cast<int>(verts.size());
                verts.push_back({wx, wy, z});
                uvs.push_back({u, v});
            }
        }
        for (int y = 0; y < h - 1; ++y) {
            for (int x = 0; x < w - 1; ++x) {
                int v00 = idxGrid[y * w + x], v10 = idxGrid[y * w + x + 1];
                int v01 = idxGrid[(y + 1) * w + x], v11 = idxGrid[(y + 1) * w + x + 1];
                tris.push_back({v00, v01, v11, 0});
                tris.push_back({v00, v11, v10, 0});
            }
        }
    } else {
        // Isolated Subject Mode (Tree OR General 3D Object)
        auto appendShell = [&](
            const SubjectAnalysis& A,
            const SubjectAnalysis* backOverride,
            float shellWorldScale,
            float rotYRad,
            float scaleXZ,
            float scaleY,
            int matFront,
            int matBack,
            bool skipLowerTrunk,
            bool orthogonalSideModulate
        ) {
            int sw = A.width, sh = A.height;
            float pivotX = A.trunkCenterX;
            float bottomY = static_cast<float>(A.maxY);
            float cosR = std::cos(rotYRad), sinR = std::sin(rotYRad);

            std::vector<int> fIdx(sw * sh, -1);
            std::vector<int> bIdx(sw * sh, -1);

            int fSpanX = std::max(1, A.maxX - A.minX);
            int fSpanY = std::max(1, A.maxY - A.minY);
            int bSpanX = backOverride ? std::max(1, backOverride->maxX - backOverride->minX) : 1;
            int bSpanY = backOverride ? std::max(1, backOverride->maxY - backOverride->minY) : 1;

            // Optional side-view horizontal width profile for 90-deg Front+Side visual hull depth scaling
            std::vector<float> sideDepthFactor(sh, 1.0f);
            if (orthogonalSideModulate && backOverride) {
                float avgSideSpan = 0.0f;
                int cntRows = 0;
                std::vector<float> rowSpan(sh, 0.0f);
                for (int y = A.minY; y <= A.maxY; ++y) {
                    float relY = static_cast<float>(y - A.minY) / static_cast<float>(fSpanY);
                    int sy = std::clamp(backOverride->minY + static_cast<int>(std::round(relY * bSpanY)), 0, backOverride->height - 1);
                    int sMin = backOverride->width, sMax = -1;
                    for (int sx = backOverride->minX; sx <= backOverride->maxX; ++sx) {
                        if (backOverride->fg[sy * backOverride->width + sx]) {
                            sMin = std::min(sMin, sx);
                            sMax = std::max(sMax, sx);
                        }
                    }
                    if (sMax >= sMin) {
                        rowSpan[y] = static_cast<float>(sMax - sMin + 1);
                        avgSideSpan += rowSpan[y];
                        cntRows++;
                    }
                }
                if (cntRows > 0) {
                    avgSideSpan /= static_cast<float>(cntRows);
                    for (int y = A.minY; y <= A.maxY; ++y) {
                        if (rowSpan[y] > 0.0f && avgSideSpan > 1.0f) {
                            sideDepthFactor[y] = std::clamp(rowSpan[y] / avgSideSpan, 0.45f, 1.85f);
                        }
                    }
                }
            }

            auto pushRotatedVert = [&](float lx, float ly, float lz, float u, float v) -> int {
                float sx = lx * scaleXZ;
                float sy = ly * scaleY;
                float sz = lz * scaleXZ;
                float rx = sx * cosR + sz * sinR;
                float rz = -sx * sinR + sz * cosR;
                int id = static_cast<int>(verts.size());
                verts.push_back({rx, sy, rz});
                uvs.push_back({u, v});
                return id;
            };

            for (int y = 0; y < sh; ++y) {
                if (skipLowerTrunk && y > A.trunkTopY + 2) continue;
                for (int x = 0; x < sw; ++x) {
                    int i = y * sw + x;
                    if (!A.fg[i]) continue;

                    float lx = (static_cast<float>(x) - pivotX) * shellWorldScale;
                    float ly = (bottomY - static_cast<float>(y)) * shellWorldScale;
                    float zc = A.zCenter[i];
                    float dMod = A.depth[i] * sideDepthFactor[y];
                    float zf = zc + dMod;
                    float zb = zc - dMod;

                    float u = static_cast<float>(x) / static_cast<float>(std::max(1, sw - 1));
                    float v = 1.0f - (static_cast<float>(y) / static_cast<float>(std::max(1, sh - 1)));

                    fIdx[i] = pushRotatedVert(lx, ly, zf, u, v);

                    float bu = u, bv = v;
                    if (backOverride && secondMatchesFront) {
                        float relX = static_cast<float>(x - A.minX) / static_cast<float>(fSpanX);
                        float relY = static_cast<float>(y - A.minY) / static_cast<float>(fSpanY);
                        int bx = std::clamp(backOverride->maxX - static_cast<int>(std::round(relX * bSpanX)), 0, backOverride->width - 1);
                        int by = std::clamp(backOverride->minY + static_cast<int>(std::round(relY * bSpanY)), 0, backOverride->height - 1);
                        int bi = by * backOverride->width + bx;
                        if (A.depth[i] <= 0.0f) {
                            zb = zc;
                        } else if (backOverride->fg[bi]) {
                            zb = zc - (0.5f * dMod + 0.5f * backOverride->depth[bi]);
                            bu = static_cast<float>(bx) / static_cast<float>(std::max(1, backOverride->width - 1));
                            bv = 1.0f - (static_cast<float>(by) / static_cast<float>(std::max(1, backOverride->height - 1)));
                        }
                    }

                    bIdx[i] = pushRotatedVert(lx, ly, zb, bu, bv);
                }
            }

            for (int y = 0; y < sh - 1; ++y) {
                for (int x = 0; x < sw - 1; ++x) {
                    int i00 = y * sw + x, i10 = y * sw + (x + 1);
                    int i01 = (y + 1) * sw + x, i11 = (y + 1) * sw + (x + 1);
                    int f00 = fIdx[i00], f10 = fIdx[i10], f01 = fIdx[i01], f11 = fIdx[i11];
                    int b00 = bIdx[i00], b10 = bIdx[i10], b01 = bIdx[i01], b11 = bIdx[i11];
                    int validCount = (f00 >= 0) + (f10 >= 0) + (f01 >= 0) + (f11 >= 0);

                    if (validCount == 4) {
                        tris.push_back({f00, f01, f11, matFront});
                        tris.push_back({f00, f11, f10, matFront});
                        tris.push_back({b00, b11, b01, matBack});
                        tris.push_back({b00, b10, b11, matBack});
                    } else if (validCount == 3) {
                        if (f00 < 0) {
                            tris.push_back({f10, f01, f11, matFront});
                            tris.push_back({b10, b11, b01, matBack});
                        } else if (f10 < 0) {
                            tris.push_back({f00, f01, f11, matFront});
                            tris.push_back({b00, b11, b01, matBack});
                        } else if (f01 < 0) {
                            tris.push_back({f00, f11, f10, matFront});
                            tris.push_back({b00, b10, b11, matBack});
                        } else if (f11 < 0) {
                            tris.push_back({f00, f01, f10, matFront});
                            tris.push_back({b00, b10, b01, matBack});
                        }
                    }
                }
            }
        };

        // Helper: True 16-segment cylindrical 3D trunk mesh following exact photo trunk curve & taper!
        auto appendCylindricalTrunk = [&](const SubjectAnalysis& A, float shellWorldScale) {
            int sw = A.width, sh = A.height;
            float pivotX = A.trunkCenterX;
            float bottomY = static_cast<float>(A.maxY);
            const int SEGS = 16;
            std::vector<std::vector<int>> rings;

            for (int y = A.maxY; y >= A.trunkTopY; y -= 2) {
                // Find widest contiguous trunk segment nearest pivotX
                int bestL = -1, bestR = -1;
                float bestDist = 1e9f;
                int x = A.minX;
                while (x <= A.maxX) {
                    if (!A.fg[y * sw + x]) { x++; continue; }
                    int l = x;
                    while (x <= A.maxX && A.fg[y * sw + x]) x++;
                    int r = x - 1;
                    float segCenter = 0.5f * (l + r);
                    float dc = std::abs(segCenter - pivotX);
                    if (dc < bestDist) { bestDist = dc; bestL = l; bestR = r; }
                }
                if (bestL < 0 || (bestR - bestL) < 2) continue;

                float segCenter = 0.5f * (bestL + bestR);
                float radiusPx = std::max(1.5f, 0.5f * (bestR - bestL));
                // Stop if canopy/fork starts spreading dramatically
                if (radiusPx > (A.maxX - A.minX) * 0.14f && y < A.maxY - (A.maxY - A.minY) * 20 / 100) break;

                float cx = (segCenter - pivotX) * shellWorldScale;
                float cy = (bottomY - static_cast<float>(y)) * shellWorldScale;
                float radW = radiusPx * shellWorldScale * 1.05f;
                float vCoord = 1.0f - (static_cast<float>(y) / static_cast<float>(std::max(1, sh - 1)));
                float uL = static_cast<float>(bestL + 1) / static_cast<float>(std::max(1, sw - 1));
                float uR = static_cast<float>(bestR - 1) / static_cast<float>(std::max(1, sw - 1));
                if (uR < uL) std::swap(uL, uR);

                std::vector<int> ring;
                ring.reserve(SEGS + 1);
                for (int s = 0; s <= SEGS; ++s) {
                    float theta = (static_cast<float>(s) / static_cast<float>(SEGS)) * 6.2831853f;
                    float vx = cx + radW * std::cos(theta);
                    float vz = radW * std::sin(theta);
                    // Wrap bark UV cleanly front & back across trunk width (zero white edge!)
                    float tU = 0.5f + 0.5f * std::cos(theta);
                    float uCoord = uL + tU * (uR - uL);
                    ring.push_back(static_cast<int>(verts.size()));
                    verts.push_back({vx, cy, vz});
                    uvs.push_back({uCoord, vCoord});
                }
                rings.push_back(std::move(ring));
            }

            for (size_t r = 0; r + 1 < rings.size(); ++r) {
                for (int s = 0; s < SEGS; ++s) {
                    int a = rings[r][s];
                    int b = rings[r][s + 1];
                    int c = rings[r + 1][s + 1];
                    int d = rings[r + 1][s];
                    tris.push_back({a, b, c, 0});
                    tris.push_back({a, c, d, 0});
                }
            }
        };

        if (frontA.isBotanicalTree) {
            logProgress(74, "Building 3D cylindrical trunk + layered 360-deg foliage tree...");
            appendCylindricalTrunk(frontA, worldScale);
            // Primary full tree (0 deg) — canopy + upper branches (trunk already built as true cylinder!)
            appendShell(frontA, nullptr, worldScale, 0.0f, 1.0f, 1.0f, 0, 0, true, false);

            if (useSecond && !secondMatchesFront && secondA.isBotanicalTree) {
                // 2 distinct tree photos: cross-rotate Photo 2 canopy cleanly at +62 deg & -62 deg (no edge-on 90-deg stripe on Front view!)
                float fHeightPx = static_cast<float>(std::max(1, frontA.maxY - frontA.minY));
                float sHeightPx = static_cast<float>(std::max(1, secondA.maxY - secondA.minY));
                float secWorldScale = 2.4f / static_cast<float>(std::max(secondA.width, secondA.height));
                float matchRatio = (fHeightPx * worldScale) / (sHeightPx * secWorldScale);
                appendShell(secondA, nullptr, secWorldScale, 1.082104f, 0.90f * matchRatio, 0.96f * matchRatio, 1, 1, true, false);
                appendShell(frontA, nullptr, worldScale, -1.047198f, 0.88f, 0.95f, 0, 0, true, false);
            } else {
                // Single-view botanical tree: cross-angled crisp canopy rings (+55 deg, -55 deg) without duplicating trunk
                appendShell(frontA, nullptr, worldScale, 0.959931f, 0.90f, 0.96f, 0, 0, true, false);
                appendShell(frontA, nullptr, worldScale, -0.959931f, 0.88f, 0.94f, 0, 0, true, false);
            }
        } else {
            logProgress(74, "Building AI-depth sculpted 3D object mesh...");
            appendShell(frontA, useSecond ? &secondA : nullptr, worldScale, 0.0f, 1.0f, 1.0f, 0,
                (useSecond && secondMatchesFront) ? 1 : 0, false, (useSecond && !secondMatchesFront));
        }
    }

    if (tris.empty()) {
        std::cerr << "[ERROR] Deep-Scan generated 0 triangles.\n";
        return false;
    }

    std::vector<Vec3> norms(verts.size(), {0.0f, 0.0f, 0.0f});
    for (const auto& t : tris) {
        const Vec3& a = verts[t.a];
        const Vec3& b = verts[t.b];
        const Vec3& c = verts[t.c];
        float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        norms[t.a].x += nx; norms[t.a].y += ny; norms[t.a].z += nz;
        norms[t.b].x += nx; norms[t.b].y += ny; norms[t.b].z += nz;
        norms[t.c].x += nx; norms[t.c].y += ny; norms[t.c].z += nz;
    }
    for (auto& nm : norms) {
        float len = std::sqrt(nm.x * nm.x + nm.y * nm.y + nm.z * nm.z);
        if (len > 1e-8f) { nm.x /= len; nm.y /= len; nm.z /= len; }
        else nm.z = 1.0f;
    }

    logProgress(90, "Writing textured 3D OBJ/MTL (" + std::to_string(verts.size()) +
        " vertices, " + std::to_string(tris.size()) + " triangles)...");

    fs::path mtlPath = outDir / "model.mtl";
    {
        std::ofstream mtl(mtlPath, std::ios::trunc);
        mtl << "# RathoreScan 3D Deep-Scan Material\n";
        mtl << "newmtl material_front\n";
        mtl << "Ka 0.200000 0.200000 0.200000\n";
        mtl << "Kd 1.000000 1.000000 1.000000\n";
        mtl << "Ks 0.080000 0.080000 0.080000\n";
        mtl << "Ns 20.000000\n";
        mtl << "d 1.000000\n";
        mtl << "illum 2\n";
        mtl << "map_Kd texture_front.png\n";
        if (useSecond) {
            mtl << "\nnewmtl material_back\n";
            mtl << "Ka 0.200000 0.200000 0.200000\n";
            mtl << "Kd 1.000000 1.000000 1.000000\n";
            mtl << "Ks 0.080000 0.080000 0.080000\n";
            mtl << "Ns 20.000000\n";
            mtl << "d 1.000000\n";
            mtl << "illum 2\n";
            mtl << "map_Kd texture_back.png\n";
        }
    }

    fs::path objPath = outDir / "model.obj";
    {
        std::ofstream obj(objPath, std::ios::trunc);
        obj << "# RathoreScan 3D Deep-Scan Engine\n";
        obj << "mtllib model.mtl\n";
        obj << std::fixed << std::setprecision(6);
        for (const auto& v : verts) obj << "v " << v.x << " " << v.y << " " << v.z << "\n";
        for (const auto& vt : uvs) obj << "vt " << vt.u << " " << vt.v << "\n";
        for (const auto& vn : norms) obj << "vn " << vn.x << " " << vn.y << " " << vn.z << "\n";

        int currentMat = -1;
        for (const auto& t : tris) {
            if (t.mat != currentMat) {
                currentMat = t.mat;
                obj << "usemtl " << (currentMat == 1 ? "material_back" : "material_front") << "\n";
            }
            int a = t.a + 1, b = t.b + 1, c = t.c + 1;
            obj << "f " << a << "/" << a << "/" << a << " "
                << b << "/" << b << "/" << b << " "
                << c << "/" << c << "/" << c << "\n";
        }
    }

    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    AiSceneInfo deepAi;
    deepAi.archetype = frontA.isBotanicalTree ? "botanical" : (frontA.hasIsolatedSubject ? "object" : "scene");
    deepAi.what = frontA.isBotanicalTree ? "Botanical Tree / Plant (AI Semantic + Depth)" : "3D Subject / Scene (AI Semantic + Depth)";
    deepAi.where = frontA.hasIsolatedSubject ? "Subject isolated from sky/backdrop" : "Full-frame 3D relief";
    writeSceneReportJson(
        outDir / "scene_report.json", deepAi,
        hasSecond ? "1-2 view AI Semantic + Depth reconstruction (upload 20-100+ 360 views for full multi-view mesh)" : "Single-view AI Semantic + Neural Depth reconstruction",
        "SegFormer Semantic Map + Depth-Anything-V2-Small + Volumetric 3D Mesh",
        hasSecond ? 90.0 : 0.0, hasSecond ? 2 : 1, hasSecond ? 2 : 1
    );
    logProgress(100, "Deep-Scan 3D Model Ready! Loading in Studio...");
    return true;
}

// =============================================================================
// Statistical Outlier Removal (SOR) on dense PLY before ReconstructMesh
// =============================================================================

enum class PlyScalarType { F32, F64, I8, U8, I16, U16, I32, U32 };

struct PlyPropDef {
    PlyScalarType stype;
    std::string name;
    int size = 4;
};

static bool parsePlyScalarType(const std::string& t, PlyScalarType& out, int& sz) {
    if (t == "float" || t == "float32") { out = PlyScalarType::F32; sz = 4; return true; }
    if (t == "double" || t == "float64") { out = PlyScalarType::F64; sz = 8; return true; }
    if (t == "char" || t == "int8") { out = PlyScalarType::I8; sz = 1; return true; }
    if (t == "uchar" || t == "uint8") { out = PlyScalarType::U8; sz = 1; return true; }
    if (t == "short" || t == "int16") { out = PlyScalarType::I16; sz = 2; return true; }
    if (t == "ushort" || t == "uint16") { out = PlyScalarType::U16; sz = 2; return true; }
    if (t == "int" || t == "int32") { out = PlyScalarType::I32; sz = 4; return true; }
    if (t == "uint" || t == "uint32") { out = PlyScalarType::U32; sz = 4; return true; }
    return false;
}

static double readPlyScalar(const uint8_t* p, PlyScalarType t) {
    switch (t) {
        case PlyScalarType::F32: { float v; std::memcpy(&v, p, 4); return static_cast<double>(v); }
        case PlyScalarType::F64: { double v; std::memcpy(&v, p, 8); return v; }
        case PlyScalarType::I8:  { int8_t v; std::memcpy(&v, p, 1); return static_cast<double>(v); }
        case PlyScalarType::U8:  { uint8_t v; std::memcpy(&v, p, 1); return static_cast<double>(v); }
        case PlyScalarType::I16: { int16_t v; std::memcpy(&v, p, 2); return static_cast<double>(v); }
        case PlyScalarType::U16: { uint16_t v; std::memcpy(&v, p, 2); return static_cast<double>(v); }
        case PlyScalarType::I32: { int32_t v; std::memcpy(&v, p, 4); return static_cast<double>(v); }
        case PlyScalarType::U32: { uint32_t v; std::memcpy(&v, p, 4); return static_cast<double>(v); }
    }
    return 0.0;
}

struct CellCoord3 {
    int64_t x, y, z;
    bool operator==(const CellCoord3& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellCoordHash {
    size_t operator()(const CellCoord3& c) const noexcept {
        uint64_t h = static_cast<uint64_t>(c.x) * 73856093ULL;
        h ^= static_cast<uint64_t>(c.y) * 19349663ULL;
        h ^= static_cast<uint64_t>(c.z) * 83492791ULL;
        return static_cast<size_t>(h ^ (h >> 33));
    }
};

static bool statisticalOutlierRemovalPly(
    const fs::path& srcPath,
    const fs::path& dstPath,
    int k,
    double stdRatio,
    size_t& totalOut,
    size_t& keptOut
) {
    totalOut = 0;
    keptOut = 0;
    std::ifstream in(srcPath, std::ios::binary);
    if (!in.is_open()) return false;

    bool isBinary = false;
    bool isLittleEndian = true;
    bool hasFaces = false;
    bool hasListProp = false;
    bool inVertexElem = false;
    size_t vertexCount = 0;
    struct PlyFieldDef {
        bool isList = false;
        PlyScalarType stype = PlyScalarType::F32;
        int scalarSz = 4;
        PlyScalarType countType = PlyScalarType::U8;
        int countSz = 1;
        int elemSz = 4;
        std::string name;
    };
    std::vector<PlyFieldDef> fields;
    int xCol = -1, yCol = -1, zCol = -1;
    std::vector<std::string> headerLines;
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        headerLines.push_back(line);
        std::istringstream iss(line);
        std::string tok;
        iss >> tok;
        if (tok == "format") {
            std::string fmt;
            iss >> fmt;
            if (fmt == "binary_little_endian") { isBinary = true; isLittleEndian = true; }
            else if (fmt == "binary_big_endian") { isBinary = true; isLittleEndian = false; }
            else if (fmt == "ascii") { isBinary = false; }
            else return false;
        } else if (tok == "element") {
            std::string ename;
            size_t cnt = 0;
            iss >> ename >> cnt;
            if (ename == "vertex") { vertexCount = cnt; inVertexElem = true; }
            else {
                inVertexElem = false;
                if (ename == "face" && cnt > 0) hasFaces = true;
            }
        } else if (tok == "property" && inVertexElem) {
            std::string ptype;
            iss >> ptype;
            if (ptype == "list") {
                std::string ctypeStr, etypeStr, pname;
                iss >> ctypeStr >> etypeStr >> pname;
                PlyScalarType ct, et;
                int csz = 0, esz = 0;
                if (!parsePlyScalarType(ctypeStr, ct, csz) || !parsePlyScalarType(etypeStr, et, esz)) return false;
                hasListProp = true;
                PlyFieldDef fd;
                fd.isList = true; fd.countType = ct; fd.countSz = csz; fd.elemSz = esz; fd.name = pname;
                fields.push_back(fd);
            } else {
                std::string pname;
                iss >> pname;
                PlyScalarType st;
                int sz = 0;
                if (!parsePlyScalarType(ptype, st, sz)) return false;
                int col = static_cast<int>(fields.size());
                if (pname == "x") xCol = col;
                else if (pname == "y") yCol = col;
                else if (pname == "z") zCol = col;
                PlyFieldDef fd;
                fd.isList = false; fd.stype = st; fd.scalarSz = sz; fd.name = pname;
                fields.push_back(fd);
            }
        } else if (tok == "end_header") {
            break;
        }
    }

    if ((isBinary && !isLittleEndian) || hasFaces || xCol < 0 || yCol < 0 || zCol < 0 || vertexCount == 0) {
        return false;
    }

    totalOut = vertexCount;
    if (vertexCount < static_cast<size_t>(k * 8)) {
        keptOut = vertexCount;
        return false;
    }

    std::vector<uint8_t> binBuf;
    std::vector<size_t> binRecStart, binRecLen;
    std::vector<std::string> asciiLines;
    std::vector<double> xs(vertexCount), ys(vertexCount), zs(vertexCount);

    if (isBinary) {
        std::streampos payloadStart = in.tellg();
        in.seekg(0, std::ios::end);
        std::streampos fileEnd = in.tellg();
        in.seekg(payloadStart);
        if (payloadStart < 0 || fileEnd <= payloadStart) return false;
        size_t payloadSize = static_cast<size_t>(fileEnd - payloadStart);
        binBuf.resize(payloadSize);
        in.read(reinterpret_cast<char*>(binBuf.data()), static_cast<std::streamsize>(payloadSize));
        if (!in) return false;

        binRecStart.resize(vertexCount);
        binRecLen.resize(vertexCount);
        size_t cursor = 0;
        for (size_t i = 0; i < vertexCount; ++i) {
            binRecStart[i] = cursor;
            for (int col = 0; col < static_cast<int>(fields.size()); ++col) {
                const auto& fd = fields[col];
                if (!fd.isList) {
                    if (cursor + static_cast<size_t>(fd.scalarSz) > payloadSize) return false;
                    if (col == xCol) xs[i] = readPlyScalar(binBuf.data() + cursor, fd.stype);
                    else if (col == yCol) ys[i] = readPlyScalar(binBuf.data() + cursor, fd.stype);
                    else if (col == zCol) zs[i] = readPlyScalar(binBuf.data() + cursor, fd.stype);
                    cursor += static_cast<size_t>(fd.scalarSz);
                } else {
                    if (cursor + static_cast<size_t>(fd.countSz) > payloadSize) return false;
                    size_t cnt = static_cast<size_t>(std::max<int64_t>(0, std::llround(readPlyScalar(binBuf.data() + cursor, fd.countType))));
                    cursor += static_cast<size_t>(fd.countSz);
                    size_t bytes = cnt * static_cast<size_t>(fd.elemSz);
                    if (cursor + bytes > payloadSize) return false;
                    cursor += bytes;
                }
            }
            binRecLen[i] = cursor - binRecStart[i];
        }
        (void)hasListProp;
    } else {
        if (hasListProp) return false;
        asciiLines.reserve(vertexCount);
        int maxCol = std::max({ xCol, yCol, zCol });
        for (size_t i = 0; i < vertexCount; ++i) {
            if (!std::getline(in, line)) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            asciiLines.push_back(line);
            std::istringstream iss(line);
            double v = 0.0;
            for (int c = 0; c <= maxCol && (iss >> v); ++c) {
                if (c == xCol) xs[i] = v;
                else if (c == yCol) ys[i] = v;
                else if (c == zCol) zs[i] = v;
            }
        }
    }
    in.close();

    double minX = 1e30, minY = 1e30, minZ = 1e30, maxX = -1e30, maxY = -1e30, maxZ = -1e30;
    size_t validFin = 0;
    for (size_t i = 0; i < vertexCount; ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i]) || !std::isfinite(zs[i])) continue;
        minX = std::min(minX, xs[i]); maxX = std::max(maxX, xs[i]);
        minY = std::min(minY, ys[i]); maxY = std::max(maxY, ys[i]);
        minZ = std::min(minZ, zs[i]); maxZ = std::max(maxZ, zs[i]);
        validFin++;
    }
    if (validFin < static_cast<size_t>(k * 8)) return false;

    double diag = std::sqrt((maxX - minX) * (maxX - minX) + (maxY - minY) * (maxY - minY) + (maxZ - minZ) * (maxZ - minZ));
    if (diag <= 1e-12) return false;
    double cellSize = std::max(diag / std::cbrt(static_cast<double>(vertexCount)) * 2.5, 1e-6);

    std::unordered_map<CellCoord3, std::vector<size_t>, CellCoordHash> grid;
    grid.reserve(std::min<size_t>(vertexCount, 500000));
    auto cellOf = [&](double x, double y, double z) -> CellCoord3 {
        return {
            static_cast<int64_t>(std::floor((x - minX) / cellSize)),
            static_cast<int64_t>(std::floor((y - minY) / cellSize)),
            static_cast<int64_t>(std::floor((z - minZ) / cellSize))
        };
    };
    for (size_t i = 0; i < vertexCount; ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i]) || !std::isfinite(zs[i])) continue;
        grid[cellOf(xs[i], ys[i], zs[i])].push_back(i);
    }

    std::vector<double> meanDist(vertexCount, 1e30);
    std::vector<double> dSq;
    dSq.reserve(64);
    double sumMean = 0.0;
    size_t countMean = 0;

    for (size_t i = 0; i < vertexCount; ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i]) || !std::isfinite(zs[i])) continue;
        CellCoord3 c = cellOf(xs[i], ys[i], zs[i]);
        for (int radius = 1; radius <= 3; ++radius) {
            dSq.clear();
            for (int64_t dx = -radius; dx <= radius; ++dx) {
                for (int64_t dy = -radius; dy <= radius; ++dy) {
                    for (int64_t dz = -radius; dz <= radius; ++dz) {
                        auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
                        if (it == grid.end()) continue;
                        for (size_t j : it->second) {
                            if (j == i) continue;
                            double ddx = xs[j] - xs[i], ddy = ys[j] - ys[i], ddz = zs[j] - zs[i];
                            dSq.push_back(ddx * ddx + ddy * ddy + ddz * ddz);
                            if (dSq.size() >= 192) break;
                        }
                        if (dSq.size() >= 192) break;
                    }
                    if (dSq.size() >= 192) break;
                }
                if (dSq.size() >= 192) break;
            }
            if (static_cast<int>(dSq.size()) >= k) break;
        }
        if (dSq.empty()) continue;
        size_t take = std::min<size_t>(dSq.size(), static_cast<size_t>(k));
        std::nth_element(dSq.begin(), dSq.begin() + static_cast<std::ptrdiff_t>(take - 1), dSq.end());
        double s = 0.0;
        for (size_t t = 0; t < take; ++t) s += std::sqrt(dSq[t]);
        meanDist[i] = s / static_cast<double>(take);
        sumMean += meanDist[i];
        countMean++;
    }

    if (countMean < 100) return false;
    double mu = sumMean / static_cast<double>(countMean);
    double var = 0.0;
    for (size_t i = 0; i < vertexCount; ++i) {
        if (meanDist[i] < 1e29) {
            double diff = meanDist[i] - mu;
            var += diff * diff;
        }
    }
    double sigma = std::sqrt(var / static_cast<double>(countMean));
    double thresh = mu + stdRatio * sigma;

    std::vector<uint8_t> keep(vertexCount, 0);
    size_t kept = 0;
    for (size_t i = 0; i < vertexCount; ++i) {
        if (meanDist[i] < 1e29 && (meanDist[i] <= thresh || sigma <= 1e-12)) {
            keep[i] = 1;
            kept++;
        }
    }

    // Safety guard: never allow SOR to strip more than 80% or drop below 500 points
    if (kept < 500 || kept * 5 < vertexCount) {
        keptOut = vertexCount;
        return false;
    }
    keptOut = kept;

    std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& hl : headerLines) {
        std::istringstream iss(hl);
        std::string tok, ename;
        if ((iss >> tok >> ename) && tok == "element" && ename == "vertex") {
            out << "element vertex " << kept << "\n";
        } else {
            out << hl << "\n";
        }
    }
    if (isBinary) {
        for (size_t i = 0; i < vertexCount; ++i) {
            if (keep[i]) out.write(reinterpret_cast<const char*>(binBuf.data() + binRecStart[i]), static_cast<std::streamsize>(binRecLen[i]));
        }
    } else {
        for (size_t i = 0; i < vertexCount; ++i) {
            if (keep[i]) out << asciiLines[i] << "\n";
        }
    }
    out.close();
    return fs::exists(dstPath) && fs::file_size(dstPath) > 256;
}

// =============================================================================
// Connected-Component Floating Shard Removal + +Y-up Export on Textured OBJ
// Drops tiny disconnected black triangles while preserving multiple materials & UVs
// =============================================================================

struct ObjFaceVert { int v = 0, vt = 0, vn = 0; };

static ObjFaceVert parseObjFaceTok(const std::string& tok, int nV, int nVt, int nVn) {
    ObjFaceVert fv;
    size_t p1 = tok.find('/');
    auto resolveIdx = [](const std::string& s, int total) -> int {
        if (s.empty()) return 0;
        int raw = std::stoi(s);
        if (raw < 0) raw = total + raw + 1;
        return (raw >= 1 && raw <= total) ? raw : 0;
    };
    if (p1 == std::string::npos) {
        fv.v = resolveIdx(tok, nV);
        return fv;
    }
    fv.v = resolveIdx(tok.substr(0, p1), nV);
    size_t p2 = tok.find('/', p1 + 1);
    if (p2 == std::string::npos) {
        fv.vt = resolveIdx(tok.substr(p1 + 1), nVt);
    } else {
        fv.vt = resolveIdx(tok.substr(p1 + 1, p2 - p1 - 1), nVt);
        fv.vn = resolveIdx(tok.substr(p2 + 1), nVn);
    }
    return fv;
}

static bool filterObjShardsAndRotateYUp(
    const fs::path& srcObj,
    const fs::path& dstObj,
    size_t& keptFacesOut,
    size_t& totalFacesOut
) {
    keptFacesOut = 0;
    totalFacesOut = 0;
    std::ifstream in(srcObj);
    if (!in.is_open()) return false;

    std::vector<std::string> mtllibLines;
    std::vector<std::array<double, 3>> verts;
    std::vector<std::string> vts;
    std::vector<std::array<double, 3>> vns;
    struct FaceRec {
        std::string mat;
        std::vector<ObjFaceVert> pts;
    };
    std::vector<FaceRec> faces;
    std::string curMat;
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string tag;
        if (!(iss >> tag)) continue;
        if (tag == "v") {
            double x = 0, y = 0, z = 0;
            if (iss >> x >> y >> z) verts.push_back({ x, y, z });
        } else if (tag == "vt") {
            std::string rest;
            std::getline(iss, rest);
            vts.push_back(rest);
        } else if (tag == "vn") {
            double x = 0, y = 0, z = 0;
            if (iss >> x >> y >> z) vns.push_back({ x, y, z });
        } else if (tag == "mtllib") {
            mtllibLines.push_back(line);
        } else if (tag == "usemtl") {
            std::getline(iss, curMat);
            size_t a = curMat.find_first_not_of(" \t");
            curMat = (a == std::string::npos) ? "" : curMat.substr(a);
        } else if (tag == "f") {
            FaceRec fr;
            fr.mat = curMat;
            std::string tok;
            while (iss >> tok) {
                ObjFaceVert fv = parseObjFaceTok(tok, static_cast<int>(verts.size()), static_cast<int>(vts.size()), static_cast<int>(vns.size()));
                if (fv.v > 0) fr.pts.push_back(fv);
            }
            if (fr.pts.size() >= 3) faces.push_back(std::move(fr));
        }
    }
    in.close();

    if (verts.empty() || faces.empty()) return false;
    totalFacesOut = faces.size();

    // Disjoint-set union across shared vertices of each face
    std::vector<int> parent(verts.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto findRoot = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        a = findRoot(a);
        b = findRoot(b);
        if (a != b) parent[b] = a;
    };

    for (const auto& f : faces) {
        int r0 = f.pts[0].v - 1;
        for (size_t i = 1; i < f.pts.size(); ++i) unite(r0, f.pts[i].v - 1);
    }

    std::unordered_map<int, size_t> compFaceCount;
    for (const auto& f : faces) compFaceCount[findRoot(f.pts[0].v - 1)]++;

    int dominantRoot = -1;
    size_t maxCompFaces = 0;
    for (const auto& kv : compFaceCount) {
        if (kv.second > maxCompFaces) { maxCompFaces = kv.second; dominantRoot = kv.first; }
    }
    if (dominantRoot < 0 || maxCompFaces == 0) return false;

    // Keep dominant component + secondary real structures (>=2% of mesh & >=150 faces); drop all small black shards
    size_t secondaryThreshold = std::max<size_t>(150, totalFacesOut / 50);
    std::vector<int> vRemap(verts.size(), 0);
    std::vector<int> vtRemap(vts.size(), 0);
    std::vector<int> vnRemap(vns.size(), 0);
    std::vector<std::array<double, 3>> outV;
    std::vector<std::string> outVt;
    std::vector<std::array<double, 3>> outVn;
    std::vector<FaceRec> keptFaces;
    keptFaces.reserve(faces.size());

    for (const auto& f : faces) {
        int r = findRoot(f.pts[0].v - 1);
        size_t sz = compFaceCount[r];
        if (r != dominantRoot && sz < secondaryThreshold) continue;

        FaceRec nf;
        nf.mat = f.mat;
        for (const auto& p : f.pts) {
            ObjFaceVert np;
            int vi = p.v - 1;
            if (vRemap[vi] == 0) {
                outV.push_back(verts[vi]);
                vRemap[vi] = static_cast<int>(outV.size());
            }
            np.v = vRemap[vi];

            if (p.vt > 0) {
                int vti = p.vt - 1;
                if (vtRemap[vti] == 0) {
                    outVt.push_back(vts[vti]);
                    vtRemap[vti] = static_cast<int>(outVt.size());
                }
                np.vt = vtRemap[vti];
            }
            if (p.vn > 0) {
                int vni = p.vn - 1;
                if (vnRemap[vni] == 0) {
                    outVn.push_back(vns[vni]);
                    vnRemap[vni] = static_cast<int>(outVn.size());
                }
                np.vn = vnRemap[vni];
            }
            nf.pts.push_back(np);
        }
        keptFaces.push_back(std::move(nf));
    }

    if (keptFaces.empty()) return false;
    keptFacesOut = keptFaces.size();

    std::ofstream out(dstObj, std::ios::trunc);
    if (!out.is_open()) return false;
    out << "# RathoreScan 3D Studio - +Y-up clean mesh export\n";
    for (const auto& ml : mtllibLines) out << ml << "\n";
    // Rotate +Y-down (COLMAP/OpenMVS) -> +Y-up (Three.js/Unity/UE5)
    for (const auto& p : outV) out << "v " << p[0] << " " << -p[1] << " " << -p[2] << "\n";
    for (const auto& s : outVt) out << "vt" << s << "\n";
    for (const auto& n : outVn) out << "vn " << n[0] << " " << -n[1] << " " << -n[2] << "\n";

    std::string activeMat;
    for (const auto& f : keptFaces) {
        if (!f.mat.empty() && f.mat != activeMat) {
            activeMat = f.mat;
            out << "usemtl " << activeMat << "\n";
        }
        out << "f";
        for (const auto& p : f.pts) {
            out << " " << p.v;
            if (p.vt > 0 || p.vn > 0) {
                out << "/" << (p.vt > 0 ? std::to_string(p.vt) : "");
                if (p.vn > 0) out << "/" << p.vn;
            }
        }
        out << "\n";
    }
    out.close();
    return fs::exists(dstObj) && fs::file_size(dstObj) > 128;
}

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5) {
        logError("Usage: scan3d_engine <job_dir> <mode:video|photos|hybrid> <video_path|none> <preset:lowpoly|balanced|highpoly|ultra> [fps] [scene_mode:auto|subject|tree|full]");
        return 1;
    }

    fs::path jobDir = fs::absolute(argv[1]);
    std::string mode = argv[2];
    std::string videoArg = argv[3];
    std::string preset = argv[4];
    std::string fpsRaw = (argc >= 6) ? argv[5] : "3";
    std::string sceneMode = (argc >= 7) ? argv[6] : "auto";

    if (mode != "video" && mode != "photos" && mode != "hybrid") mode = "video";
    if (preset != "lowpoly" && preset != "balanced" && preset != "highpoly" && preset != "ultra") preset = "balanced";
    if (sceneMode != "auto" && sceneMode != "subject" && sceneMode != "tree" && sceneMode != "full") sceneMode = "auto";

    int fpsVal = 2;
    try { fpsVal = std::stoi(fpsRaw); } catch (...) { fpsVal = 2; }
    if (fpsVal < 1 || fpsVal > 10) fpsVal = 2;
    std::string fps = std::to_string(fpsVal);

    ToolConfig cfg = fs::exists("tools_config.txt")
        ? loadConfig("tools_config.txt")
        : loadConfig("../tools_config.txt");

    if (cfg.ffmpeg != "ffmpeg" && !fs::exists(cfg.ffmpeg)) {
        std::cerr << "[ERROR] FFmpeg not found at: " << cfg.ffmpeg << " (Update tools_config.txt)\n";
        return 2;
    }

    int maxImgSize = 1440, mvsResLevel = 2, texResLevel = 1;
    std::string decimateFactor = "0.45";
    bool runRefine = false;

    if (preset == "lowpoly") { maxImgSize = 1024; mvsResLevel = 3; texResLevel = 2; decimateFactor = "0.18"; }
    else if (preset == "balanced") { maxImgSize = 1440; mvsResLevel = 2; texResLevel = 1; decimateFactor = "0.45"; }
    else if (preset == "highpoly") { maxImgSize = 1920; mvsResLevel = 1; texResLevel = 1; decimateFactor = "0.80"; runRefine = true; }
    else if (preset == "ultra") { maxImgSize = 2400; mvsResLevel = 1; texResLevel = 0; decimateFactor = "1.0"; runRefine = true; }

    fs::path rawPhotoDir = jobDir / "raw_photos";
    fs::path imgDir = jobDir / "images";
    fs::path masksDir = jobDir / "masks";
    fs::path sparseDir = jobDir / "sparse";
    fs::path denseDir = jobDir / "dense";
    fs::path outDir = jobDir / "output";
    fs::path dbPath = jobDir / "database.db";

    fs::create_directories(imgDir);
    fs::create_directories(masksDir);
    fs::create_directories(sparseDir);
    fs::create_directories(denseDir);
    fs::create_directories(outDir);

    if (mode == "video" || mode == "hybrid") {
        fs::path videoPath = fs::absolute(videoArg);
        if (!fs::exists(videoPath)) {
            std::cerr << "[ERROR] Video file does not exist: " << videoPath.string() << "\n";
            return 2;
        }
        logProgress(8, "Extracting sharp frames from video via FFmpeg...");
        std::string ffCmd = "\"" + cfg.ffmpeg + "\" -y -i \"" + videoPath.string() +
            "\" -vf \"fps=" + fps + ",mpdecimate=hi=64*12:lo=64*5:frac=0.33,scale='min(" +
            std::to_string(maxImgSize) + ",iw)':-2\" -fps_mode vfr -qscale:v 2 \"" +
            (imgDir / "frame_%04d.jpg").string() + "\"";
        if (runCmd(ffCmd) != 0) {
            std::cerr << "[ERROR] FFmpeg video frame extraction failed.\n";
            return 2;
        }
    }

    if ((mode == "photos" || mode == "hybrid") && fs::exists(rawPhotoDir)) {
        std::vector<fs::path> photos;
        for (const auto& entry : fs::directory_iterator(rawPhotoDir)) {
            if (entry.is_regular_file() && isImageExt(entry.path().extension().string())) photos.push_back(entry.path());
        }
        std::sort(photos.begin(), photos.end());
        if (photos.empty() && mode == "photos") {
            std::cerr << "[ERROR] No valid photos found in job.\n";
            return 2;
        }
        logProgress(12, "Optimizing " + std::to_string(photos.size()) + " uploaded photo(s) to " + std::to_string(maxImgSize) + "px...");
        int idx = 1;
        for (const auto& p : photos) {
            std::ostringstream oss;
            oss << "photo_" << std::setw(4) << std::setfill('0') << idx++ << ".jpg";
            fs::path dst = imgDir / oss.str();
            std::string imgCmd = "\"" + cfg.ffmpeg + "\" -y -i \"" + p.string() +
                "\" -vf \"scale='min(" + std::to_string(maxImgSize) + ",iw)':-2\" -qscale:v 2 \"" + dst.string() + "\"";
            if (runCmd(imgCmd) != 0) std::cerr << "[WARN] Skipping unreadable photo: " << p.filename().string() << "\n";
        }
    }

    int totalImages = 0;
    for (const auto& entry : fs::directory_iterator(imgDir)) {
        if (entry.is_regular_file()) totalImages++;
    }
    if (totalImages < 1) {
        std::cerr << "[ERROR] Need at least 1 photo or video frame for 3D reconstruction.\n";
        return 2;
    }

    logProgress(16, "Inspecting " + std::to_string(totalImages) + " view(s) for unique angles & surface detail...");
    std::vector<fs::path> uniqueViews = deduplicateViews(cfg, imgDir, jobDir / "dedup_tmp");
    std::error_code dedupEc;
    fs::remove_all(jobDir / "dedup_tmp", dedupEc);

    int uniqueCount = static_cast<int>(uniqueViews.size());
    logProgress(18, "Detected " + std::to_string(uniqueCount) + " unique camera view(s).");

    if (uniqueCount == 1) {
        logProgress(22, "Single-View AI + Volumetric Deep-Scan Mode active...");
        if (!reconstructDeepScan(cfg, jobDir, uniqueViews[0], fs::path(), preset, sceneMode)) return 12;
        return 0;
    }

    bool hasPhotogrammetry = (cfg.colmap == "colmap" || fs::exists(cfg.colmap)) &&
                             (cfg.openmvsDir.empty() || fs::exists(cfg.openmvsDir));
    if (!hasPhotogrammetry) {
        logProgress(22, "COLMAP/OpenMVS not configured — running AI Deep-Scan 3D reconstruction...");
        fs::path backView = (uniqueCount >= 2) ? uniqueViews[1] : fs::path();
        return reconstructDeepScan(cfg, jobDir, uniqueViews[0], backView, preset, sceneMode) ? 0 : 12;
    }

    std::string colmapCall = "\"" + cfg.colmap + "\"";
#ifdef _WIN32
    if (cfg.colmap.size() >= 4 && cfg.colmap.substr(cfg.colmap.size() - 4) == ".bat") {
        colmapCall = "call \"" + cfg.colmap + "\"";
    }
#endif
    std::string singleCam = (mode == "video") ? "1" : "0";
    std::string maxFeats = (preset == "lowpoly") ? "4096" : (preset == "balanced" ? "6144" : "8192");

    logProgress(19, "AI Scene Understanding: analyzing What, Where, Why & Strategy...");
    AiSceneInfo aiInfo = runAiUnderstandBatch(cfg, imgDir, masksDir, sceneMode, jobDir / "scene_ai.json");
    if (aiInfo.valid) {
        logProgress(20, "[AI-WHAT] " + aiInfo.what);
        logProgress(21, "[AI-WHERE] " + aiInfo.where);
    }

    auto prepareSubjectMasks = [&]() -> bool {
        int existingMasks = 0;
        if (fs::exists(masksDir)) {
            for (const auto& entry : fs::directory_iterator(masksDir)) {
                if (entry.is_regular_file()) existingMasks++;
            }
        }
        if (existingMasks >= totalImages) return true; // Use pixel-accurate SegFormer masks!

        fs::path firstImg;
        for (const auto& entry : fs::directory_iterator(imgDir)) {
            if (entry.is_regular_file()) { firstImg = entry.path(); break; }
        }
        if (firstImg.empty()) return false;
        fs::path baseMask = jobDir / "subject_mask.png";
        std::string maskCmd = "\"" + cfg.ffmpeg + "\" -y -i \"" + firstImg.string() +
            "\" -vf \"format=gray,geq=lum='if(lte(pow((X-W/2)/(W*0.26),2)+pow((Y-H/2)/(H*0.47),2),1),255,0)'\" -frames:v 1 -update 1 \"" +
            baseMask.string() + "\"";
        if (runCmd(maskCmd) != 0 || !fs::exists(baseMask)) return false;
        for (const auto& entry : fs::directory_iterator(imgDir)) {
            if (entry.is_regular_file()) {
                fs::copy_file(baseMask, masksDir / (entry.path().filename().string() + ".png"), fs::copy_options::overwrite_existing);
            }
        }
        return true;
    };

    auto runSfmPass = [&](bool useMask, int sfmMaxDim) -> int {
        std::error_code ec;
        fs::remove(dbPath, ec);
        fs::remove_all(sparseDir, ec);
        fs::remove_all(denseDir, ec);
        fs::create_directories(sparseDir);
        fs::create_directories(denseDir);

        std::string maskFlag = "";
        if (useMask) {
            logProgress(20, "Generating Subject-Focus Mask (ignoring blurred background)...");
            if (prepareSubjectMasks()) maskFlag = " --ImageReader.mask_path \"" + masksDir.string() + "\"";
        }

        logProgress(25, std::string("Running COLMAP GPU SIFT at ") + std::to_string(sfmMaxDim) + "px on " + std::to_string(totalImages) +
            (useMask ? " masked views..." : " views..."));
        std::string featCmd = colmapCall + " feature_extractor"
            " --database_path \"" + dbPath.string() + "\""
            " --image_path \"" + imgDir.string() + "\"" + maskFlag +
            " --ImageReader.single_camera " + singleCam +
            " --FeatureExtraction.use_gpu 1"
            " --FeatureExtraction.max_image_size " + std::to_string(sfmMaxDim) +
            " --SiftExtraction.max_num_features " + maxFeats +
            " --SiftExtraction.domain_size_pooling 1";
        if (sfmMaxDim >= 1600 && (preset == "highpoly" || preset == "ultra")) featCmd += " --SiftExtraction.estimate_affine_shape 1";
        if (runCmd(featCmd) != 0) return -1;

        std::string matchCmd;
        if (totalImages <= 45) {
            logProgress(40, "Running Guided Exhaustive Matching (" + std::to_string(totalImages) + " views)...");
            matchCmd = colmapCall + " exhaustive_matcher --database_path \"" + dbPath.string() + "\" --FeatureMatching.use_gpu 1 --FeatureMatching.guided_matching 1";
        } else if (mode == "video") {
            logProgress(40, "Matching sequential video frames with Guided Matching on GPU...");
            matchCmd = colmapCall + " sequential_matcher --database_path \"" + dbPath.string() + "\" --FeatureMatching.use_gpu 1 --FeatureMatching.guided_matching 1 --SequentialMatching.overlap 18 --SequentialMatching.quadratic_overlap 1";
        } else if (totalImages <= 120) {
            logProgress(40, "Running Guided Exhaustive Matching for high accuracy...");
            matchCmd = colmapCall + " exhaustive_matcher --database_path \"" + dbPath.string() + "\" --FeatureMatching.use_gpu 1 --FeatureMatching.guided_matching 1";
        } else {
            logProgress(40, "Matching large 100+ photo set with quadratic overlap on GPU...");
            matchCmd = colmapCall + " sequential_matcher --database_path \"" + dbPath.string() + "\" --FeatureMatching.use_gpu 1 --FeatureMatching.guided_matching 1 --SequentialMatching.overlap 22 --SequentialMatching.quadratic_overlap 1";
        }
        if (runCmd(matchCmd) != 0) return -1;

        logProgress(52, "Computing 3D Camera Poses & Sparse Point Cloud...");
        std::string mapCmd = colmapCall + " mapper --database_path \"" + dbPath.string() + "\" --image_path \"" + imgDir.string() + "\" --output_path \"" + sparseDir.string() + "\" --Mapper.min_num_matches 10 --Mapper.init_min_num_inliers 40";
        if (runCmd(mapCmd) != 0) return -1;

        fs::path bestSparse;
        uintmax_t bestScore = 0;
        for (const auto& entry : fs::directory_iterator(sparseDir)) {
            if (!entry.is_directory()) continue;
            fs::path imgBin = entry.path() / "images.bin", ptsBin = entry.path() / "points3D.bin";
            if (fs::exists(imgBin) && fs::exists(ptsBin)) {
                uintmax_t score = fs::file_size(imgBin) + fs::file_size(ptsBin);
                if (score > bestScore) { bestScore = score; bestSparse = entry.path(); }
            }
        }
        if (bestSparse.empty()) return 0;

        // Auto-level 3D scene using Manhattan-World alignment (keeps horizon & walls straight)
        fs::path alignedSparse = sparseDir / "aligned";
        fs::create_directories(alignedSparse);
        std::string alignCmd = colmapCall + " model_orientation_aligner"
            " --image_path \"" + imgDir.string() + "\""
            " --input_path \"" + bestSparse.string() + "\""
            " --output_path \"" + alignedSparse.string() + "\""
            " --method MANHATTAN-WORLD"
            " --max_image_size " + std::to_string(std::min(sfmMaxDim, 1024));
        if (runCmd(alignCmd) == 0 && fs::exists(alignedSparse / "images.bin")) {
            bestSparse = alignedSparse;
        }

        logProgress(62, "Undistorting camera frames from aligned model (" + bestSparse.filename().string() + ")...");
        std::string undistCmd = colmapCall + " image_undistorter --image_path \"" + imgDir.string() + "\" --input_path \"" + bestSparse.string() + "\" --output_path \"" + denseDir.string() + "\" --output_type COLMAP --max_image_size " + std::to_string(sfmMaxDim);
        if (runCmd(undistCmd) != 0) return -1;

        int regCount = 0;
        if (fs::exists(denseDir / "images")) {
            for (const auto& e : fs::directory_iterator(denseDir / "images")) {
                if (e.is_regular_file()) regCount++;
            }
        }
        return regCount;
    };

    // GPU-safe resolution ladder: step down only on CUDA/tool failure (<0), never discard valid low-count diagnostics
    int usedSfmDim = maxImgSize;
    auto runSfmWithGpuLadder = [&](bool useMask) -> int {
        std::vector<int> ladder = { maxImgSize, 1600, 1280, 960 };
        std::vector<int> dims;
        for (int d : ladder) {
            if (d <= maxImgSize && (dims.empty() || dims.back() != d)) dims.push_back(d);
        }
        int lastRes = -1;
        for (size_t i = 0; i < dims.size(); ++i) {
            if (i > 0) {
                logProgress(24, "GPU VRAM fallback: stepping COLMAP resolution down to " + std::to_string(dims[i]) + "px...");
            }
            lastRes = runSfmPass(useMask, dims[i]);
            if (lastRes >= 0) {
                usedSfmDim = dims[i];
                return lastRes;
            }
        }
        return lastRes;
    };

    int regViews = 0;
    if (uniqueCount == 2) {
        regViews = runSfmWithGpuLadder(false);
        if (regViews < 2) {
            logProgress(25, "2 wide-angle/distinct photos detected — activating Dual-View AI Deep-Scan 3D Reconstruction...");
            if (!reconstructDeepScan(cfg, jobDir, uniqueViews[0], uniqueViews[1], preset, sceneMode)) return 12;
            return 0;
        }
    } else {
        if (sceneMode == "subject" || sceneMode == "tree") regViews = runSfmWithGpuLadder(true);
        else if (sceneMode == "full") regViews = runSfmWithGpuLadder(false);
        else {
            // Smart Auto: use AI semantic mask first when SegFormer detected an isolated subject/character/architecture/tree
            bool preferMask = aiInfo.valid && (aiInfo.archetype != "environment");
            regViews = runSfmWithGpuLadder(preferMask);
            if (regViews < std::min(5, totalImages)) {
                logProgress(22, "Only " + std::to_string(std::max(0, regViews)) + " views matched — retrying with alternate mask strategy...");
                int altViews = runSfmWithGpuLadder(!preferMask);
                if (altViews > regViews) regViews = altViews;
                else if (regViews > 0) regViews = runSfmWithGpuLadder(preferMask);
            }
        }
    }

    if (regViews < 2) {
        logProgress(25, "Insufficient SfM parallax (registered=" + std::to_string(regViews) + ") — activating AI Deep-Scan 3D Reconstruction fallback...");
        fs::path backView = (uniqueCount >= 2) ? uniqueViews.back() : fs::path();
        if (!reconstructDeepScan(cfg, jobDir, uniqueViews[0], backView, preset, sceneMode)) return 5;
        return 0;
    }
    logProgress(66, "Aligned " + std::to_string(regViews) + " / " + std::to_string(totalImages) + " camera views in 3D!");

    logProgress(68, "Converting camera model to OpenMVS TXT format...");
    std::string convCmd = colmapCall + " model_converter --input_path \"" + (denseDir / "sparse").string() + "\" --output_path \"" + (denseDir / "sparse").string() + "\" --output_type TXT";
    if (runCmd(convCmd) != 0) { std::cerr << "[ERROR] COLMAP model_converter failed.\n"; return 7; }

    double orbitDeg = computeOrbitSpanDeg(denseDir / "sparse" / "images.txt");
    std::string whyCoverage = (orbitDeg >= 160.0)
        ? ("Wide/360 camera orbit (" + std::to_string(static_cast<int>(std::round(orbitDeg))) + " deg) captured all sides cleanly.")
        : ("Camera only orbited ~" + std::to_string(static_cast<int>(std::round(orbitDeg))) + " deg (partial/front arc) — unfilmed back surfaces cannot appear without 360-deg views.");
    logProgress(71, "[AI-WHY] " + whyCoverage);

    logProgress(74, "Initializing OpenMVS scene...");
    std::string mvsInit = mvsExe(cfg, "InterfaceCOLMAP") + " -i dense -o scene.mvs --image-folder images";
    if (runCmd(mvsInit, jobDir.string()) != 0) { std::cerr << "[ERROR] OpenMVS InterfaceCOLMAP failed.\n"; return 8; }

    logProgress(82, "Generating Dense 3D Point Cloud...");
    auto cleanTempDmaps = [](const fs::path& dir) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".dmap") {
                fs::remove(entry.path(), ec);
            }
        }
    };
    struct DensifyProfile {
        int resLevel;
        int maxRes;
        int minRes;
        int numViews;
        int fusionFilter;
        int maxThreads;
        std::string label;
    };
    std::vector<DensifyProfile> densifyTiers;
    if (regViews >= 50) {
        int safeRes = std::max(mvsResLevel, 2);
        int safeMax = std::min(maxImgSize, 960);
        densifyTiers.push_back({ safeRes, safeMax, 320, std::min(4, regViews), 1, 4, "Large-scene fast memory-safe mode (" + std::to_string(regViews) + " views)" });
        densifyTiers.push_back({ 3, 720, 320, std::min(3, regViews), 1, 2, "Low-RAM retry (level 3, max 720px, 2 threads)" });
        densifyTiers.push_back({ 3, 560, 256, std::min(3, regViews), 0, 2, "Ultra-safe merge retry (level 3, max 560px)" });
    } else if (regViews >= 24) {
        densifyTiers.push_back({ mvsResLevel, maxImgSize, 480, std::min(4, regViews), (mvsResLevel <= 1 ? 1 : 2), 4, "Medium-scene balanced mode" });
        densifyTiers.push_back({ std::max(mvsResLevel + 1, 2), std::min(maxImgSize, 960), 320, std::min(4, regViews), 1, 4, "Memory-safe retry (level 2, max 960px)" });
        densifyTiers.push_back({ 3, 720, 320, std::min(3, regViews), 1, 2, "Low-RAM retry (level 3, max 720px)" });
    } else {
        densifyTiers.push_back({ mvsResLevel, maxImgSize, 480, std::min(5, regViews), 2, 0, "Preset full quality" });
        densifyTiers.push_back({ std::max(mvsResLevel + 1, 2), std::min(maxImgSize, 960), 320, std::min(4, regViews), 1, 4, "Memory-safe retry (level 2, max 960px)" });
        densifyTiers.push_back({ 3, 720, 320, std::min(3, regViews), 1, 2, "Low-RAM retry (level 3, max 720px)" });
    }

    bool densifyOk = false;
    std::error_code denseEc;
    for (size_t ti = 0; ti < densifyTiers.size(); ++ti) {
        const auto& dp = densifyTiers[ti];
        cleanTempDmaps(jobDir);
        fs::remove(jobDir / "scene_dense.mvs", denseEc);
        fs::remove(jobDir / "scene_dense.ply", denseEc);
        if (ti > 0 || regViews >= 50) {
            logProgress(82, "[AI-HOW] Dense cloud: " + dp.label);
        }
        std::string densifyCmd = mvsExe(cfg, "DensifyPointCloud") +
            " scene.mvs --resolution-level " + std::to_string(dp.resLevel) +
            " --max-resolution " + std::to_string(dp.maxRes) +
            " --min-resolution " + std::to_string(dp.minRes) +
            " --number-views " + std::to_string(std::max(2, dp.numViews)) +
            " --fusion-filter " + std::to_string(dp.fusionFilter) +
            " --remove-dmaps 1";
        if (dp.maxThreads > 0) {
            densifyCmd += " --max-threads " + std::to_string(dp.maxThreads);
        }
        if (runCmd(densifyCmd, jobDir.string()) == 0 &&
            fs::exists(jobDir / "scene_dense.mvs", denseEc) &&
            fs::exists(jobDir / "scene_dense.ply", denseEc) &&
            fs::file_size(jobDir / "scene_dense.ply", denseEc) > 256) {
            densifyOk = true;
            break;
        }
    }
    cleanTempDmaps(jobDir);
    if (!densifyOk) { logError("OpenMVS DensifyPointCloud failed across all memory tiers."); return 9; }

    // Statistical Outlier Removal (SOR): strip sparse floating noise points before meshing
    {
        fs::path densePly = jobDir / "scene_dense.ply";
        fs::path cleanPly = jobDir / "scene_dense_clean.ply";
        size_t totalPts = 0, keptPts = 0;
        logProgress(87, "Filtering dense point cloud outliers (SOR noise removal)...");
        if (statisticalOutlierRemovalPly(densePly, cleanPly, 12, 1.2, totalPts, keptPts)) {
            std::error_code sorEc;
            fs::remove(densePly, sorEc);
            fs::rename(cleanPly, densePly, sorEc);
            if (sorEc) {
                fs::copy_file(cleanPly, densePly, fs::copy_options::overwrite_existing, sorEc);
                fs::remove(cleanPly, sorEc);
            }
            size_t droppedPts = (totalPts >= keptPts) ? (totalPts - keptPts) : 0;
            double pct = (totalPts > 0) ? (100.0 * static_cast<double>(droppedPts) / static_cast<double>(totalPts)) : 0.0;
            char sorMsg[160];
            std::snprintf(sorMsg, sizeof(sorMsg), "[AI-HOW] Point-cloud SOR removed %zu/%zu sparse noise points (%.1f%%).", droppedPts, totalPts, pct);
            logProgress(88, sorMsg);
        } else {
            std::error_code sorEc;
            fs::remove(cleanPly, sorEc);
        }
    }

    logProgress(90, "Building Clean 3D Polygon Mesh (Preset: " + preset + ")...");
    std::string meshCmd = mvsExe(cfg, "ReconstructMesh") + " scene_dense.mvs -o scene_mesh.mvs --decimate " + decimateFactor +
        " --remove-spurious 35 --remove-spikes 1 --close-holes 0 --smooth 2";
    if (runCmd(meshCmd, jobDir.string()) != 0) { std::cerr << "[ERROR] OpenMVS ReconstructMesh failed.\n"; return 10; }

    fs::path meshPly = jobDir / "scene_mesh.ply";
    auto hasValidMesh = [](const fs::path& p) {
        if (!fs::exists(p) || fs::file_size(p) < 256) return false;
        std::ifstream in(p, std::ios::binary);
        std::string header;
        for (int i = 0; i < 20 && std::getline(in, header); ++i) {
            if (header.find("element face 0") != std::string::npos) return false;
            if (header.find("end_header") != std::string::npos) break;
        }
        return true;
    };

    if (!hasValidMesh(meshPly) && decimateFactor != "1.0") {
        logProgress(92, "Retrying Mesh Reconstruction without decimation...");
        runCmd(mvsExe(cfg, "ReconstructMesh") + " scene_dense.mvs -o scene_mesh.mvs --decimate 1", jobDir.string());
    }

    if (!hasValidMesh(meshPly)) {
        logProgress(35, "OpenMVS produced 0 faces — falling back to AI Deep-Scan 3D Reconstruction...");
        fs::path backView = (uniqueCount >= 2) ? uniqueViews.back() : fs::path();
        if (!reconstructDeepScan(cfg, jobDir, uniqueViews[0], backView, preset, sceneMode)) return 10;
        return 0;
    }

    std::string meshInputForTex = "scene_mesh.ply";
    if (runRefine) {
        logProgress(94, "Refining High-Frequency Surface Details...");
        if (runCmd(mvsExe(cfg, "RefineMesh") + " scene_dense.mvs -m scene_mesh.ply -o scene_refined.mvs --resolution-level 2 --max-face-area 64", jobDir.string()) == 0) {
            meshInputForTex = "scene_refined.ply";
        }
    }

    logProgress(97, "Baking Clean UV Map & Texture...");
    fs::path finalObj = outDir / "model.obj";
    auto makeTexCmd = [&](int resLvl) {
        return mvsExe(cfg, "TextureMesh") + " scene_dense.mvs -m " + meshInputForTex +
            " -o \"" + finalObj.string() + "\" --export-type obj --resolution-level " + std::to_string(resLvl) +
            " --outlier-threshold 0 --global-seam-leveling 0 --local-seam-leveling 0 --empty-color 8421504 --cost-smoothness-ratio 0.4";
    };
    int texExit = runCmd(makeTexCmd(texResLevel), jobDir.string());
    if (texExit != 0 && texResLevel < 2) {
        logProgress(97, "Retrying TextureMesh at memory-safe UV resolution...");
        texExit = runCmd(makeTexCmd(texResLevel + 1), jobDir.string());
    }
    if (texExit != 0) { logError("OpenMVS TextureMesh failed."); return 11; }

    fs::path finalMtl = outDir / "model.mtl";
    if (fs::exists(finalMtl)) {
        std::ifstream mtlIn(finalMtl);
        std::ostringstream mtlBuf;
        std::string line;
        while (std::getline(mtlIn, line)) {
            if (line.rfind("Tr ", 0) == 0) mtlBuf << "d 1.000000\n";
            else mtlBuf << line << "\n";
        }
        mtlIn.close();
        std::ofstream mtlOut(finalMtl, std::ios::trunc);
        mtlOut << mtlBuf.str();
    }

    // Connected-component floating shard cleanup + +Y-up orientation (COLMAP/OpenMVS +Y-down -> Three.js/Unity/UE5 +Y-up)
    if (fs::exists(finalObj)) {
        logProgress(98, "Removing disconnected floating shards & orienting +Y-up...");
        fs::path tmpObj = outDir / "model_clean_yup.obj";
        size_t keptFaces = 0, totalFaces = 0;
        if (filterObjShardsAndRotateYUp(finalObj, tmpObj, keptFaces, totalFaces)) {
            std::error_code ec;
            fs::rename(tmpObj, finalObj, ec);
            if (ec) {
                fs::copy_file(tmpObj, finalObj, fs::copy_options::overwrite_existing, ec);
                fs::remove(tmpObj, ec);
            }
            size_t dropped = (totalFaces >= keptFaces) ? (totalFaces - keptFaces) : 0;
            char shardMsg[180];
            std::snprintf(shardMsg, sizeof(shardMsg), "[AI-HOW] Clean mesh: kept %zu/%zu triangles (dropped %zu disconnected fragments).", keptFaces, totalFaces, dropped);
            logProgress(99, shardMsg);
        } else {
            fs::path fallbackObj = outDir / "model_yup.obj";
            std::ifstream objIn(finalObj);
            std::ofstream objOut(fallbackObj, std::ios::trunc);
            std::string line;
            while (std::getline(objIn, line)) {
                if (line.rfind("v ", 0) == 0 || line.rfind("vn ", 0) == 0) {
                    std::istringstream iss(line);
                    std::string tag;
                    double x = 0, y = 0, z = 0;
                    if (iss >> tag >> x >> y >> z) {
                        objOut << tag << " " << x << " " << -y << " " << -z;
                        std::string rest;
                        if (std::getline(iss, rest)) objOut << rest;
                        objOut << "\n";
                        continue;
                    }
                }
                objOut << line << "\n";
            }
            objIn.close();
            objOut.close();
            std::error_code ec;
            fs::rename(fallbackObj, finalObj, ec);
            if (ec) {
                fs::copy_file(fallbackObj, finalObj, fs::copy_options::overwrite_existing, ec);
                fs::remove(fallbackObj, ec);
            }
        }
    }

    writeSceneReportJson(
        outDir / "scene_report.json",
        aiInfo,
        whyCoverage,
        "SegFormer Semantic Mask + COLMAP CUDA (" + std::to_string(usedSfmDim) + "px, " + std::to_string(regViews) + "/" + std::to_string(totalImages) + " views) + SOR + OpenMVS Shard-Clean (" + preset + ")",
        orbitDeg,
        regViews,
        totalImages
    );

    logProgress(100, "3D Model Ready! Loading in Studio...");
    return 0;
}
