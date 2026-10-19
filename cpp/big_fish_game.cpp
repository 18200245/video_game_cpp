// ============================================================================
// BIG FISH EATS SMALL FISH (CA LON NUOT CA BE) — OpenCV Game Video Simulator
// (Ultra-Fast Multi-Threaded & Pre-Baked LUT Render Edition)
// ============================================================================
// Build command:
//   g++ big_fish_game.cpp -o big_fish_game $(pkg-config --cflags --libs opencv4) -std=c++17 -O3 -march=native -pthread
//
// Run examples:
//   ./big_fish_game -o feeding_frenzy.mp4 --duration 100
//   ./big_fish_game -o feeding_frenzy.mp4 -s subtitles.srt --bg-folder ./backgrounds -w 1280 -h 720 -f 30
// ============================================================================

#include <opencv2/opencv.hpp>
#include <opencv2/freetype.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <random>
#include <cmath>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <cstdio>

using namespace cv;
using namespace std;
namespace fs = std::filesystem;

// ============================================================================
// 1. Structs & Constants
// ============================================================================

struct Config {
    // I/O
    string outputPath       = "big_fish_game.mp4";
    string subtitleFile     = "";           // optional
    string fontFile         = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    string backgroundFile   = "";
    string backgroundFolder = "";
    int    bgChangeInterval = 300;
    double bgOpacity        = 1.0;

    // Video
    int    width            = 1280;
    int    height           = 720;
    int    fps              = 30;
    int    duration         = 60;           // giay, dung khi khong co subtitle
    int    seed             = -1;
    int    numThreads       = max(1, (int)thread::hardware_concurrency());

    // Font / Subtitle
    int    fontSize         = 32;
    int    subtitleY        = -1;
    int    subtitleOffset   = -1;

    // Physics & Gameplay (Ca Lon Nuot Ca Be - Qua Tung Man)
    float  gravity          = 0.12f;        // luc chim nhe duoi nuoc (buoyancy sinking)
    float  swimAccel        = 0.85f;        // gia toc boi len/xuong cua ca
    float  waterDrag        = 0.90f;        // luc can cua nuoc
    float  maxVerticalSpeed = 9.5f;         // toc do boi doc toi da
    float  baseGameSpeed    = 5.2f;         // toc do troi ngang co ban
    int    lives            = 3;            // so mang
    int    baseStageTarget  = 10;           // so ca can an de qua man 1
};

struct Subtitle {
    double start = 0.0;
    double end   = 0.0;
    string text  = "";
};

struct FtMetrics {
    int ascender  = 0;
    int descender = 0;
    int height    = 0;
};

// Loai sinh vat / chuong ngai vat duoi dai duong
enum EntityType {
    ENT_TINY_FISH    = 0,  // Tier 1: Ca nho (luon an duoc)
    ENT_MEDIUM_FISH  = 1,  // Tier 2: Ca vua (an duoc khi Player dat Tier >= 2)
    ENT_LARGE_FISH   = 2,  // Tier 3: Ca lon (an duoc khi Player dat Tier >= 3)
    ENT_APEX_SHARK   = 3,  // Tier 4: Ca map khong lo (luon nguy hiem, phai ne!)
    ENT_SEA_MINE     = 4,  // Thuy loi gai (luon nguy hiem, phai ne!)
    ENT_JELLYFISH    = 5,  // Sua dien phat sang (boi nhap nhay doc, nguy hiem!)
    ENT_GOLDEN_PEARL = 6   // Ngoc trai vang (thuong diem lon + tien hoa nhanh)
};

struct Obstacle {
    float x          = 0.0f;
    float y          = 0.0f;
    float baseY      = 0.0f;
    float vx         = 0.0f;
    float vy         = 0.0f;
    int   w          = 48;
    int   h          = 32;
    int   type       = ENT_TINY_FISH;
    int   sizeTier   = 1;     // 1: Nho, 2: Vua, 3: Lon, 4: Nguy hiem
    int   colorTheme = 0;
    float phase      = 0.0f;
    float waveAmp    = 0.0f;
    float waveFreq   = 0.0f;
    bool  passed     = false;
    bool  eaten      = false;
};

struct Particle {
    float  x        = 0.0f;
    float  y        = 0.0f;
    float  vx       = 0.0f;
    float  vy       = 0.0f;
    float  alpha    = 1.0f;
    float  size     = 4.0f;
    Scalar color    = Scalar(255, 240, 200);
    bool   isBubble = true;
};

struct ScorePop {
    int    x     = 0;
    float  y     = 0.0f;
    int    value = 0;
    string label = "";
    float  alpha = 1.0f;
    Scalar color = Scalar(50, 235, 255);
};

struct CoralProp {
    float  x     = 0.0f;
    int    w     = 40;
    int    h     = 90;
    int    style = 0;
    Scalar color = Scalar(120, 80, 200);
    float  phase = 0.0f;
};

struct BgBubble {
    float x     = 0.0f;
    float y     = 0.0f;
    float speed = 1.0f;
    float r     = 3.0f;
    float phase = 0.0f;
};

// Snapshot trang thai cua 1 frame de render song song tren nhieu luong (Data-Parallel Render)
struct FrameSnapshot {
    int               frameId         = 0;
    double            cycle           = 0.0;
    int               score           = 0;
    int               bestScore       = 0;
    int               lives           = 3;
    int               stage           = 1;
    int               playerTier      = 1;
    int               eatenInStage    = 0;
    int               stageTarget     = 10;
    float             scrollX         = 0.0f;
    float             playerX         = 0.0f;
    float             playerY         = 0.0f;
    int               playerW         = 62;
    int               playerH         = 40;
    int               mouthOpenTimer  = 0;
    int               invincibleTimer = 0;
    int               overlayTimer    = 0;
    string            overlayTitle    = "";
    string            overlaySubtitle = "";
    Scalar            overlayColor    = Scalar(60, 220, 255);
    int               milestoneTimer  = 0;
    string            milestoneText   = "";
    int               subIdx          = -1;
    vector<Obstacle>  obstacles;
    vector<Particle>  particles;
    vector<ScorePop>  scorePops;
    vector<CoralProp> corals;
    vector<BgBubble>  bgBubbles;
};

// Thread Pool tai su dung luong xuyen suot qua trinh render (khong ton chi phi tao thread moi batch)
class RenderThreadPool {
public:
    explicit RenderThreadPool(int numThreads) : stop_(false), activeTasks_(0) {
        int n = max(1, numThreads);
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(mtx_);
                        cvTask_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                    {
                        unique_lock<mutex> lock(mtx_);
                        if (--activeTasks_ == 0 && tasks_.empty()) {
                            cvDone_.notify_all();
                        }
                    }
                }
            });
        }
    }

    ~RenderThreadPool() {
        {
            unique_lock<mutex> lock(mtx_);
            stop_ = true;
        }
        cvTask_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    void parallelFor(int count, const function<void(int)>& fn) {
        if (count <= 0) return;
        if (workers_.size() <= 1 || count == 1) {
            for (int i = 0; i < count; ++i) fn(i);
            return;
        }
        {
            unique_lock<mutex> lock(mtx_);
            activeTasks_ += count;
            for (int i = 0; i < count; ++i) {
                tasks_.push([fn, i]() { fn(i); });
            }
        }
        cvTask_.notify_all();
        unique_lock<mutex> lock(mtx_);
        cvDone_.wait(lock, [this]() { return activeTasks_ == 0 && tasks_.empty(); });
    }

private:
    vector<thread>          workers_;
    queue<function<void()>> tasks_;
    mutex                   mtx_;
    condition_variable      cvTask_;
    condition_variable      cvDone_;
    bool                    stop_;
    int                     activeTasks_;
};

// ============================================================================
// 2. parseArguments()
// ============================================================================

void printHelp(const char* progName) {
    cout << "Usage: " << progName << " [options]\n\n"
         << "Options:\n"
         << "  -o, --output <path>         File video output (default: big_fish_game.mp4)\n"
         << "  -s, --subtitles <path>      File SRT (optional — neu bo, dung --duration)\n"
         << "  --duration <int>            Thoi luong video khi khong co subtitle (default: 60)\n"
         << "  -b, --background <path>     Anh nen tinh\n"
         << "  --bg-folder <path>          Thu muc anh nen xoay vong\n"
         << "  --bg-interval <int>         Doi anh nen moi N frames (default: 300)\n"
         << "  --bg-opacity <float>        Do mo nen (0.0-1.0, default: 1.0)\n"
         << "  --font <path>               File font TTF\n"
         << "  --font-size <int>           Co chu subtitle (default: 32)\n"
         << "  -w, --width <int>           Chieu rong video (default: 1280)\n"
         << "  -h, --height <int>          Chieu cao video (default: 720)\n"
         << "  -f, --fps <int>             FPS (default: 30)\n"
         << "  --seed <int>                Random seed (default: random)\n"
         << "  --threads <int>             So luong render song song\n"
         << "  --subtitle-y <int>          Vi tri Y subtitle tu duoi len\n"
         << "  --help                      In huong dan\n";
}

bool parseArguments(int argc, char* argv[], Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--help") {
            printHelp(argv[0]);
            return false;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            cfg.outputPath = argv[++i];
        } else if ((arg == "-s" || arg == "--subtitles") && i + 1 < argc) {
            cfg.subtitleFile = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            cfg.duration = max(1, stoi(argv[++i]));
        } else if ((arg == "-b" || arg == "--background") && i + 1 < argc) {
            cfg.backgroundFile = argv[++i];
        } else if (arg == "--bg-folder" && i + 1 < argc) {
            cfg.backgroundFolder = argv[++i];
        } else if (arg == "--bg-interval" && i + 1 < argc) {
            cfg.bgChangeInterval = max(1, stoi(argv[++i]));
        } else if (arg == "--bg-opacity" && i + 1 < argc) {
            cfg.bgOpacity = std::clamp(stod(argv[++i]), 0.0, 1.0);
        } else if (arg == "--font" && i + 1 < argc) {
            cfg.fontFile = argv[++i];
        } else if (arg == "--font-size" && i + 1 < argc) {
            cfg.fontSize = max(10, stoi(argv[++i]));
        } else if ((arg == "-w" || arg == "--width") && i + 1 < argc) {
            cfg.width = max(320, stoi(argv[++i]));
        } else if ((arg == "-h" || arg == "--height") && i + 1 < argc) {
            cfg.height = max(240, stoi(argv[++i]));
        } else if ((arg == "-f" || arg == "--fps") && i + 1 < argc) {
            cfg.fps = max(1, stoi(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            cfg.seed = stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            cfg.numThreads = max(1, stoi(argv[++i]));
        } else if (arg == "--subtitle-y" && i + 1 < argc) {
            cfg.subtitleY = stoi(argv[++i]);
            cfg.subtitleOffset = cfg.subtitleY;
        } else {
            cerr << "[Warning] Tham so khong hop le hoac thieu gia tri: " << arg << "\n";
        }
    }
    return true;
}

