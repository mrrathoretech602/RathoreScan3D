// RathoreScan 3D Studio — C++17 Photogrammetry Pipeline Engine
// Copyright (c) 2026 RathoreTech (https://github.com/mrrathoretech602/RathoreScan3D)
// Licensed under the RathoreScan 3D Source-Available & Protected License (see LICENSE).

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

namespace fs = std::filesystem;

struct ToolConfig {
    std::string ffmpeg = "ffmpeg";
    std::string colmap = "colmap";
    std::string openmvsDir = "";
};

ToolConfig loadConfig(const std::string& path) {
    ToolConfig cfg;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        auto last = val.find_last_not_of(" \r\n\t");
        if (last != std::string::npos) {
            val.erase(last + 1);
        } else {
            val.clear();
        }
        if (key == "FFMPEG_BIN") cfg.ffmpeg = val;
        else if (key == "COLMAP_BIN") cfg.colmap = val;
        else if (key == "OPENMVS_DIR") cfg.openmvsDir = val;
    }
    return cfg;
}

void logProgress(int pct, const std::string& msg) {
    std::cout << "[PROGRESS:" << pct << "] " << msg << std::endl;
    std::cout.flush();
}

int runCmd(const std::string& cmd, const std::string& workDir = "") {
    std::string fullCmd;
#ifdef _WIN32
    if (!workDir.empty()) {
        fullCmd = "cmd /C \"cd /D \"" + workDir + "\" && " + cmd + "\"";
    } else {
        fullCmd = "cmd /C \"" + cmd + "\"";
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

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: scan3d_engine <job_dir> <video_path> <preset:fast|balanced|ultra> <fps>\n";
        return 1;
    }

    fs::path jobDir = fs::absolute(argv[1]);
    fs::path videoPath = fs::absolute(argv[2]);
    std::string preset = argv[3];
    std::string fpsRaw = argv[4];

    if (!fs::exists(videoPath)) {
        std::cerr << "[ERROR] Video file does not exist.\n";
        return 1;
    }

    int fpsVal = 2;
    try {
        fpsVal = std::stoi(fpsRaw);
    } catch (...) {
        fpsVal = 2;
    }
    if (fpsVal < 1 || fpsVal > 10) fpsVal = 2;
    std::string fps = std::to_string(fpsVal);

    if (preset != "fast" && preset != "balanced" && preset != "ultra") {
        preset = "balanced";
    }

    ToolConfig cfg = loadConfig("tools_config.txt");

    // Preset Tuning (4GB VRAM Safe Defaults)
    int maxImgSize = 1600;
    int mvsResLevel = 2;
    std::string decimateFactor = "0.6";
    bool runRefine = false;

    if (preset == "fast") {
        maxImgSize = 1280;
        mvsResLevel = 2;
        decimateFactor = "0.4";
        runRefine = false;
    } else if (preset == "balanced") {
        maxImgSize = 1600;
        mvsResLevel = 2;
        decimateFactor = "0.65";
        runRefine = false;
    } else if (preset == "ultra") {
        maxImgSize = 2048;
        mvsResLevel = 1;
        decimateFactor = "0.85";
        runRefine = true;
    }

    fs::path imgDir = jobDir / "images";
    fs::path sparseDir = jobDir / "sparse";
    fs::path denseDir = jobDir / "dense";
    fs::path outDir = jobDir / "output";
    fs::path dbPath = jobDir / "database.db";

    fs::create_directories(imgDir);
    fs::create_directories(sparseDir);
    fs::create_directories(denseDir);
    fs::create_directories(outDir);

    // 1. FFmpeg Frame Extraction
    logProgress(10, "Extracting sharp video frames via FFmpeg...");
    std::string ffCmd = "\"" + cfg.ffmpeg + "\" -y -i \"" + videoPath.string() +
        "\" -vf \"fps=" + fps + ",scale='min(" + std::to_string(maxImgSize) + ",iw)':-2\" -qscale:v 2 \"" +
        (imgDir / "frame_%04d.jpg").string() + "\"";
    if (runCmd(ffCmd) != 0) {
        std::cerr << "[ERROR] FFmpeg frame extraction failed.\n";
        return 2;
    }

    // 2. COLMAP Feature Extraction (GPU SIFT, Single Camera Model for Video)
    logProgress(25, "Running COLMAP GPU SIFT Feature Extraction...");
    std::string colmapCall = "\"" + cfg.colmap + "\"";
#ifdef _WIN32
    if (cfg.colmap.size() >= 4 && cfg.colmap.substr(cfg.colmap.size() - 4) == ".bat") {
        colmapCall = "call \"" + cfg.colmap + "\"";
    }
#endif

    std::string featCmd = colmapCall + " feature_extractor"
        " --database_path \"" + dbPath.string() + "\""
        " --image_path \"" + imgDir.string() + "\""
        " --ImageReader.single_camera 1"
        " --SiftExtraction.use_gpu 1"
        " --SiftExtraction.max_image_size " + std::to_string(maxImgSize);
    if (runCmd(featCmd) != 0) {
        std::cerr << "[ERROR] COLMAP feature_extractor failed.\n";
        return 3;
    }

    // 3. COLMAP Sequential Matcher (10x faster than exhaustive for video)
    logProgress(40, "Matching sequential video frames on GPU...");
    std::string matchCmd = colmapCall + " sequential_matcher"
        " --database_path \"" + dbPath.string() + "\""
        " --SiftMatching.use_gpu 1"
        " --SequentialMatching.overlap 15";
    if (runCmd(matchCmd) != 0) {
        std::cerr << "[ERROR] COLMAP sequential_matcher failed.\n";
        return 4;
    }

    // 4. COLMAP Sparse Reconstruction (Structure from Motion)
    logProgress(52, "Computing 3D Camera Poses & Sparse Point Cloud...");
    std::string mapCmd = colmapCall + " mapper"
        " --database_path \"" + dbPath.string() + "\""
        " --image_path \"" + imgDir.string() + "\""
        " --output_path \"" + sparseDir.string() + "\"";
    if (runCmd(mapCmd) != 0 || !fs::exists(sparseDir / "0")) {
        std::cerr << "[ERROR] COLMAP mapper failed. Ensure video has slow 360 movement & good lighting.\n";
        return 5;
    }

    // 5. Undistort Images for OpenMVS
    logProgress(62, "Undistorting camera frames for Dense Reconstruction...");
    std::string undistCmd = colmapCall + " image_undistorter"
        " --image_path \"" + imgDir.string() + "\""
        " --input_path \"" + (sparseDir / "0").string() + "\""
        " --output_path \"" + denseDir.string() + "\""
        " --output_type COLMAP"
        " --max_image_size " + std::to_string(maxImgSize);
    if (runCmd(undistCmd) != 0) {
        std::cerr << "[ERROR] COLMAP image_undistorter failed.\n";
        return 6;
    }

    // 6. Export Sparse TXT inside dense/sparse for OpenMVS InterfaceCOLMAP
    logProgress(68, "Converting camera model to OpenMVS TXT format...");
    std::string convCmd = colmapCall + " model_converter"
        " --input_path \"" + (denseDir / "sparse").string() + "\""
        " --output_path \"" + (denseDir / "sparse").string() + "\""
        " --output_type TXT";
    if (runCmd(convCmd) != 0) {
        std::cerr << "[ERROR] COLMAP model_converter failed.\n";
        return 7;
    }

    // 7. OpenMVS InterfaceCOLMAP
    logProgress(74, "Initializing OpenMVS scene...");
    std::string mvsInit = mvsExe(cfg, "InterfaceCOLMAP") +
        " -i dense -o scene.mvs --image-folder dense/images";
    if (runCmd(mvsInit, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS InterfaceCOLMAP failed.\n";
        return 8;
    }

    // 8. OpenMVS DensifyPointCloud
    logProgress(82, "Generating Dense 3D Point Cloud (CUDA/Multi-Thread)...");
    std::string densifyCmd = mvsExe(cfg, "DensifyPointCloud") +
        " scene.mvs --resolution-level " + std::to_string(mvsResLevel) +
        " --max-resolution " + std::to_string(maxImgSize) +
        " --number-views 5";
    if (runCmd(densifyCmd, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS DensifyPointCloud failed.\n";
        return 9;
    }

    // 9. OpenMVS ReconstructMesh
    logProgress(90, "Building 3D Polygon Mesh...");
    std::string meshCmd = mvsExe(cfg, "ReconstructMesh") +
        " scene_dense.mvs -o scene_mesh.mvs --decimate " + decimateFactor;
    if (runCmd(meshCmd, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS ReconstructMesh failed.\n";
        return 10;
    }

    std::string meshInputForTex = "scene_mesh.ply";

    // 10. Optional RefineMesh (Ultra Preset)
    if (runRefine) {
        logProgress(94, "Refining High-Frequency Surface Details...");
        std::string refineCmd = mvsExe(cfg, "RefineMesh") +
            " scene_dense.mvs -m scene_mesh.ply -o scene_refined.mvs --resolution-level 2 --max-face-area 64";
        if (runCmd(refineCmd, jobDir.string()) == 0) {
            meshInputForTex = "scene_refined.ply";
        }
    }

    // 11. OpenMVS TextureMesh -> Final OBJ + MTL + PBR Albedo Map
    logProgress(97, "Baking UV Map & High-Res Texture for Unity / Unreal...");
    fs::path finalObj = outDir / "model.obj";
    std::string texCmd = mvsExe(cfg, "TextureMesh") +
        " scene_dense.mvs -m " + meshInputForTex +
        " -o \"" + finalObj.string() + "\""
        " --export-type obj --resolution-level 1";
    if (runCmd(texCmd, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS TextureMesh failed.\n";
        return 11;
    }

    logProgress(100, "3D Model Ready! Loading in Studio...");
    return 0;
}
