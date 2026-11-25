// RathoreScan 3D Studio — C++17 Photogrammetry Pipeline Engine
// Copyright (c) 2026 RathoreTech (https://github.com/mrrathoretech602/RathoreScan3D)
// Licensed under the RathoreScan 3D Source-Available & Protected License (see LICENSE).

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <iomanip>

namespace fs = std::filesystem;

struct ToolConfig {
    std::string ffmpeg = "ffmpeg";
    std::string colmap = "colmap";
    std::string openmvsDir = "";
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

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: scan3d_engine <job_dir> <mode:video|photos|hybrid> <video_path|none> <preset:lowpoly|balanced|highpoly|ultra> [fps] [scene_mode:auto|subject|full]\n";
        return 1;
    }

    fs::path jobDir = fs::absolute(argv[1]);
    std::string mode = argv[2];
    std::string videoArg = argv[3];
    std::string preset = argv[4];
    std::string fpsRaw = (argc >= 6) ? argv[5] : "3";
    std::string sceneMode = (argc >= 7) ? argv[6] : "auto";

    if (mode != "video" && mode != "photos" && mode != "hybrid") {
        mode = "video";
    }
    if (preset != "lowpoly" && preset != "balanced" && preset != "highpoly" && preset != "ultra") {
        preset = "balanced";
    }
    if (sceneMode != "auto" && sceneMode != "subject" && sceneMode != "full") {
        sceneMode = "auto";
    }

    int fpsVal = 2;
    try {
        fpsVal = std::stoi(fpsRaw);
    } catch (...) {
        fpsVal = 2;
    }
    if (fpsVal < 1 || fpsVal > 10) fpsVal = 2;
    std::string fps = std::to_string(fpsVal);

    ToolConfig cfg = fs::exists("tools_config.txt")
        ? loadConfig("tools_config.txt")
        : loadConfig("../tools_config.txt");

    if (cfg.ffmpeg != "ffmpeg" && !fs::exists(cfg.ffmpeg)) {
        std::cerr << "[ERROR] FFmpeg not found at: " << cfg.ffmpeg << " (Update tools_config.txt)\n";
        return 2;
    }
    if (cfg.colmap != "colmap" && !fs::exists(cfg.colmap)) {
        std::cerr << "[ERROR] COLMAP not found at: " << cfg.colmap << " (Update tools_config.txt)\n";
        return 3;
    }
    if (!cfg.openmvsDir.empty() && !fs::exists(cfg.openmvsDir)) {
        std::cerr << "[ERROR] OpenMVS folder not found at: " << cfg.openmvsDir << " (Update tools_config.txt)\n";
        return 8;
    }

    // Poly-Budget & Quality Presets (Optimized for speed, small size, and VRAM safety)
    int maxImgSize = 1440;
    int mvsResLevel = 2;
    int texResLevel = 1;
    std::string decimateFactor = "0.45";
    bool runRefine = false;

    if (preset == "lowpoly") {
        maxImgSize = 1024;
        mvsResLevel = 3;
        texResLevel = 2;
        decimateFactor = "0.18";
        runRefine = false;
    } else if (preset == "balanced") {
        maxImgSize = 1440;
        mvsResLevel = 2;
        texResLevel = 1;
        decimateFactor = "0.45";
        runRefine = false;
    } else if (preset == "highpoly") {
        maxImgSize = 1920;
        mvsResLevel = 1;
        texResLevel = 1;
        decimateFactor = "0.80";
        runRefine = true;
    } else if (preset == "ultra") {
        maxImgSize = 2400;
        mvsResLevel = 1;
        texResLevel = 0;
        decimateFactor = "1.0";
        runRefine = true;
    }

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

    // 1A. Extract frames from video (with duplicate-frame drop to save time & size)
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

    // 1B. Normalize uploaded multi-angle photos (100+ photos supported cleanly)
    if ((mode == "photos" || mode == "hybrid") && fs::exists(rawPhotoDir)) {
        std::vector<fs::path> photos;
        for (const auto& entry : fs::directory_iterator(rawPhotoDir)) {
            if (entry.is_regular_file() && isImageExt(entry.path().extension().string())) {
                photos.push_back(entry.path());
            }
        }
        std::sort(photos.begin(), photos.end());
        if (photos.empty() && mode == "photos") {
            std::cerr << "[ERROR] No valid photos found in job.\n";
            return 2;
        }
        logProgress(14, "Optimizing " + std::to_string(photos.size()) + " uploaded photos to " + std::to_string(maxImgSize) + "px...");
        int idx = 1;
        for (const auto& p : photos) {
            std::ostringstream oss;
            oss << "photo_" << std::setw(4) << std::setfill('0') << idx++ << ".jpg";
            fs::path dst = imgDir / oss.str();
            std::string imgCmd = "\"" + cfg.ffmpeg + "\" -y -i \"" + p.string() +
                "\" -vf \"scale='min(" + std::to_string(maxImgSize) + ",iw)':-2\" -qscale:v 2 \"" +
                dst.string() + "\"";
            if (runCmd(imgCmd) != 0) {
                std::cerr << "[WARN] Skipping unreadable photo: " << p.filename().string() << "\n";
            }
        }
    }

    int totalImages = 0;
    for (const auto& entry : fs::directory_iterator(imgDir)) {
        if (entry.is_regular_file()) totalImages++;
    }
    if (totalImages < 3) {
        std::cerr << "[ERROR] Need at least 3 sharp images/frames for 3D reconstruction (found " << totalImages << ").\n";
        return 2;
    }

    // Helper: run COLMAP SfM pass (with optional Subject-Focus foreground mask)
    std::string colmapCall = "\"" + cfg.colmap + "\"";