// ============================================================================
// 3. BackgroundManager (Pre-baked Ocean Gradient & Sun-Ray LUT Cache)
// ============================================================================

static Scalar lerpScalar(const Scalar& a, const Scalar& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return Scalar(
        a[0] * (1.0 - t) + b[0] * t,
        a[1] * (1.0 - t) + b[1] * t,
        a[2] * (1.0 - t) + b[2] * t
    );
}

class BackgroundManager {
public:
    BackgroundManager(const Config& cfg)
        : width_(cfg.width), height_(cfg.height),
          changeInterval_(cfg.bgChangeInterval), opacity_(cfg.bgOpacity) {
        if (!cfg.backgroundFile.empty()) {
            loadSingleImage(cfg.backgroundFile);
        } else if (!cfg.backgroundFolder.empty()) {
            loadFolder(cfg.backgroundFolder);
        }
        buildOceanCycleCache();
    }

    bool hasBackground() const {
        return !images_.empty();
    }

    // Copy pre-computed background in ~0.15ms instead of recomputing 921,600 pixels every frame
    void render(Mat& frame, int frameId, double cycle) const {
        int cIdx = std::clamp((int)(cycle * OCEAN_CACHE_STEPS) % OCEAN_CACHE_STEPS, 0, OCEAN_CACHE_STEPS - 1);
        const Mat& oceanBg = oceanCycleCache_[cIdx];

        if (images_.empty()) {
            oceanBg.copyTo(frame);
            return;
        }
        int idx = 0;
        if (images_.size() > 1 && changeInterval_ > 0) {
            idx = (frameId / changeInterval_) % (int)images_.size();
        }
        const Mat& bg = images_[idx];
        if (opacity_ >= 0.999) {
            bg.copyTo(frame);
        } else {
            addWeighted(bg, opacity_, oceanBg, 1.0 - opacity_, 0.0, frame);
        }
    }

    static void drawVerticalGradient(Mat& frame, const Scalar& topColor, const Scalar& botColor) {
        int h = frame.rows;
        int w = frame.cols;
        for (int y = 0; y < h; ++y) {
            double t = (double)y / max(1, h - 1);
            Vec3b c(
                saturate_cast<uchar>(topColor[0] * (1.0 - t) + botColor[0] * t),
                saturate_cast<uchar>(topColor[1] * (1.0 - t) + botColor[1] * t),
                saturate_cast<uchar>(topColor[2] * (1.0 - t) + botColor[2] * t)
            );
            Vec3b* rowPtr = frame.ptr<Vec3b>(y);
            std::fill(rowPtr, rowPtr + w, c);
        }
    }

private:
    static constexpr int OCEAN_CACHE_STEPS = 32;
    int width_;
    int height_;
    int changeInterval_;
    double opacity_;
    vector<Mat> images_;
    vector<Mat> oceanCycleCache_;

    // Pre-bake 32 ocean cycle frames (water gradient + sun rays + sandy seabed gradient) once at startup
    void buildOceanCycleCache() {
        oceanCycleCache_.resize(OCEAN_CACHE_STEPS);
        int surfaceY = 82;
        int groundY  = height_ - 64;

        Scalar dayTop(235, 175, 45),   dayBot(145, 75, 10);
        Scalar duskTop(165, 95, 75),   duskBot(85, 35, 25);
        Scalar nightTop(75, 35, 15),   nightBot(25, 10, 5);

        for (int s = 0; s < OCEAN_CACHE_STEPS; ++s) {
            double cycle = (double)s / OCEAN_CACHE_STEPS;
            Scalar oceanTop, oceanBot;
            if (cycle < 0.5) {
                oceanTop = lerpScalar(dayTop, duskTop, cycle * 2.0);
                oceanBot = lerpScalar(dayBot, duskBot, cycle * 2.0);
            } else {
                oceanTop = lerpScalar(duskTop, nightTop, (cycle - 0.5) * 2.0);
                oceanBot = lerpScalar(duskBot, nightBot, (cycle - 0.5) * 2.0);
            }

            Mat canvas(height_, width_, CV_8UC3);
            drawVerticalGradient(canvas, oceanTop, oceanBot);

            if (cycle < 0.65) {
                double rayAlpha = (1.0 - cycle / 0.65) * 0.12;
                Mat overlay = canvas.clone();
                for (int i = 0; i < 5; ++i) {
                    int topX = 160 + i * (width_ / 5);
                    Point pts[4] = {
                        Point(topX - 32, surfaceY),
                        Point(topX + 32, surfaceY),
                        Point(topX + 145, groundY),
                        Point(topX - 85, groundY)
                    };
                    fillConvexPoly(overlay, pts, 4, Scalar(240, 255, 255), LINE_8);
                }
                addWeighted(overlay, rayAlpha, canvas, 1.0 - rayAlpha, 0.0, canvas);
            }

            Scalar sandTop = lerpScalar(Scalar(115, 195, 235), Scalar(65, 95, 125), cycle);
            Scalar sandBot = lerpScalar(Scalar(75, 145, 190),  Scalar(35, 55, 80),  cycle);
            Rect groundRect(0, groundY, width_, height_ - groundY);
            if (groundRect.height > 0) {
                Mat groundRoi = canvas(groundRect);
                drawVerticalGradient(groundRoi, sandTop, sandBot);
            }

            oceanCycleCache_[s] = std::move(canvas);
        }
    }

    void loadSingleImage(const string& path) {
        Mat img = imread(path, IMREAD_COLOR);
        if (!img.empty()) {
            Mat resized;
            resize(img, resized, Size(width_, height_), 0, 0, INTER_LINEAR);
            images_.push_back(resized);
        } else {
            cerr << "[BackgroundManager] Khong the doc anh: " << path << "\n";
        }
    }

    void loadFolder(const string& folderPath) {
        error_code ec;
        if (!fs::exists(folderPath, ec) || !fs::is_directory(folderPath, ec)) {
            cerr << "[BackgroundManager] Thu muc khong ton tai: " << folderPath << "\n";
            return;
        }
        vector<string> files;
        for (const auto& entry : fs::directory_iterator(folderPath, ec)) {
            if (!entry.is_regular_file()) continue;
            string ext = entry.path().extension().string();
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".webp") {
                files.push_back(entry.path().string());
            }
        }
        sort(files.begin(), files.end());
        for (const auto& f : files) {
            loadSingleImage(f);
        }
    }
};

// ============================================================================
// 4. parseSRT()
// ============================================================================

static double parseSrtTimestamp(const string& ts) {
    int hh = 0, mm = 0, ss = 0, ms = 0;
    char sep = ',';
    if (sscanf(ts.c_str(), "%d:%d:%d%c%d", &hh, &mm, &ss, &sep, &ms) >= 4) {
        return hh * 3600.0 + mm * 60.0 + ss + ms / 1000.0;
    }
    return 0.0;
}

vector<Subtitle> parseSRT(const string& filename) {
    vector<Subtitle> subs;
    if (filename.empty()) return subs;

    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "[SRT] Khong mo duoc file subtitle: " << filename << " -> su dung --duration.\n";
        return subs;
    }

    string line;
    while (getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        bool isIndex = all_of(line.begin(), line.end(), ::isdigit);
        if (isIndex) {
            if (!getline(file, line)) break;
            if (!line.empty() && line.back() == '\r') line.pop_back();
        }

        size_t arrowPos = line.find("-->");
        if (arrowPos == string::npos) continue;

        string startStr = line.substr(0, arrowPos);
        string endStr   = line.substr(arrowPos + 3);
        Subtitle sub;
        sub.start = parseSrtTimestamp(startStr);
        sub.end   = parseSrtTimestamp(endStr);

        string textAccum;
        while (getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (!textAccum.empty()) textAccum += " ";
            textAccum += line;
        }
        sub.text = textAccum;
        if (!sub.text.empty()) {
            subs.push_back(sub);
        }
    }
    return subs;
}

// ============================================================================
// 5. FreeType2 subtitle
// ============================================================================

static Ptr<freetype::FreeType2> g_ft2;
static bool g_ft2Available = false;
static map<int, FtMetrics> g_ftMetricsCache;
static mutex g_ftMutex;

void initFreeType(const string& fontFile) {
    try {
        g_ft2 = freetype::createFreeType2();
        if (fs::exists(fontFile)) {
            g_ft2->loadFontData(fontFile, 0);
            g_ft2Available = true;
        } else {
            vector<string> fallbacks = {
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf"
            };
            for (const auto& fb : fallbacks) {
                if (fs::exists(fb)) {
                    g_ft2->loadFontData(fb, 0);
                    g_ft2Available = true;
                    break;
                }
            }
        }
    } catch (const cv::Exception& e) {
        cerr << "[FreeType2] Canh bao khoi tao font: " << e.what() << "\n";
        g_ft2Available = false;
    }
}

FtMetrics getFtMetrics(int fontSize) {
    auto it = g_ftMetricsCache.find(fontSize);
    if (it != g_ftMetricsCache.end()) return it->second;

    FtMetrics m;
    if (g_ft2Available && g_ft2) {
        int baseline = 0;
        Size sz = g_ft2->getTextSize("Ayg|", fontSize, -1, &baseline);
        m.ascender  = sz.height;
        m.descender = max(6, baseline);
        m.height    = m.ascender + m.descender;
    } else {
        m.ascender  = fontSize;
        m.descender = fontSize / 4;
        m.height    = m.ascender + m.descender;
    }
    g_ftMetricsCache[fontSize] = m;
    return m;
}