#ifdef _WIN32
    if (cfg.colmap.size() >= 4 && cfg.colmap.substr(cfg.colmap.size() - 4) == ".bat") {
        colmapCall = "call \"" + cfg.colmap + "\"";
    }
#endif
    std::string singleCam = (mode == "video") ? "1" : "0";
    std::string maxFeats = (preset == "lowpoly") ? "4096" : (preset == "balanced" ? "6144" : "8192");

    auto prepareSubjectMasks = [&]() -> bool {
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

    auto runSfmPass = [&](bool useMask) -> int {
        std::error_code ec;
        fs::remove(dbPath, ec);
        fs::remove_all(sparseDir, ec);
        fs::remove_all(denseDir, ec);
        fs::create_directories(sparseDir);
        fs::create_directories(denseDir);

        std::string maskFlag = "";
        if (useMask) {
            logProgress(20, "Generating Subject-Focus Mask (ignoring blurred background)...");
            if (prepareSubjectMasks()) {
                maskFlag = " --ImageReader.mask_path \"" + masksDir.string() + "\"";
            }
        }

        logProgress(25, std::string("Running COLMAP GPU SIFT on ") + std::to_string(totalImages) +
            (useMask ? " masked views..." : " views..."));
        std::string featCmd = colmapCall + " feature_extractor"
            " --database_path \"" + dbPath.string() + "\""
            " --image_path \"" + imgDir.string() + "\"" +
            maskFlag +
            " --ImageReader.single_camera " + singleCam +
            " --FeatureExtraction.use_gpu 1"
            " --FeatureExtraction.max_image_size " + std::to_string(maxImgSize) +
            " --SiftExtraction.max_num_features " + maxFeats +
            " --SiftExtraction.domain_size_pooling 1";
        if (preset == "highpoly" || preset == "ultra") {
            featCmd += " --SiftExtraction.estimate_affine_shape 1";
        }
        if (runCmd(featCmd) != 0) return -1;

        std::string matchCmd;
        if (totalImages <= 45) {
            logProgress(40, "Running Guided Exhaustive Matching (" + std::to_string(totalImages) + " views)...");
            matchCmd = colmapCall + " exhaustive_matcher"
                " --database_path \"" + dbPath.string() + "\""
                " --FeatureMatching.use_gpu 1"
                " --FeatureMatching.guided_matching 1";
        } else if (mode == "video") {
            logProgress(40, "Matching sequential video frames with Guided Matching on GPU...");
            matchCmd = colmapCall + " sequential_matcher"
                " --database_path \"" + dbPath.string() + "\""
                " --FeatureMatching.use_gpu 1"
                " --FeatureMatching.guided_matching 1"
                " --SequentialMatching.overlap 18"
                " --SequentialMatching.quadratic_overlap 1";
        } else if (totalImages <= 120) {
            logProgress(40, "Running Guided Exhaustive Matching for high accuracy...");
            matchCmd = colmapCall + " exhaustive_matcher"
                " --database_path \"" + dbPath.string() + "\""
                " --FeatureMatching.use_gpu 1"
                " --FeatureMatching.guided_matching 1";
        } else {
            logProgress(40, "Matching large 100+ photo set with quadratic overlap on GPU...");
            matchCmd = colmapCall + " sequential_matcher"
                " --database_path \"" + dbPath.string() + "\""
                " --FeatureMatching.use_gpu 1"
                " --FeatureMatching.guided_matching 1"
                " --SequentialMatching.overlap 22"
                " --SequentialMatching.quadratic_overlap 1";
        }
        if (runCmd(matchCmd) != 0) return -1;

        logProgress(52, "Computing 3D Camera Poses & Sparse Point Cloud...");
        std::string mapCmd = colmapCall + " mapper"
            " --database_path \"" + dbPath.string() + "\""
            " --image_path \"" + imgDir.string() + "\""
            " --output_path \"" + sparseDir.string() + "\""
            " --Mapper.min_num_matches 10"
            " --Mapper.init_min_num_inliers 40";
        if (runCmd(mapCmd) != 0) return -1;

        fs::path bestSparse;
        uintmax_t bestScore = 0;
        for (const auto& entry : fs::directory_iterator(sparseDir)) {
            if (!entry.is_directory()) continue;
            fs::path imgBin = entry.path() / "images.bin";
            fs::path ptsBin = entry.path() / "points3D.bin";
            if (fs::exists(imgBin) && fs::exists(ptsBin)) {
                uintmax_t score = fs::file_size(imgBin) + fs::file_size(ptsBin);
                if (score > bestScore) {
                    bestScore = score;
                    bestSparse = entry.path();
                }
            }
        }
        if (bestSparse.empty()) return 0;

        logProgress(62, "Undistorting camera frames from best sparse model (" + bestSparse.filename().string() + ")...");
        std::string undistCmd = colmapCall + " image_undistorter"
            " --image_path \"" + imgDir.string() + "\""
            " --input_path \"" + bestSparse.string() + "\""
            " --output_path \"" + denseDir.string() + "\""
            " --output_type COLMAP"
            " --max_image_size " + std::to_string(maxImgSize);
        if (runCmd(undistCmd) != 0) return -1;

        int regCount = 0;
        if (fs::exists(denseDir / "images")) {
            for (const auto& e : fs::directory_iterator(denseDir / "images")) {
                if (e.is_regular_file()) regCount++;
            }
        }
        return regCount;
    };

    int regViews = 0;
    if (sceneMode == "subject") {
        regViews = runSfmPass(true);
    } else if (sceneMode == "full") {
        regViews = runSfmPass(false);
    } else {
        regViews = runSfmPass(false);
        if (regViews < std::min(5, totalImages)) {
            logProgress(22, "Only " + std::to_string(std::max(0, regViews)) + " full-frame views matched — retrying with Subject-Focus Background Mask...");
            int maskedViews = runSfmPass(true);
            if (maskedViews > regViews) {
                regViews = maskedViews;
            } else if (regViews > 0) {
                // Restore unmasked pass if it actually had more views
                regViews = runSfmPass(false);
            }
        }
    }

    if (regViews < 2) {
        std::cerr << "[ERROR] COLMAP could not align enough 3D views (registered=" << regViews << "). Move slowly 360 degrees around a textured object or upload 15+ overlapping photos.\n";
        return 5;
    }
    logProgress(66, "Aligned " + std::to_string(regViews) + " / " + std::to_string(totalImages) + " camera views in 3D!");

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
        " -i dense -o scene.mvs --image-folder images";
    if (runCmd(mvsInit, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS InterfaceCOLMAP failed.\n";
        return 8;
    }

    // 8. OpenMVS DensifyPointCloud
    logProgress(82, "Generating Dense 3D Point Cloud...");
    std::string densifyCmd = mvsExe(cfg, "DensifyPointCloud") +
        " scene.mvs --resolution-level " + std::to_string(mvsResLevel) +
        " --max-resolution " + std::to_string(maxImgSize) +
        " --number-views 5";
    if (runCmd(densifyCmd, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS DensifyPointCloud failed.\n";
        return 9;
    }

    // 9. OpenMVS ReconstructMesh (with preset poly decimation + safe fallback)
    logProgress(90, "Building 3D Polygon Mesh (Preset: " + preset + ")...");
    std::string meshCmd = mvsExe(cfg, "ReconstructMesh") +
        " scene_dense.mvs -o scene_mesh.mvs --decimate " + decimateFactor;
    if (runCmd(meshCmd, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS ReconstructMesh failed.\n";
        return 10;
    }

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
        std::string retryMeshCmd = mvsExe(cfg, "ReconstructMesh") +
            " scene_dense.mvs -o scene_mesh.mvs --decimate 1";
        runCmd(retryMeshCmd, jobDir.string());
    }

    if (!hasValidMesh(meshPly)) {
        std::cerr << "[ERROR] Reconstructed 3D mesh has 0 faces (insufficient 3D parallax/overlap in video/photos). Move slowly 360 degrees around a textured object or upload 15+ overlapping photos.\n";
        return 10;
    }

    std::string meshInputForTex = "scene_mesh.ply";

    // 10. Optional RefineMesh (High-Poly / Ultra Presets)
    if (runRefine) {
        logProgress(94, "Refining High-Frequency Surface Details...");
        std::string refineCmd = mvsExe(cfg, "RefineMesh") +
            " scene_dense.mvs -m scene_mesh.ply -o scene_refined.mvs --resolution-level 2 --max-face-area 64";
        if (runCmd(refineCmd, jobDir.string()) == 0) {
            meshInputForTex = "scene_refined.ply";
        }
    }

    // 11. OpenMVS TextureMesh -> Clean UV Map & Texture (disable seam-leveling overflow blobs)
    logProgress(97, "Baking Clean UV Map & Texture...");
    fs::path finalObj = outDir / "model.obj";
    std::string texCmd = mvsExe(cfg, "TextureMesh") +
        " scene_dense.mvs -m " + meshInputForTex +
        " -o \"" + finalObj.string() + "\""
        " --export-type obj --resolution-level " + std::to_string(texResLevel) +
        " --outlier-threshold 0 --global-seam-leveling 0 --local-seam-leveling 0"
        " --empty-color 8421504 --cost-smoothness-ratio 0.4";
    if (runCmd(texCmd, jobDir.string()) != 0) {
        std::cerr << "[ERROR] OpenMVS TextureMesh failed.\n";
        return 11;
    }

    // Fix OpenMVS MTL quirk: replace "Tr 1.000000" (treated as 100% transparent by Three.js/glTF) with "d 1.000000"
    fs::path finalMtl = outDir / "model.mtl";
    if (fs::exists(finalMtl)) {
        std::ifstream mtlIn(finalMtl);
        std::ostringstream mtlBuf;
        std::string line;
        while (std::getline(mtlIn, line)) {
            if (line.rfind("Tr ", 0) == 0) {
                mtlBuf << "d 1.000000\n";
            } else {
                mtlBuf << line << "\n";
            }
        }
        mtlIn.close();
        std::ofstream mtlOut(finalMtl, std::ios::trunc);
        mtlOut << mtlBuf.str();
    }

    logProgress(100, "3D Model Ready! Loading in Studio...");
    return 0;
}