static int measureTextWidth(const string& text, int fontSize) {
    if (text.empty()) return 0;
    if (g_ft2Available && g_ft2) {
        int baseline = 0;
        return g_ft2->getTextSize(text, fontSize, -1, &baseline).width;
    }
    int baseline = 0;
    double scale = fontSize / 32.0;
    return getTextSize(text, FONT_HERSHEY_SIMPLEX, scale, 2, &baseline).width;
}

void drawSubtitle(Mat& frame, const string& text, const Config& cfg) {
    if (text.empty()) return;

    lock_guard<mutex> lock(g_ftMutex);
    int maxWidth = cfg.width - 120;
    FtMetrics metrics = getFtMetrics(cfg.fontSize);

    vector<string> lines;
    istringstream iss(text);
    string word, currentLine;
    while (iss >> word) {
        string candidate = currentLine.empty() ? word : (currentLine + " " + word);
        if (measureTextWidth(candidate, cfg.fontSize) <= maxWidth || currentLine.empty()) {
            currentLine = candidate;
        } else {
            lines.push_back(currentLine);
            currentLine = word;
        }
    }
    if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }
    if (lines.empty()) return;

    int lineGap = 8;
    int padX    = 24;
    int padY    = 14;
    int maxLineW = 0;
    vector<int> lineWidths(lines.size(), 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        lineWidths[i] = measureTextWidth(lines[i], cfg.fontSize);
        maxLineW = max(maxLineW, lineWidths[i]);
    }

    int boxW = min(cfg.width - 40, maxLineW + padX * 2);
    int boxH = (int)lines.size() * metrics.height + ((int)lines.size() - 1) * lineGap + padY * 2;
    int boxX = (cfg.width - boxW) / 2;
    int bottomMargin = (cfg.subtitleY > 0) ? cfg.subtitleY : 58;
    int boxY = cfg.height - bottomMargin - boxH;
    boxX = std::clamp(boxX, 0, max(0, cfg.width - boxW));
    boxY = std::clamp(boxY, 0, max(0, cfg.height - boxH));

    // In-place SIMD ROI darkening (70% dark) — zero Mat allocation!
    Rect roiRect(boxX, boxY, boxW, boxH);
    roiRect &= Rect(0, 0, frame.cols, frame.rows);
    if (roiRect.width > 0 && roiRect.height > 0) {
        Mat roi = frame(roiRect);
        roi *= 0.30;
        roi += Scalar(15 * 0.70, 18 * 0.70, 25 * 0.70);
        rectangle(frame, roiRect, Scalar(100, 100, 100), 1, LINE_8);
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        int tx = (cfg.width - lineWidths[i]) / 2;
        int ty = boxY + padY + (int)i * (metrics.height + lineGap);

        if (g_ft2Available && g_ft2) {
            g_ft2->putText(frame, lines[i], Point(tx + 2, ty + 2),
                           cfg.fontSize, Scalar(0, 0, 0), -1, LINE_AA, false);
            g_ft2->putText(frame, lines[i], Point(tx, ty),
                           cfg.fontSize, Scalar(255, 255, 255), -1, LINE_AA, false);
        } else {
            double scale = cfg.fontSize / 32.0;
            int by = ty + metrics.ascender;
            putText(frame, lines[i], Point(tx + 2, by + 2),
                    FONT_HERSHEY_SIMPLEX, scale, Scalar(0, 0, 0), 3, LINE_8);
            putText(frame, lines[i], Point(tx, by),
                    FONT_HERSHEY_SIMPLEX, scale, Scalar(255, 255, 255), 2, LINE_AA);
        }
    }
}

// ============================================================================
// 6. AsyncVideoWriter
// ============================================================================

class AsyncVideoWriter {
public:
    AsyncVideoWriter(const string& path, int fourcc, double fps, Size frameSize)
        : done_(false) {
        writer_.open(path, fourcc, fps, frameSize, true);
        if (!writer_.isOpened()) {
            writer_.open(path, VideoWriter::fourcc('a', 'v', 'c', '1'), fps, frameSize, true);
        }
        worker_ = thread(&AsyncVideoWriter::run, this);
    }

    ~AsyncVideoWriter() {
        {
            unique_lock<mutex> lock(mtx_);
            done_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        writer_.release();
    }

    bool isOpened() const {
        return writer_.isOpened();
    }

    void write(Mat frame) {
        unique_lock<mutex> lock(mtx_);
        cv_.wait(lock, [this]() { return queue_.size() < MAX_QUEUE_SIZE || done_; });
        if (!done_) {
            queue_.push(std::move(frame));
            cv_.notify_one();
        }
    }

private:
    static constexpr size_t MAX_QUEUE_SIZE = 12;
    VideoWriter writer_;
    queue<Mat> queue_;
    mutex mtx_;
    condition_variable cv_;
    bool done_;
    thread worker_;

    void run() {
        while (true) {
            Mat frame;
            {
                unique_lock<mutex> lock(mtx_);
                cv_.wait(lock, [this]() { return !queue_.empty() || done_; });
                if (queue_.empty() && done_) {
                    break;
                }
                frame = std::move(queue_.front());
                queue_.pop();
                cv_.notify_one();
            }
            if (!frame.empty() && writer_.isOpened()) {
                writer_.write(frame);
            }
        }
    }
};

// ============================================================================
// 7. Drawing helpers (Optimized for High-Throughput Rendering)
// ============================================================================

void drawRoundRect(Mat& img, Rect rect, int r, const Scalar& color, int thickness = -1) {
    int x = rect.x, y = rect.y, w = rect.width, h = rect.height;
    r = std::clamp(r, 1, min(w, h) / 2);

    if (thickness < 0) {
        rectangle(img, Rect(x + r, y, w - 2 * r, h), color, FILLED, LINE_8);
        rectangle(img, Rect(x, y + r, r, h - 2 * r), color, FILLED, LINE_8);
        rectangle(img, Rect(x + w - r, y + r, r, h - 2 * r), color, FILLED, LINE_8);
        circle(img, Point(x + r, y + r), r, color, FILLED, LINE_8);
        circle(img, Point(x + w - r, y + r), r, color, FILLED, LINE_8);
        circle(img, Point(x + r, y + h - r), r, color, FILLED, LINE_8);
        circle(img, Point(x + w - r, y + h - r), r, color, FILLED, LINE_8);
    } else {
        line(img, Point(x + r, y), Point(x + w - r, y), color, thickness, LINE_8);
        line(img, Point(x + r, y + h), Point(x + w - r, y + h), color, thickness, LINE_8);
        line(img, Point(x, y + r), Point(x, y + h - r), color, thickness, LINE_8);
        line(img, Point(x + w, y + r), Point(x + w, y + h - r), color, thickness, LINE_8);
        ellipse(img, Point(x + r, y + r), Size(r, r), 180, 0, 90, color, thickness, LINE_8);
        ellipse(img, Point(x + w - r, y + r), Size(r, r), 270, 0, 90, color, thickness, LINE_8);
        ellipse(img, Point(x + w - r, y + h - r), Size(r, r), 0, 0, 90, color, thickness, LINE_8);
        ellipse(img, Point(x + r, y + h - r), Size(r, r), 90, 0, 90, color, thickness, LINE_8);
    }
}

void drawPillCard(Mat& frame, int cx, int cy, int w, int h, int r,
                  const string& label, const string& value,
                  Scalar bgColor, Scalar borderColor,
                  Scalar labelColor, Scalar valueColor) {
    int x = cx - w / 2;
    int y = cy - h / 2;
    Rect roiRect(x, y, w, h);
    roiRect &= Rect(0, 0, frame.cols, frame.rows);
    if (roiRect.width <= 0 || roiRect.height <= 0) return;

    // 1. In-place SIMD ROI blend (80% dark bgColor, 0 heap allocations!)
    Mat roi = frame(roiRect);
    roi *= 0.20;
    roi += bgColor * 0.80;

    // 2. Border
    drawRoundRect(frame, roiRect, r, borderColor, 2);

    // 3. Small centered label on top (FONT_HERSHEY_SIMPLEX, 0.42)
    int baseLabel = 0;
    Size lblSz = getTextSize(label, FONT_HERSHEY_SIMPLEX, 0.42, 1, &baseLabel);
    int lblX = cx - lblSz.width / 2;
    int lblY = y + 18;
    putText(frame, label, Point(lblX, lblY), FONT_HERSHEY_SIMPLEX, 0.42, labelColor, 1, LINE_AA);

    // 4. Large centered value on bottom (scale 0.85, thickness 2)
    double valScale = 0.85;
    int baseVal = 0;
    Size valSz = getTextSize(value, FONT_HERSHEY_SIMPLEX, valScale, 2, &baseVal);
    if (valSz.width > w - 14) {
        valScale = 0.68;
        valSz = getTextSize(value, FONT_HERSHEY_SIMPLEX, valScale, 2, &baseVal);
    }
    int valX = cx - valSz.width / 2;
    int valY = y + h - 11;
    putText(frame, value, Point(valX, valY), FONT_HERSHEY_SIMPLEX, valScale, valueColor, 2, LINE_AA);
}

string getStageName(int stage) {
    switch ((stage - 1) % 5) {
        case 0: return "SHALLOW CORAL REEF";
        case 1: return "EMERALD KELP FOREST";
        case 2: return "SUNSET LAGOON";
        case 3: return "TWILIGHT DEEP ZONE";
        default: return "ABYSSAL TRENCH";
    }
}

void drawOceanAtmosphere(Mat& frame, int frameId, double cycle, int stage,
                         const vector<BgBubble>& bgBubbles, int groundY) {
    int w = frame.cols;
    int surfaceY = 82;

    // 1. Single polyline call for water surface shimmer wave
    vector<Point> wavePts;
    wavePts.reserve(w / 16 + 2);
    for (int x = 0; x <= w; x += 16) {
        int wy = surfaceY + (int)(sin(x * 0.025f + frameId * 0.08f) * 4.0f);
        wavePts.emplace_back(x, wy);
    }
    polylines(frame, wavePts, false, Scalar(255, 240, 190), 2, LINE_8);

    // 2. Deep sea glowing plankton when in deep/night cycle
    if (cycle >= 0.65) {
        for (int i = 0; i < 24; ++i) {
            int sx = (i * 197 + frameId * ((i % 3) + 1)) % w;
            int sy = 100 + ((i * 131) % max(100, groundY - 130));
            double pulse = 0.5 + 0.5 * sin(frameId * 0.1f + i * 1.7f);
            int br = (int)(120 + 135 * pulse);
            Scalar glowCol = (stage % 2 == 0) ? Scalar(br, br, 80) : Scalar(br, 180, 255);
            circle(frame, Point(sx, sy), (i % 3 == 0) ? 3 : 2, glowCol, FILLED, LINE_8);
        }
    }

    // 3. Background rising bubbles
    for (const auto& b : bgBubbles) {
        int bx = (int)(b.x + sin(frameId * 0.05f + b.phase) * 8.0f);
        int by = (int)b.y;
        if (by > surfaceY && by < groundY) {
            circle(frame, Point(bx, by), (int)b.r, Scalar(235, 220, 175), 1, LINE_8);
        }
    }
}

void drawCoralAndKelp(Mat& frame, const vector<CoralProp>& corals, int groundY, int frameId) {
    for (const auto& c : corals) {
        int baseX = (int)c.x;
        if (baseX < -80 || baseX > frame.cols + 80) continue;

        if (c.style == 0) {
            int segments = 6;
            int segH = c.h / segments;
            Point prev(baseX, groundY);
            for (int s = 1; s <= segments; ++s) {
                float sway = sin(frameId * 0.06f + c.phase + s * 0.45f) * (s * 2.5f);
                Point curr(baseX + (int)sway, groundY - s * segH);
                int thick = max(3, c.w / 4 - s / 2);
                line(frame, prev, curr, c.color, thick, LINE_8);
                prev = curr;
            }
        } else if (c.style == 1) {
            Point root(baseX, groundY);
            Point top(baseX, groundY - c.h);
            line(frame, root, top, c.color, 8, LINE_8);
            line(frame, Point(baseX, groundY - c.h / 2),
                 Point(baseX - c.w / 2, groundY - (c.h * 3) / 4), c.color, 6, LINE_8);
            line(frame, Point(baseX, groundY - (c.h * 2) / 5),
                 Point(baseX + c.w / 2, groundY - (c.h * 4) / 5), c.color, 6, LINE_8);
            circle(frame, top, 6, c.color + Scalar(30, 30, 30), FILLED, LINE_8);
        } else {
            ellipse(frame, Point(baseX, groundY - c.h / 2), Size(c.w / 3, c.h / 2),
                    0, 0, 360, c.color, FILLED, LINE_8);
            ellipse(frame, Point(baseX, groundY - c.h), Size(c.w / 3, 6),
                    0, 0, 360, c.color * 0.6, FILLED, LINE_8);
        }
    }
}

void drawStyledFish(Mat& frame, float x, float y, int w, int h,
                    Scalar bodyColor, Scalar finColor, Scalar bellyColor,
                    bool facingRight, int frameId, float phase,
                    float mouthOpenRatio, bool isPredatorAlert, bool isPlayer, int tierBadge) {
    int cx = (int)(x + w * 0.5f);
    int cy = (int)(y + h * 0.5f);
    int rx = max(8, w / 2);
    int ry = max(6, h / 2);
    int dir = facingRight ? 1 : -1;

    // 1. Wagging tail fin
    float wag = sin(frameId * 0.32f + phase) * (ry * 0.35f);
    int tailBaseX = cx - dir * (int)(rx * 0.82f);
    int tailTipX  = cx - dir * (int)(rx * 1.48f);
    Point tailPts[4] = {
        Point(tailBaseX, cy),
        Point(tailTipX, cy - (int)(ry * 0.85f + wag)),
        Point(tailBaseX - dir * (int)(rx * 0.25f), cy),
        Point(tailTipX, cy + (int)(ry * 0.85f - wag))
    };
    fillConvexPoly(frame, tailPts, 4, finColor, LINE_8);

    // 2. Dorsal fin & pelvic fin
    Point dorsalPts[3] = {
        Point(cx - dir * (int)(rx * 0.35f), cy - (int)(ry * 0.80f)),
        Point(cx + dir * (int)(rx * 0.20f), cy - (int)(ry * 0.85f)),
        Point(cx - dir * (int)(rx * 0.25f), cy - (int)(ry * 1.40f))
    };
    fillConvexPoly(frame, dorsalPts, 3, finColor, LINE_8);

    Point pelvicPts[3] = {
        Point(cx - dir * (int)(rx * 0.20f), cy + (int)(ry * 0.80f)),
        Point(cx + dir * (int)(rx * 0.15f), cy + (int)(ry * 0.80f)),
        Point(cx - dir * (int)(rx * 0.15f), cy + (int)(ry * 1.25f))
    };
    fillConvexPoly(frame, pelvicPts, 3, finColor, LINE_8);

    // 3. Main fish body & lighter belly
    ellipse(frame, Point(cx, cy), Size(rx, ry), 0, 0, 360, bodyColor, FILLED, LINE_8);
    ellipse(frame, Point(cx, cy + (int)(ry * 0.22f)), Size((int)(rx * 0.82f), (int)(ry * 0.58f)),
            0, 0, 180, bellyColor, FILLED, LINE_8);

    for (int s = -1; s <= 1; ++s) {
        int sx = cx + s * (int)(rx * 0.32f);
        line(frame, Point(sx, cy - (int)(ry * 0.72f)),
             Point(sx - dir * 4, cy + (int)(ry * 0.55f)),
             finColor * 0.85, max(2, rx / 10), LINE_8);
    }
    ellipse(frame, Point(cx, cy), Size(rx, ry), 0, 0, 360, Scalar(25, 30, 35), 2, LINE_AA);

    // 4. Mouth
    int mouthX = cx + dir * (int)(rx * 0.88f);
    int mouthY = cy + (int)(ry * 0.12f);
    if (mouthOpenRatio > 0.05f) {
        int mOpen = max(3, (int)(ry * 0.48f * mouthOpenRatio));
        Point mPts[3] = {
            Point(cx + dir * (int)(rx * 0.45f), mouthY),
            Point(mouthX + dir * 4, mouthY - mOpen),
            Point(mouthX + dir * 4, mouthY + mOpen)
        };
        fillConvexPoly(frame, mPts, 3, Scalar(35, 25, 90), LINE_8);
    } else {
        line(frame, Point(mouthX, mouthY), Point(mouthX - dir * (int)(rx * 0.25f), mouthY + 2),
             Scalar(25, 25, 25), 2, LINE_8);
    }

    // 5. Pectoral side fin
    float sideFinAngle = sin(frameId * 0.25f + phase) * 15.0f;
    ellipse(frame, Point(cx + dir * (int)(rx * 0.05f), cy + (int)(ry * 0.25f)),
            Size(max(4, rx / 3), max(3, ry / 4)),
            sideFinAngle, 0, 360, finColor, FILLED, LINE_8);

    // 6. Eye
    int eyeX = cx + dir * (int)(rx * 0.48f);
    int eyeY = cy - (int)(ry * 0.24f);
    int eyeR = max(4, ry / 4);
    circle(frame, Point(eyeX, eyeY), eyeR + 1, Scalar(255, 255, 255), FILLED, LINE_8);
    circle(frame, Point(eyeX + dir * 1, eyeY), max(2, eyeR / 2 + 1), Scalar(15, 15, 15), FILLED, LINE_8);

    if (isPredatorAlert) {
        line(frame, Point(eyeX - dir * (eyeR + 2), eyeY - eyeR - 3),
             Point(eyeX + dir * (eyeR + 2), eyeY - eyeR + 1),
             Scalar(30, 30, 240), 2, LINE_8);
        circle(frame, Point(cx, (int)y - 10), 7, Scalar(30, 30, 230), FILLED, LINE_8);
        putText(frame, "!", Point(cx - 3, (int)y - 5), FONT_HERSHEY_SIMPLEX, 0.42, Scalar(255, 255, 255), 2, LINE_8);
    }

    if (isPlayer) {
        int crX = cx + dir * (int)(rx * 0.15f);
        int crY = (int)y - 4;
        Point crown[5] = {
            Point(crX - 10, crY),
            Point(crX - 12, crY - 10),
            Point(crX,      crY - 5),
            Point(crX + 12, crY - 10),
            Point(crX + 10, crY)
        };
        fillConvexPoly(frame, crown, 5, Scalar(30, 215, 255), LINE_8);

        char tBuf[16];
        snprintf(tBuf, sizeof(tBuf), "Lv.%d", tierBadge);
        putText(frame, tBuf, Point(cx - 14, cy + 4), FONT_HERSHEY_SIMPLEX, 0.40,
                Scalar(20, 20, 20), 3, LINE_8);
        putText(frame, tBuf, Point(cx - 14, cy + 4), FONT_HERSHEY_SIMPLEX, 0.40,
                Scalar(255, 255, 255), 1, LINE_AA);
    }
}

void drawObstacles(Mat& frame, const vector<Obstacle>& obstacles, int playerTier, int frameId) {
    for (const auto& ob : obstacles) {
        if (ob.eaten) continue;
        int cx = (int)(ob.x + ob.w * 0.5f);
        int cy = (int)(ob.y + ob.h * 0.5f);

        if (ob.type == ENT_SEA_MINE) {
            int r = min(ob.w, ob.h) / 2 - 4;
            line(frame, Point(cx, cy + r), Point(cx, min(frame.rows - 45, cy + r + 80)),
                 Scalar(90, 95, 100), 2, LINE_8);
            for (int a = 0; a < 8; ++a) {
                double ang = a * CV_PI / 4.0 + frameId * 0.02;
                Point p1(cx + (int)(cos(ang) * (r - 2)), cy + (int)(sin(ang) * (r - 2)));
                Point p2(cx + (int)(cos(ang) * (r + 9)), cy + (int)(sin(ang) * (r + 9)));
                line(frame, p1, p2, Scalar(70, 75, 85), 4, LINE_8);
                circle(frame, p2, 3, Scalar(50, 50, 220), FILLED, LINE_8);
            }
            circle(frame, Point(cx, cy), r, Scalar(65, 70, 78), FILLED, LINE_8);
            circle(frame, Point(cx - r / 3, cy - r / 3), r / 3, Scalar(120, 125, 135), FILLED, LINE_8);
            circle(frame, Point(cx, cy), r, Scalar(30, 35, 40), 2, LINE_AA);
            if ((frameId / 8) % 2 == 0) {
                circle(frame, Point(cx, cy), 5, Scalar(40, 40, 255), FILLED, LINE_8);
            }
        } else if (ob.type == ENT_JELLYFISH) {
            int rx = ob.w / 2;
            int ry = ob.h / 3;
            for (int t = -2; t <= 2; ++t) {
                Point prev(cx + t * (rx / 3), cy);
                for (int s = 1; s <= 3; ++s) {
                    int sw = (int)(sin(frameId * 0.18f + ob.phase + s * 0.7f + t) * 6.0f);
                    Point curr(cx + t * (rx / 3) + sw, cy + s * 11);
                    line(frame, prev, curr, Scalar(255, 140, 240), 2, LINE_8);
                    prev = curr;
                }
            }
            ellipse(frame, Point(cx, cy), Size(rx, ry), 0, 180, 360, Scalar(235, 110, 220), FILLED, LINE_8);
            ellipse(frame, Point(cx, cy), Size(rx - 4, ry - 4), 0, 180, 360, Scalar(255, 200, 250), FILLED, LINE_8);
        } else if (ob.type == ENT_GOLDEN_PEARL) {
            int r = min(ob.w, ob.h) / 2;
            double pulse = 0.5 + 0.5 * sin(frameId * 0.2 + ob.phase);
            circle(frame, Point(cx, cy), r + (int)(4 * pulse), Scalar(120, 240, 255), 1, LINE_8);
            circle(frame, Point(cx, cy), r, Scalar(40, 215, 255), FILLED, LINE_8);
            circle(frame, Point(cx - r / 3, cy - r / 3), max(2, r / 3), Scalar(240, 255, 255), FILLED, LINE_8);
        } else {
            bool isPredator = (ob.sizeTier > playerTier);
            Scalar bodyCol, finCol, bellyCol;

            if (ob.type == ENT_TINY_FISH) {
                if (ob.colorTheme % 2 == 0) {
                    bodyCol  = Scalar(40, 140, 255);
                    finCol   = Scalar(255, 255, 255);
                    bellyCol = Scalar(160, 215, 255);
                } else {
                    bodyCol  = Scalar(240, 210, 40);
                    finCol   = Scalar(180, 255, 120);
                    bellyCol = Scalar(255, 245, 190);
                }
            } else if (ob.type == ENT_MEDIUM_FISH) {
                bodyCol  = Scalar(180, 95, 45);
                finCol   = Scalar(60, 200, 245);
                bellyCol = Scalar(220, 185, 150);
            } else if (ob.type == ENT_LARGE_FISH) {
                bodyCol  = Scalar(130, 60, 175);
                finCol   = Scalar(80, 40, 230);
                bellyCol = Scalar(195, 165, 220);
            } else {
                bodyCol  = Scalar(115, 105, 95);
                finCol   = Scalar(75, 70, 65);
                bellyCol = Scalar(215, 215, 215);
            }

            float mouthOpen = isPredator ? (0.35f + 0.25f * sin(frameId * 0.25f + ob.phase)) : 0.0f;
            drawStyledFish(frame, ob.x, ob.y, ob.w, ob.h,
                           bodyCol, finCol, bellyCol,
                           false, frameId, ob.phase,
                           mouthOpen, isPredator, false, ob.sizeTier);
        }
    }
}

void drawOceanFloor(Mat& frame, int groundY, int frameId, float scrollX, double cycle) {
    int w = frame.cols;
    int h = frame.rows;

    Scalar sandTop = lerpScalar(Scalar(115, 195, 235), Scalar(65, 95, 125), cycle);
    Scalar sandBot = lerpScalar(Scalar(75, 145, 190),  Scalar(35, 55, 80),  cycle);

    vector<Point> dunePts;
    dunePts.reserve(w / 16 + 2);
    for (int x = 0; x <= w; x += 16) {
        int dy = groundY + (int)(sin((x + scrollX * 0.5f) * 0.03f + frameId * 0.04f) * 3.0f);
        dunePts.emplace_back(x, dy);
    }
    polylines(frame, dunePts, false, sandTop * 1.15, 3, LINE_8);

    for (int i = 0; i < 18; ++i) {
        int px = (int)fmod((i * 97.0f - scrollX * 0.8f), (float)w);
        if (px < 0) px += w;
        int py = groundY + 12 + (i * 19) % max(10, (h - groundY - 18));
        int pr = 4 + (i % 5);
        ellipse(frame, Point(px, py), Size(pr + 2, pr - 1), 0, 0, 360, sandBot * 0.85, FILLED, LINE_8);
    }
}

void drawStageProgressBar(Mat& frame, int stage, int playerTier, int eatenInStage, int stageTarget) {
    int barW = 460;
    int barH = 22;
    int barX = (frame.cols - barW) / 2;
    int barY = 66;

    drawRoundRect(frame, Rect(barX, barY, barW, barH), 10, Scalar(30, 35, 45), FILLED);

    float ratio = std::clamp((float)eatenInStage / max(1, stageTarget), 0.0f, 1.0f);
    int fillW = max(16, (int)(barW * ratio));
    Scalar fillCol = (playerTier == 1) ? Scalar(50, 200, 255)
                   : (playerTier == 2) ? Scalar(80, 235, 120)
                                       : Scalar(220, 120, 255);
    drawRoundRect(frame, Rect(barX, barY, fillW, barH), 10, fillCol, FILLED);
    rectangle(frame, Rect(barX, barY, barW, barH), Scalar(180, 200, 220), 1, LINE_8);

    int m1 = barX + barW / 3;
    int m2 = barX + (barW * 2) / 3;
    line(frame, Point(m1, barY + 2), Point(m1, barY + barH - 2), Scalar(255, 255, 255), 1, LINE_8);
    line(frame, Point(m2, barY + 2), Point(m2, barY + barH - 2), Scalar(255, 255, 255), 1, LINE_8);

    char infoBuf[128];
    string stName = getStageName(stage);
    snprintf(infoBuf, sizeof(infoBuf), "STAGE %d: %s  |  FISH TIER %d/3  (%d/%d)",
             stage, stName.c_str(), playerTier, eatenInStage, stageTarget);

    int base = 0;
    Size tSz = getTextSize(infoBuf, FONT_HERSHEY_SIMPLEX, 0.42, 1, &base);
    int tx = (frame.cols - tSz.width) / 2;
    int ty = barY + 15;
    putText(frame, infoBuf, Point(tx + 1, ty + 1), FONT_HERSHEY_SIMPLEX, 0.42, Scalar(0, 0, 0), 2, LINE_8);
    putText(frame, infoBuf, Point(tx, ty), FONT_HERSHEY_SIMPLEX, 0.42, Scalar(255, 255, 255), 1, LINE_AA);
}

// Render 1 FrameSnapshot hoan chinh (Chay song song tren RenderThreadPool)
void renderSnapshotToFrame(Mat& frame, const FrameSnapshot& st,
                           const Config& cfg, const BackgroundManager* bgMgr,
                           const vector<Subtitle>& subtitles) {
    int groundY = cfg.height - 64;

    // 1. Background (Pre-baked Ocean Depth + Sun Rays + Seabed Gradient)
    bgMgr->render(frame, st.frameId, st.cycle);

    // 2. Far atmosphere effects
    drawOceanAtmosphere(frame, st.frameId, st.cycle, st.stage, st.bgBubbles, groundY);
    drawCoralAndKelp(frame, st.corals, groundY, st.frameId);

    // 3. Obstacles
    drawObstacles(frame, st.obstacles, st.playerTier, st.frameId);

    // 4. Particles / bubble trail
    for (const auto& p : st.particles) {
        int br = (int)(220 * std::clamp(p.alpha, 0.0f, 1.0f));
        Scalar c(min(255, (int)p.color[0] * br / 220),
                 min(255, (int)p.color[1] * br / 220),
                 min(255, (int)p.color[2] * br / 220));
        int rad = max(1, (int)(p.size * p.alpha));
        if (p.isBubble) {
            circle(frame, Point((int)p.x, (int)p.y), rad, c, 1, LINE_8);
        } else {
            circle(frame, Point((int)p.x, (int)p.y), rad, c, FILLED, LINE_8);
        }
    }

    // 5. Main Player Fish
    if (st.invincibleTimer == 0 || (st.frameId % 4 < 2)) {
        Scalar pBody  = (st.playerTier == 1) ? Scalar(35, 185, 255)
                      : (st.playerTier == 2) ? Scalar(60, 225, 110)
                                             : Scalar(210, 95, 255);
        Scalar pFin   = (st.playerTier == 1) ? Scalar(20, 95, 245)
                      : (st.playerTier == 2) ? Scalar(30, 150, 255)
                                             : Scalar(50, 220, 255);
        Scalar pBelly = Scalar(190, 245, 255);
        float mOpen = (st.mouthOpenTimer > 0) ? (st.mouthOpenTimer / 14.0f) : 0.1f;

        drawStyledFish(frame, st.playerX, st.playerY, st.playerW, st.playerH,
                       pBody, pFin, pBelly,
                       true, st.frameId, 0.0f,
                       mOpen, false, true, st.playerTier);
    }

    // 6. Near effects (ScorePop & Milestone Flash)
    for (const auto& sp : st.scorePops) {
        char sBuf[32];
        snprintf(sBuf, sizeof(sBuf), "+%d", sp.value);
        double sc = 0.72;
        Scalar col = sp.color * std::clamp(sp.alpha, 0.0f, 1.0f);
        putText(frame, sBuf, Point(sp.x + 2, (int)sp.y + 2),
                FONT_HERSHEY_SIMPLEX, sc, Scalar(0, 0, 0), 3, LINE_8);
        putText(frame, sBuf, Point(sp.x, (int)sp.y),
                FONT_HERSHEY_SIMPLEX, sc, col, 2, LINE_AA);
    }

    if (st.milestoneTimer > 0) {
        double alpha = std::clamp(st.milestoneTimer / 30.0, 0.0, 1.0);
        int base = 0;
        double sc = 1.05;
        Size mSz = getTextSize(st.milestoneText, FONT_HERSHEY_SIMPLEX, sc, 3, &base);
        int mx = (cfg.width - mSz.width) / 2;
        int my = cfg.height / 3;
        Scalar flashCol(50 * alpha, 230 * alpha, 255 * alpha);
        putText(frame, st.milestoneText, Point(mx + 2, my + 2),
                FONT_HERSHEY_SIMPLEX, sc, Scalar(0, 0, 0), 5, LINE_8);
        putText(frame, st.milestoneText, Point(mx, my),
                FONT_HERSHEY_SIMPLEX, sc, flashCol, 3, LINE_AA);
    }

    // 7. Ground details
    drawOceanFloor(frame, groundY, st.frameId, st.scrollX, st.cycle);

    // 8. HUD (Pill Cards + Stage Progress Bar)
    int cardW  = 195;
    int cardH  = 50;
    int cardY  = 34;
    int gapX   = 215;
    int startX = cfg.width / 2 - gapX * 2;

    char scoreBuf[32], bestBuf[32], stageBuf[32], timeBuf[32], livesBuf[32];
    snprintf(scoreBuf, sizeof(scoreBuf), "%d", st.score);
    snprintf(bestBuf,  sizeof(bestBuf),  "%d", st.bestScore);
    snprintf(stageBuf, sizeof(stageBuf), "STG %d (T%d)", st.stage, st.playerTier);

    int elapsedSec = st.frameId / cfg.fps;
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", elapsedSec / 60, elapsedSec % 60);
    snprintf(livesBuf, sizeof(livesBuf), "%d / %d", st.lives, cfg.lives);

    drawPillCard(frame, startX + 0 * gapX, cardY, cardW, cardH, 12,
                 "SCORE", scoreBuf,
                 Scalar(35, 30, 25), Scalar(30, 215, 255),
                 Scalar(190, 190, 190), Scalar(30, 215, 255));

    drawPillCard(frame, startX + 1 * gapX, cardY, cardW, cardH, 12,
                 "BEST", bestBuf,
                 Scalar(35, 35, 25), Scalar(220, 220, 50),
                 Scalar(190, 190, 190), Scalar(220, 220, 50));

    drawPillCard(frame, startX + 2 * gapX, cardY, cardW, cardH, 12,
                 "STAGE / SIZE", stageBuf,
                 Scalar(40, 30, 35), Scalar(255, 200, 120),
                 Scalar(190, 190, 190), Scalar(255, 255, 255));

    drawPillCard(frame, startX + 3 * gapX, cardY, cardW, cardH, 12,
                 "TIME", timeBuf,
                 Scalar(30, 30, 30), Scalar(150, 150, 150),
                 Scalar(180, 180, 180), Scalar(180, 180, 180));

    Scalar lifeCol = (st.lives > 1) ? Scalar(90, 235, 90) : Scalar(60, 60, 255);
    drawPillCard(frame, startX + 4 * gapX, cardY, cardW, cardH, 12,
                 "LIVES", livesBuf,
                 Scalar(25, 35, 25), lifeCol,
                 Scalar(190, 190, 190), lifeCol);

    drawStageProgressBar(frame, st.stage, st.playerTier, st.eatenInStage, st.stageTarget);

    // 9. Subtitle
    if (st.subIdx >= 0 && st.subIdx < (int)subtitles.size()) {
        drawSubtitle(frame, subtitles[st.subIdx].text, cfg);
    }

    // 10. Overlay Message (GAME OVER / OUCH)
    if (st.overlayTimer > 0) {
        int ovW = 540;
        int ovH = 140;
        Rect ovRect((cfg.width - ovW) / 2, (cfg.height - ovH) / 2, ovW, ovH);
        ovRect &= Rect(0, 0, cfg.width, cfg.height);
        Mat roi = frame(ovRect);
        roi *= 0.22;
        roi += Scalar(15 * 0.78, 15 * 0.78, 20 * 0.78);
        drawRoundRect(frame, ovRect, 16, st.overlayColor, 2);

        int b1 = 0, b2 = 0;
        Size s1 = getTextSize(st.overlayTitle, FONT_HERSHEY_SIMPLEX, 1.45, 3, &b1);
        Size s2 = getTextSize(st.overlaySubtitle, FONT_HERSHEY_SIMPLEX, 0.72, 2, &b2);

        putText(frame, st.overlayTitle,
                Point((cfg.width - s1.width) / 2, ovRect.y + 62),
                FONT_HERSHEY_SIMPLEX, 1.45, st.overlayColor, 3, LINE_AA);
        putText(frame, st.overlaySubtitle,
                Point((cfg.width - s2.width) / 2, ovRect.y + 108),
                FONT_HERSHEY_SIMPLEX, 0.72, Scalar(215, 215, 215), 2, LINE_AA);
    }
}

// ============================================================================
// 8. AI Controller
// ============================================================================

struct AIDecision {
    float targetVy    = 0.0f;
    bool  triggerDash = false;
};

AIDecision computeAIDecision(float playerX, float playerY, int playerW, int playerH,
                             int playerTier, float gameSpeed, int dashCooldown,
                             int surfaceY, int groundY,
                             const vector<Obstacle>& obstacles) {
    AIDecision dec;
    float playerCenterX = playerX + playerW * 0.5f;
    float playerCenterY = playerY + playerH * 0.5f;

    const Obstacle* nearestThreat = nullptr;
    float minThreatDist = 1e9f;

    const Obstacle* bestPrey = nullptr;
    float bestPreyScore = -1e9f;

    float minSafeY = surfaceY + 28.0f + playerH * 0.5f;
    float maxSafeY = groundY  - 28.0f - playerH * 0.5f;

    for (const auto& ob : obstacles) {
        if (ob.eaten) continue;
        float obRight = ob.x + ob.w;
        if (obRight < playerX - 20.0f) continue;

        float distX = ob.x - (playerX + playerW);
        float relSpeed = max(2.0f, gameSpeed + ob.vx);
        float approachTime = max(0.0f, distX / relSpeed);

        float futureObY = ob.y + ob.h * 0.5f;
        if (ob.waveAmp > 0.1f) {
            futureObY = ob.baseY + sin(ob.phase + approachTime * ob.waveFreq) * ob.waveAmp + ob.h * 0.5f;
        }

        bool isThreat = (ob.type == ENT_SEA_MINE || ob.type == ENT_JELLYFISH ||
                         ob.type == ENT_APEX_SHARK || ob.sizeTier > playerTier);

        if (isThreat) {
            if (distX < 380.0f && distX < minThreatDist) {
                minThreatDist = distX;
                nearestThreat = &ob;
            }
        } else {
            if (distX < 460.0f) {
                bool nearHazard = false;
                for (const auto& other : obstacles) {
                    if (other.eaten || &other == &ob) continue;
                    bool otherThreat = (other.type == ENT_SEA_MINE || other.type == ENT_JELLYFISH ||
                                        other.type == ENT_APEX_SHARK || other.sizeTier > playerTier);
                    if (otherThreat) {
                        float dx = fabs((other.x + other.w * 0.5f) - (ob.x + ob.w * 0.5f));
                        float dy = fabs((other.y + other.h * 0.5f) - futureObY);
                        if (dx < 115.0f && dy < 95.0f) {
                            nearHazard = true;
                            break;
                        }
                    }
                }
                if (!nearHazard) {
                    float valueBonus = (ob.type == ENT_GOLDEN_PEARL) ? 140.0f : (ob.sizeTier * 45.0f);
                    float distPenalty = fabs(distX) * 0.55f + fabs(futureObY - playerCenterY) * 0.75f;
                    float candidateScore = valueBonus - distPenalty;
                    if (candidateScore > bestPreyScore) {
                        bestPreyScore = candidateScore;
                        bestPrey = &ob;
                    }
                }
            }
        }
    }

    float desiredY = playerCenterY;

    if (nearestThreat != nullptr && minThreatDist < 240.0f) {
        float bestLaneY = playerCenterY;
        float bestSafety = -1e9f;
        for (int i = 0; i < 9; ++i) {
            float laneY = minSafeY + (maxSafeY - minSafeY) * (i / 8.0f);
            float minClearance = 1e9f;
            for (const auto& ob : obstacles) {
                if (ob.eaten) continue;
                bool isThreat = (ob.type == ENT_SEA_MINE || ob.type == ENT_JELLYFISH ||
                                 ob.type == ENT_APEX_SHARK || ob.sizeTier > playerTier);
                if (!isThreat) continue;
                float dx = (ob.x + ob.w * 0.5f) - playerCenterX;
                if (dx < -40.0f || dx > 320.0f) continue;
                float obCy = ob.y + ob.h * 0.5f;
                float clearance = fabs(laneY - obCy) - (ob.h * 0.5f + playerH * 0.5f);
                minClearance = min(minClearance, clearance);
            }
            float score = minClearance - fabs(laneY - playerCenterY) * 0.18f;
            if (score > bestSafety) {
                bestSafety = score;
                bestLaneY = laneY;
            }
        }
        desiredY = bestLaneY;

        if (minThreatDist < 110.0f && dashCooldown == 0 && fabs(desiredY - playerCenterY) > 25.0f) {
            dec.triggerDash = true;
        }
    } else if (bestPrey != nullptr) {
        float distX = max(0.0f, bestPrey->x - (playerX + playerW));
        float relSpeed = max(2.0f, gameSpeed + bestPrey->vx);
        float lookaheadFrames = min(20.0f, distX / relSpeed);
        float predY = bestPrey->y + bestPrey->h * 0.5f;
        if (bestPrey->waveAmp > 0.1f) {
            predY = bestPrey->baseY + sin(bestPrey->phase + lookaheadFrames * bestPrey->waveFreq) * bestPrey->waveAmp
                    + bestPrey->h * 0.5f;
        }
        desiredY = std::clamp(predY, minSafeY, maxSafeY);

        if (distX < 95.0f && fabs(desiredY - playerCenterY) < 28.0f && dashCooldown == 0) {
            dec.triggerDash = true;
        }
    } else {
        float midY = (minSafeY + maxSafeY) * 0.5f;
        desiredY = midY + sin(playerX * 0.05f) * 35.0f;
    }

    float dy = desiredY - playerCenterY;
    dec.targetVy = std::clamp(dy * 0.18f, -8.5f, 8.5f);
    return dec;
}

// ============================================================================
// 9. main()
// ============================================================================

int main(int argc, char* argv[]) {
    Config cfg;
    if (!parseArguments(argc, argv, cfg)) {
        return 0;
    }

    // Use 1 internal OpenCV thread per worker since RenderThreadPool parallelizes across frames
    setNumThreads(1);
    unsigned int actualSeed = (cfg.seed >= 0) ? (unsigned int)cfg.seed : (unsigned int)time(nullptr);
    mt19937 rng(actualSeed);

    initFreeType(cfg.fontFile);

    vector<Subtitle> subtitles = parseSRT(cfg.subtitleFile);
    int totalFrames = cfg.duration * cfg.fps;
    if (!subtitles.empty()) {
        double maxEndTime = 0.0;
        for (const auto& s : subtitles) {
            maxEndTime = max(maxEndTime, s.end);
        }
        if (maxEndTime > 0.0) {
            totalFrames = max(totalFrames, (int)ceil((maxEndTime + 1.5) * cfg.fps));
        }
    }

    vector<int> frameSub(totalFrames, -1);
    for (int f = 0; f < totalFrames; ++f) {
        double t = (double)f / cfg.fps;
        for (int s = 0; s < (int)subtitles.size(); ++s) {
            if (t >= subtitles[s].start && t <= subtitles[s].end) {
                frameSub[f] = s;
                break;
            }
        }
    }

    BackgroundManager* bgMgr = new BackgroundManager(cfg);

    int fourcc = VideoWriter::fourcc('m', 'p', '4', 'v');
    AsyncVideoWriter video(cfg.outputPath, fourcc, cfg.fps, Size(cfg.width, cfg.height));
    if (!video.isOpened()) {
        cerr << "[Error] Khong the mo file video output: " << cfg.outputPath << "\n";
        delete bgMgr;
        return 1;
    }

    // Game State Initialization
    int surfaceY = 82;
    int groundY  = cfg.height - 64;

    int   score           = 0;
    int   bestScore       = 0;
    int   lives           = cfg.lives;
    int   stage           = 1;
    int   playerTier      = 1;
    int   eatenInStage    = 0;
    int   stageTarget     = cfg.baseStageTarget;
    float gameSpeed       = cfg.baseGameSpeed;
    float scrollX         = 0.0f;

    float playerBaseW     = 62.0f;
    float playerBaseH     = 40.0f;
    float playerScale     = 1.0f;
    float playerX         = cfg.width * 0.18f;
    float playerY         = (surfaceY + groundY) * 0.5f;
    float playerVy        = 0.0f;
    int   dashCooldown    = 0;
    int   dashTimer       = 0;
    int   mouthOpenTimer  = 0;
    int   invincibleTimer = 0;

    int    overlayTimer    = 0;
    string overlayTitle    = "";
    string overlaySubtitle = "";
    Scalar overlayColor    = Scalar(60, 220, 255);

    int    milestoneTimer  = 45;
    string milestoneText   = "STAGE 1: SHALLOW CORAL REEF";

    vector<Obstacle>  obstacles;
    vector<Particle>  particles;
    vector<ScorePop>  scorePops;
    vector<CoralProp> corals;
    vector<BgBubble>  bgBubbles;

    uniform_real_distribution<float> xDist(0.0f, (float)cfg.width);
    uniform_real_distribution<float> yDist((float)surfaceY, (float)groundY);
    for (int i = 0; i < 24; ++i) {
        BgBubble b;
        b.x     = xDist(rng);
        b.y     = yDist(rng);
        b.speed = 0.8f + (i % 5) * 0.35f;
        b.r     = 2.0f + (i % 4) * 1.5f;
        b.phase = i * 0.7f;
        bgBubbles.push_back(b);
    }

    for (int i = 0; i < 14; ++i) {
        CoralProp c;
        c.x     = i * (cfg.width / 12.0f) + (i % 3) * 15.0f;
        c.w     = 28 + (i % 4) * 10;
        c.h     = 55 + (i % 5) * 22;
        c.style = i % 3;
        c.phase = i * 0.9f;
        if (c.style == 0) c.color = Scalar(75, 165, 55);
        else if (c.style == 1) c.color = Scalar(130, 95, 225);
        else c.color = Scalar(80, 175, 230);
        corals.push_back(c);
    }

    uniform_int_distribution<int> gapDist(220, 390);
    uniform_real_distribution<float> phaseDist(0.0f, 6.28f);

    int renderThreads = max(1, cfg.numThreads);
    RenderThreadPool pool(renderThreads);
    int batchSize     = max(8, renderThreads * 2);
    vector<FrameSnapshot> batchSnapshots(batchSize);
    vector<Mat>           batchFrames(batchSize);
    for (int b = 0; b < batchSize; ++b) {
        batchFrames[b] = Mat(cfg.height, cfg.width, CV_8UC3);
    }

    auto startTime = chrono::steady_clock::now();
    cout << "[BigFishGame] Bat dau render " << totalFrames << " frames ("
         << (totalFrames / cfg.fps) << "s @ " << cfg.fps << " FPS, "
         << renderThreads << " threads) -> " << cfg.outputPath << "\n";

    // ========================================================================
    // MAIN GAME LOOP: Batch [LOGIC] -> Multi-Threaded [RENDER] -> [WRITE]
    // ========================================================================
    for (int baseFrame = 0; baseFrame < totalFrames; baseFrame += batchSize) {
        int currentBatch = min(batchSize, totalFrames - baseFrame);

        // --------------------------------------------------------------------
        // [LOGIC] — Run deterministic physics & AI sequentially for batch
        // --------------------------------------------------------------------
        for (int b = 0; b < currentBatch; ++b) {
            int frameId = baseFrame + b;

            float targetScale = (playerTier == 1) ? 1.0f : (playerTier == 2 ? 1.32f : 1.65f);
            playerScale += (targetScale - playerScale) * 0.08f;
            int playerW = (int)round(playerBaseW * playerScale);
            int playerH = (int)round(playerBaseH * playerScale);

            gameSpeed = cfg.baseGameSpeed + (stage - 1) * 0.55f + min(2.5f, score * 0.004f);
            float effectiveSpeed = gameSpeed + (dashTimer > 0 ? 3.5f : 0.0f);
            scrollX += effectiveSpeed;

            if (dashCooldown > 0) dashCooldown--;
            if (dashTimer > 0) dashTimer--;
            if (mouthOpenTimer > 0) mouthOpenTimer--;
            if (invincibleTimer > 0) invincibleTimer--;
            if (overlayTimer > 0) overlayTimer--;
            if (milestoneTimer > 0) milestoneTimer--;

            AIDecision ai = computeAIDecision(playerX, playerY, playerW, playerH,
                                              playerTier, effectiveSpeed, dashCooldown,
                                              surfaceY, groundY, obstacles);
            if (ai.triggerDash && dashCooldown == 0) {
                dashTimer = 14;
                dashCooldown = 36;
                mouthOpenTimer = max(mouthOpenTimer, 12);
            }

            playerVy = playerVy * cfg.waterDrag + (ai.targetVy - playerVy) * 0.28f + cfg.gravity;
            playerVy = std::clamp(playerVy, -cfg.maxVerticalSpeed, cfg.maxVerticalSpeed);
            playerY += playerVy;

            float minPlayerY = (float)surfaceY + 14.0f;
            float maxPlayerY = (float)groundY - playerH - 10.0f;
            if (playerY < minPlayerY) { playerY = minPlayerY; playerVy = 0.0f; }
            if (playerY > maxPlayerY) { playerY = maxPlayerY; playerVy = 0.0f; }

            if (frameId % 3 == 0 || dashTimer > 0) {
                Particle p;
                p.x        = playerX + 6.0f;
                p.y        = playerY + playerH * 0.5f + (float)((rng() % 11) - 5);
                p.vx       = -effectiveSpeed * 0.45f - (rng() % 20) * 0.08f;
                p.vy       = -0.6f - (rng() % 15) * 0.08f;
                p.alpha    = 1.0f;
                p.size     = (dashTimer > 0) ? 5.0f : 3.5f;
                p.color    = (dashTimer > 0) ? Scalar(120, 245, 255) : Scalar(245, 230, 190);
                p.isBubble = true;
                particles.push_back(p);
            }

            for (auto& bb : bgBubbles) {
                bb.y -= bb.speed;
                if (bb.y < surfaceY + 4) {
                    bb.y = (float)groundY - 5.0f;
                    bb.x = xDist(rng);
                }
            }
            for (auto& c : corals) {
                c.x -= effectiveSpeed * 0.45f;
                if (c.x < -60.0f) {
                    c.x += cfg.width + 110.0f;
                }
            }

            float rightmostX = 0.0f;
            for (auto& ob : obstacles) {
                ob.x -= (effectiveSpeed + ob.vx);
                if (ob.waveAmp > 0.1f) {
                    ob.phase += ob.waveFreq;
                    ob.y = ob.baseY + sin(ob.phase) * ob.waveAmp;
                    ob.y = std::clamp(ob.y, (float)surfaceY + 18.0f, (float)groundY - ob.h - 14.0f);
                }
                rightmostX = max(rightmostX, ob.x + ob.w);
            }

            int spawnGap = (int)(gapDist(rng) * 0.55f + (150.0f / max(3.0f, gameSpeed)));
            if (obstacles.empty() || rightmostX < cfg.width - spawnGap) {
                Obstacle ob;
                ob.x     = (float)cfg.width + 40.0f + (rng() % 60);
                ob.phase = phaseDist(rng);
                ob.colorTheme = rng() % 4;

                int roll = rng() % 100;
                if (roll < 38) {
                    ob.type     = ENT_TINY_FISH;
                    ob.sizeTier = 1;
                    ob.w        = 44;
                    ob.h        = 28;
                    ob.vx       = 0.8f + (rng() % 15) * 0.1f;
                    ob.waveAmp  = 14.0f + (rng() % 18);
                    ob.waveFreq = 0.07f;
                } else if (roll < 64) {
                    ob.type     = ENT_MEDIUM_FISH;
                    ob.sizeTier = 2;
                    ob.w        = 74;
                    ob.h        = 48;
                    ob.vx       = 1.2f + (rng() % 15) * 0.1f;
                    ob.waveAmp  = 18.0f;
                    ob.waveFreq = 0.05f;
                } else if (roll < 80 && (stage >= 2 || playerTier >= 2 || score >= 30)) {
                    ob.type     = ENT_LARGE_FISH;
                    ob.sizeTier = 3;
                    ob.w        = 102;
                    ob.h        = 64;
                    ob.vx       = 1.5f + (rng() % 15) * 0.1f;
                    ob.waveAmp  = 12.0f;
                    ob.waveFreq = 0.04f;
                } else if (roll < 87) {
                    ob.type     = ENT_GOLDEN_PEARL;
                    ob.sizeTier = 0;
                    ob.w        = 28;
                    ob.h        = 28;
                    ob.vx       = 0.2f;
                    ob.waveAmp  = 10.0f;
                    ob.waveFreq = 0.08f;
                } else if (roll < 94) {
                    ob.type     = ENT_SEA_MINE;
                    ob.sizeTier = 99;
                    ob.w        = 54;
                    ob.h        = 54;
                    ob.vx       = 0.0f;
                    ob.waveAmp  = 8.0f;
                    ob.waveFreq = 0.04f;
                } else {
                    if (stage >= 2 && (rng() % 2 == 0)) {
                        ob.type     = ENT_APEX_SHARK;
                        ob.sizeTier = 4;
                        ob.w        = 136;
                        ob.h        = 82;
                        ob.vx       = 2.2f;
                        ob.waveAmp  = 8.0f;
                        ob.waveFreq = 0.03f;
                    } else {
                        ob.type     = ENT_JELLYFISH;
                        ob.sizeTier = 99;
                        ob.w        = 46;
                        ob.h        = 58;
                        ob.vx       = 0.5f;
                        ob.waveAmp  = 34.0f;
                        ob.waveFreq = 0.06f;
                    }
                }

                float minY = (float)surfaceY + 28.0f;
                float maxY = (float)groundY - ob.h - 28.0f;
                uniform_real_distribution<float> obYDist(minY, max(minY + 10.0f, maxY));
                ob.baseY = obYDist(rng);
                ob.y     = ob.baseY;
                obstacles.push_back(ob);
            }

            // Collision Detection (20% shrunk hitbox)
            int dLeft  = (int)(playerX + playerW * 0.15f);
            int dRight = (int)(playerX + playerW * 0.85f);
            int dTop   = (int)(playerY + playerH * 0.10f);
            int dBot   = (int)(playerY + playerH * 0.90f);

            bool tookDamageThisFrame = false;

            for (auto& ob : obstacles) {
                if (ob.eaten) continue;

                int oLeft  = (int)(ob.x + ob.w * 0.15f);
                int oRight = (int)(ob.x + ob.w * 0.85f);
                int oTop   = (int)(ob.y + ob.h * 0.10f);
                int oBot   = (int)(ob.y + ob.h * 0.90f);

                bool hit = (dRight > oLeft && dLeft < oRight && dBot > oTop && dTop < oBot);
                if (!hit) continue;

                bool isThreat = (ob.type == ENT_SEA_MINE || ob.type == ENT_JELLYFISH ||
                                 ob.type == ENT_APEX_SHARK || ob.sizeTier > playerTier);

                if (!isThreat) {
                    ob.eaten = true;
                    mouthOpenTimer = 14;

                    int pts = (ob.type == ENT_GOLDEN_PEARL) ? 25 : (ob.sizeTier * 10);
                    int oldScore = score;
                    score += pts;
                    bestScore = max(bestScore, score);
                    eatenInStage += (ob.type == ENT_GOLDEN_PEARL) ? 2 : 1;

                    ScorePop sp;
                    sp.x     = (int)(ob.x + ob.w * 0.5f);
                    sp.y     = ob.y;
                    sp.value = pts;
                    sp.alpha = 1.0f;
                    sp.color = (ob.type == ENT_GOLDEN_PEARL) ? Scalar(30, 235, 255) : Scalar(100, 255, 140);
                    scorePops.push_back(sp);

                    for (int k = 0; k < 8; ++k) {
                        Particle p;
                        p.x     = ob.x + ob.w * 0.5f;
                        p.y     = ob.y + ob.h * 0.5f;
                        float ang = (k / 8.0f) * 6.28318f;
                        float spd = 1.8f + (rng() % 25) * 0.1f;
                        p.vx    = cos(ang) * spd;
                        p.vy    = sin(ang) * spd - 0.8f;
                        p.alpha = 1.0f;
                        p.size  = 4.0f + (k % 3);
                        p.color = (ob.type == ENT_GOLDEN_PEARL) ? Scalar(60, 230, 255) : Scalar(150, 240, 255);
                        p.isBubble = false;
                        particles.push_back(p);
                    }

                    int newTier = 1;
                    if (eatenInStage >= (stageTarget * 2) / 3) newTier = 3;
                    else if (eatenInStage >= stageTarget / 3) newTier = 2;

                    if (newTier > playerTier) {
                        playerTier = newTier;
                        milestoneText  = (playerTier == 2) ? "GROWTH UP! MEDIUM FISH (TIER 2)"
                                                           : "MEGA GROWTH! GIANT FISH (TIER 3)";
                        milestoneTimer = 36;
                    }

                    if (eatenInStage >= stageTarget) {
                        stage++;
                        eatenInStage = 0;
                        playerTier   = 1;
                        stageTarget  = cfg.baseStageTarget + (stage - 1) * 3;
                        score       += 50;
                        bestScore    = max(bestScore, score);

                        char stBuf[128];
                        snprintf(stBuf, sizeof(stBuf), "STAGE %d CLEAR! -> %s",
                                 stage - 1, getStageName(stage).c_str());
                        milestoneText  = stBuf;
                        milestoneTimer = 50;

                        for (auto& rem : obstacles) {
                            if (rem.sizeTier > 1) rem.eaten = true;
                        }
                    } else if ((oldScore / 100) < (score / 100)) {
                        char msBuf[64];
                        snprintf(msBuf, sizeof(msBuf), "FRENZY MILESTONE %d PTS!", (score / 100) * 100);
                        milestoneText  = msBuf;
                        milestoneTimer = 30;
                    }
                } else if (invincibleTimer == 0) {
                    tookDamageThisFrame = true;
                    break;
                }
            }

            if (tookDamageThisFrame) {
                lives--;
                invincibleTimer = 45;
                if (lives <= 0) {
                    overlayTitle    = "GAME OVER";
                    char subBuf[96];
                    snprintf(subBuf, sizeof(subBuf), "Final Score: %d | Reached Stage %d", score, stage);
                    overlaySubtitle = subBuf;
                    overlayColor    = Scalar(50, 50, 255);
                    overlayTimer    = 2 * cfg.fps;

                    score        = 0;
                    stage        = 1;
                    playerTier   = 1;
                    eatenInStage = 0;
                    stageTarget  = cfg.baseStageTarget;
                    lives        = cfg.lives;
                    gameSpeed    = cfg.baseGameSpeed;
                    playerY      = (surfaceY + groundY) * 0.5f;
                    playerVy     = 0.0f;
                    obstacles.clear();
                } else {
                    overlayTitle    = "OUCH!";
                    char subBuf[96];
                    snprintf(subBuf, sizeof(subBuf), "Watch out for bigger fish & mines! (%d lives left)", lives);
                    overlaySubtitle = subBuf;
                    overlayColor    = Scalar(40, 180, 255);
                    overlayTimer    = 1 * cfg.fps;

                    playerY  = (surfaceY + groundY) * 0.5f;
                    playerVy = 0.0f;
                    obstacles.erase(
                        remove_if(obstacles.begin(), obstacles.end(),
                                  [&](const Obstacle& o) { return o.x < playerX + 420.0f; }),
                        obstacles.end()
                    );
                }
            }

            obstacles.erase(
                remove_if(obstacles.begin(), obstacles.end(),
                          [](const Obstacle& o) { return o.eaten || (o.x + o.w < -80.0f); }),
                obstacles.end()
            );

            for (auto& p : particles) {
                p.x     += p.vx;
                p.y     += p.vy;
                p.alpha -= 0.055f;
            }
            particles.erase(
                remove_if(particles.begin(), particles.end(),
                          [](const Particle& p) { return p.alpha <= 0.0f; }),
                particles.end()
            );

            for (auto& sp : scorePops) {
                sp.y     -= 1.5f;
                sp.alpha -= 0.025f;
            }
            scorePops.erase(
                remove_if(scorePops.begin(), scorePops.end(),
                          [](const ScorePop& sp) { return sp.alpha <= 0.0f; }),
                scorePops.end()
            );

            // Save snapshot for parallel rendering
            FrameSnapshot& snap   = batchSnapshots[b];
            snap.frameId          = frameId;
            snap.cycle            = fmod(score / 250.0 + (stage - 1) * 0.22, 1.0);
            snap.score            = score;
            snap.bestScore        = bestScore;
            snap.lives            = lives;
            snap.stage            = stage;
            snap.playerTier       = playerTier;
            snap.eatenInStage     = eatenInStage;
            snap.stageTarget      = stageTarget;
            snap.scrollX          = scrollX;
            snap.playerX          = playerX;
            snap.playerY          = playerY;
            snap.playerW          = playerW;
            snap.playerH          = playerH;
            snap.mouthOpenTimer   = mouthOpenTimer;
            snap.invincibleTimer  = invincibleTimer;
            snap.overlayTimer     = overlayTimer;
            snap.overlayTitle     = overlayTitle;
            snap.overlaySubtitle  = overlaySubtitle;
            snap.overlayColor     = overlayColor;
            snap.milestoneTimer   = milestoneTimer;
            snap.milestoneText    = milestoneText;
            snap.subIdx           = frameSub[frameId];
            snap.obstacles        = obstacles;
            snap.particles        = particles;
            snap.scorePops        = scorePops;
            snap.corals           = corals;
            snap.bgBubbles        = bgBubbles;
        }

        // --------------------------------------------------------------------
        // [RENDER] — Parallel multi-threaded rendering via persistent ThreadPool
        // --------------------------------------------------------------------
        pool.parallelFor(currentBatch, [&](int b) {
            renderSnapshotToFrame(batchFrames[b], batchSnapshots[b], cfg, bgMgr, subtitles);
        });

        // --------------------------------------------------------------------
        // [WRITE] — Push rendered frames in exact sequential order
        // --------------------------------------------------------------------
        for (int b = 0; b < currentBatch; ++b) {
            video.write(batchFrames[b].clone());
        }
    }

    delete bgMgr;

    auto endTime = chrono::steady_clock::now();
    double elapsedSec = chrono::duration<double>(endTime - startTime).count();
    cout << "[BigFishGame] Hoan tat! Render " << totalFrames << " frames trong "
         << elapsedSec << "s (" << (int)(totalFrames / max(0.001, elapsedSec))
         << " fps) -> " << cfg.outputPath << "\n";
    return 0;
}