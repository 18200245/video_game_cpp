#include <opencv2/opencv.hpp>
#include <opencv2/freetype.hpp>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <deque>
#include <random>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <dirent.h>

using namespace cv;
using namespace std;

// ============================================================================
// Structs & Constants
// ============================================================================

struct Subtitle {
    double start;
    double end;
    string text;
};

struct Config {
    string outputPath      = "flappy_bird.mp4";
    string subtitleFile    = "subtitles.srt";
    string fontFile        = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    string backgroundFile  = "";
    string backgroundFolder= "";
    int    bgChangeInterval= 300;
    int    fontSize        = 32;
    int    width           = 1280;
    int    height          = 720;
    int    fps             = 30;
    int    seed            = -1;
    double bgOpacity       = 1.0;
    bool   showGrid        = false;
    int    subtitleY       = -1;
    int    subtitleOffset  = -1;
    int    numThreads      = max(1, (int)thread::hardware_concurrency());

    // Flappy Bird specific
    float  gravity         = 0.5f;
    float  flapStrength    = -9.0f;
    float  pipeSpeed       = 4.5f;
    int    pipeGap         = 200;
    int    pipeWidth       = 80;
    int    pipeSpacing     = 380;  // horizontal distance between pipes
    int    birdX           = 200;
    int    birdRadius      = 22;
};

struct Pipe {
    float x;
    int   gapY;   // center of the gap
    bool  passed;
};

// ============================================================================
// Parse Arguments
// ============================================================================

void printUsage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "  -o, --output <path>        Output video (default: flappy_bird.mp4)\n"
         << "  -s, --subtitles <path>     Subtitle SRT file\n"
         << "  -b, --background <path>    Background image\n"
         << "  --bg-folder <path>         Folder with backgrounds\n"
         << "  --bg-interval <int>        Change BG every N frames (default: 300)\n"
         << "  --bg-opacity <0.0-1.0>     Background opacity (default: 1.0)\n"
         << "  --font <path>              Font file\n"
         << "  --font-size <int>          Font size (default: 32)\n"
         << "  -w, --width <int>          Video width (default: 1280)\n"
         << "  -h, --height <int>         Video height (default: 720)\n"
         << "  -f, --fps <int>            FPS (default: 30)\n"
         << "  --seed <int>               Random seed\n"
         << "  --gravity <float>          Gravity strength (default: 0.5)\n"
         << "  --flap <float>             Flap strength (default: -9.0)\n"
         << "  --pipe-speed <float>       Pipe speed (default: 4.5)\n"
         << "  --pipe-gap <int>           Gap between pipes (default: 200)\n"
         << "  --pipe-spacing <int>       Horizontal space between pipes (default: 380)\n"
         << "  --subtitle-y <int>         Subtitle Y margin from bottom\n"
         << "  --subtitle-offset <int>    Baseline offset trong box (default: fontSize)\n"
         << "  --threads <int>            Number of threads\n"
         << "  --help                     Show help\n";
}

Config parseArguments(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if      (arg=="-o"||arg=="--output")      { if(i+1<argc) cfg.outputPath       = argv[++i]; }
        else if (arg=="-s"||arg=="--subtitles")   { if(i+1<argc) cfg.subtitleFile     = argv[++i]; }
        else if (arg=="-b"||arg=="--background")  { if(i+1<argc) cfg.backgroundFile   = argv[++i]; }
        else if (arg=="--bg-folder")              { if(i+1<argc) cfg.backgroundFolder  = argv[++i]; }
        else if (arg=="--bg-interval")            { if(i+1<argc) cfg.bgChangeInterval  = stoi(argv[++i]); }
        else if (arg=="--bg-opacity")             { if(i+1<argc) cfg.bgOpacity         = stod(argv[++i]); }
        else if (arg=="--font")                   { if(i+1<argc) cfg.fontFile          = argv[++i]; }
        else if (arg=="--font-size")              { if(i+1<argc) cfg.fontSize          = stoi(argv[++i]); }
        else if (arg=="-w"||arg=="--width")       { if(i+1<argc) cfg.width             = stoi(argv[++i]); }
        else if (arg=="-h"||arg=="--height")      { if(i+1<argc) cfg.height            = stoi(argv[++i]); }
        else if (arg=="-f"||arg=="--fps")         { if(i+1<argc) cfg.fps               = stoi(argv[++i]); }
        else if (arg=="--seed")                   { if(i+1<argc) cfg.seed              = stoi(argv[++i]); }
        else if (arg=="--gravity")                { if(i+1<argc) cfg.gravity           = stof(argv[++i]); }
        else if (arg=="--flap")                   { if(i+1<argc) cfg.flapStrength      = stof(argv[++i]); }
        else if (arg=="--pipe-speed")             { if(i+1<argc) cfg.pipeSpeed         = stof(argv[++i]); }
        else if (arg=="--pipe-gap")               { if(i+1<argc) cfg.pipeGap           = stoi(argv[++i]); }
        else if (arg=="--pipe-spacing")           { if(i+1<argc) cfg.pipeSpacing       = stoi(argv[++i]); }
        else if (arg=="--subtitle-y")             { if(i+1<argc) cfg.subtitleY         = stoi(argv[++i]); }
        else if (arg=="--subtitle-offset")        { if(i+1<argc) cfg.subtitleOffset    = stoi(argv[++i]); }
        else if (arg=="--threads")                { if(i+1<argc) cfg.numThreads        = stoi(argv[++i]); }
        else if (arg=="--help")                   { printUsage(argv[0]); exit(0); }
        else { cerr << "Unknown argument: " << arg << endl; printUsage(argv[0]); exit(1); }
    }
    cfg.bgOpacity        = max(0.0, min(1.0, cfg.bgOpacity));
    cfg.bgChangeInterval = max(1, cfg.bgChangeInterval);
    cfg.numThreads       = max(1, cfg.numThreads);
    return cfg;
}

// ============================================================================
// Background Manager
// ============================================================================

class BackgroundManager {
public:
    BackgroundManager(const string& folder, int w, int h, double opacity, int seed)
        : width_(w), height_(h), opacity_(opacity), rng_(seed) {
        if (!folder.empty()) load(folder);
    }

    void load(const string& folder) {
        DIR* dir = opendir(folder.c_str());
        if (!dir) return;
        struct dirent* e;
        while ((e = readdir(dir)) != nullptr) {
            string name = e->d_name;
            if (name.size() > 4) {
                string ext = name.substr(name.size()-4);
                transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext==".jpg"||ext==".png"||ext==".bmp"||ext==".jpeg")
                    imgs_.push_back(folder+"/"+name);
            }
        }
        closedir(dir);
        shuffle(imgs_.begin(), imgs_.end(), rng_);
        idx_ = 0;
    }

    Mat next() {
        if (imgs_.empty()) return Mat();
        Mat raw = imread(imgs_[idx_]);
        idx_ = (idx_+1) % imgs_.size();
        if (raw.empty()) return next();
        Mat bg; resize(raw, bg, Size(width_, height_));
        if (opacity_ < 1.0) bg.convertTo(bg, -1, opacity_, 0);
        return bg;
    }

    bool has() const { return !imgs_.empty(); }

private:
    vector<string> imgs_;
    size_t idx_ = 0;
    int width_, height_;
    double opacity_;
    mt19937 rng_;
};

// ============================================================================
// Parse SRT
// ============================================================================

string readFileUTF8(const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) return "";
    return {istreambuf_iterator<char>(file), istreambuf_iterator<char>()};
}

double timeToSec(const string& t) {
    int h, m, s, ms; char dot;
    stringstream ss(t);
    ss>>h; ss.get(); ss>>m; ss.get(); ss>>s; ss.get(dot); ss>>ms;
    return h*3600 + m*60 + s + ms/1000.0;
}

vector<Subtitle> parseSRT(const string& filename) {
    vector<Subtitle> subs;
    string data = readFileUTF8(filename);
    if (data.empty()) { cerr << "Cannot open subtitle file: " << filename << endl; return subs; }
    replace(data.begin(), data.end(), '\r', '\n');
    size_t pos;
    while ((pos = data.find("\n\n\n")) != string::npos) data.replace(pos, 3, "\n\n");

    auto parseBlock = [&](const string& block) {
        stringstream ss(block);
        string idx, line, timeLine;
        getline(ss, idx);
        while (getline(ss, line) && line.find(" --> ") == string::npos) {}
        timeLine = line;
        string text;
        while (getline(ss, line)) { if (!text.empty()) text += " "; text += line; }
        size_t arrowPos = timeLine.find(" --> ");
        if (arrowPos == string::npos) return;
        string start = timeLine.substr(0, arrowPos);
        string end   = timeLine.substr(arrowPos + 5);
        auto trim = [](string& s) {
            size_t f = s.find_first_not_of(" \t"), l = s.find_last_not_of(" \t");
            if (f != string::npos) s = s.substr(f, l-f+1);
        };
        trim(start); trim(end);
        subs.push_back({timeToSec(start), timeToSec(end), text});
    };

    stringstream cs(data);
    string block, line;
    while (getline(cs, line)) {
        if (line.empty()) {
            if (!block.empty()) { parseBlock(block); block.clear(); }
        } else {
            if (!block.empty()) block += "\n";
            block += line;
        }
    }
    if (!block.empty()) parseBlock(block);
    return subs;
}

// ============================================================================
// FreeType2 subtitle  —  box nền mờ
// ============================================================================

static Ptr<freetype::FreeType2> g_ft2;

void initFreeType(const string& fontFile) {
    g_ft2 = freetype::createFreeType2();
    g_ft2->loadFontData(fontFile, 0);
}

// Đo ascender / descender thực của font ở fontSize này.
// Dùng chuỗi mẫu chứa dấu tiếng Việt + descender (g,y) + pipe
// để FreeType trả về full height kể cả diacritics.
// Gọi một lần rồi cache — không gọi lại mỗi frame.
struct FtMetrics {
    int ascender;   // pixel trên  baseline
    int descender;  // pixel dưới baseline (>= 0)
};

static FtMetrics getFtMetrics(int fontSize) {
    static map<int, FtMetrics> cache;
    auto it = cache.find(fontSize);
    if (it != cache.end()) return it->second;

    FtMetrics m;
    if (g_ft2) {
        int bl = 0;
        Size s = g_ft2->getTextSize("Ágy|", fontSize, -1, &bl);
        m.ascender  = s.height; // phần trên  baseline
        m.descender = bl;       // phần dưới baseline (bl > 0)
    } else {
        m.ascender  = (int)(fontSize * 0.75);
        m.descender = (int)(fontSize * 0.25);
    }
    cache[fontSize] = m;
    return m;
}

void drawSubtitle(Mat& frame, const string& text, int width, int height,
                  int fontSize, int fixedY, int subtitleOffset) {

    // ---- 1. Wrap text thành lines dùng FreeType getTextSize ----
    vector<string> lines;
    string current;
    int maxWidth = width - 120; // lề 60px mỗi bên
    {
        stringstream ws(text); string word;
        while (ws >> word) {
            string test = current.empty() ? word : current + " " + word;
            int measuredW = g_ft2
                ? g_ft2->getTextSize(test, fontSize, -1, nullptr).width
                : (int)test.size() * (fontSize / 2);
            if (measuredW <= maxWidth) current = test;
            else { if (!current.empty()) lines.push_back(current); current = word; }
        }
        if (!current.empty()) lines.push_back(current);
    }
    if (lines.empty()) return;

    // ---- 2. Lấy font metrics & đo width từng line ----
    FtMetrics fm  = getFtMetrics(fontSize);
    int nLines    = (int)lines.size();
    int maxLineW  = 0;

    vector<int> lineWidths(nLines);
    for (int i = 0; i < nLines; i++) {
        lineWidths[i] = g_ft2
            ? g_ft2->getTextSize(lines[i], fontSize, -1, nullptr).width
            : (int)lines[i].size() * (fontSize / 2);
        maxLineW = max(maxLineW, lineWidths[i]);
    }

    // ---- 3. Tính kích thước box ----
    const int padX    = 20;
    const int padYTop = 10;
    const int padYBot = 12;   // hơi nhiều hơn để descender không chạm đáy box
    const int leading = 6;    // khoảng thêm giữa các dòng

    // chiều cao 1 dòng = ascender + descender + leading
    // (bỏ leading ở dòng cuối nên dùng riêng)
    int lineH = fm.ascender + fm.descender + leading;

    int boxW = min(maxLineW + padX * 2, width - 40);
    int boxH = padYTop + nLines * lineH - leading + padYBot;

    int boxX = (width - boxW) / 2;
    int bottomMargin = (fixedY >= 0) ? fixedY : 80;
    int boxY = max(4, height - bottomMargin - boxH);

    // ---- 4. Vẽ background nền mờ chỉ trên ROI nhỏ ----
    {
        int rx1 = max(0, boxX),        ry1 = max(0, boxY);
        int rx2 = min(width, boxX+boxW), ry2 = min(height, boxY+boxH);
        if (rx2 > rx1 && ry2 > ry1) {
            Mat roi = frame(Rect(rx1, ry1, rx2-rx1, ry2-ry1));
            Mat dark(roi.size(), roi.type(), Scalar(0,0,0));
            addWeighted(roi, 0.30, dark, 0.70, 0, roi); // 70% tối
        }
    }
    // Viền mỏng giúp box tách khỏi nền
    rectangle(frame,
              cv::Point(boxX,      boxY),
              cv::Point(boxX+boxW, boxY+boxH),
              Scalar(100,100,100), 1);

    // ---- 5. Vẽ text — điểm đặt là baseline của FreeType ----
    // baseline dòng đầu = boxY + padYTop + ascender - offset
    // offset < 0 means auto (dùng fontSize)
    int offset    = (subtitleOffset >= 0) ? subtitleOffset : fontSize;
    int baselineY = boxY + padYTop + fm.ascender - offset;

    for (int i = 0; i < nLines; i++) {
        int tx = (width - lineWidths[i]) / 2; // căn giữa theo frame
        int ty = baselineY + i * lineH;

        if (g_ft2) {
            g_ft2->putText(frame, lines[i], cv::Point(tx, ty),
                           fontSize, Scalar(255,255,255), -1, LINE_AA, false);
        } else {
            putText(frame, lines[i], cv::Point(tx, ty),
                    FONT_HERSHEY_SIMPLEX, fontSize / 30.0,
                    Scalar(255,255,255), 2, LINE_AA);
        }
    }
}

// ============================================================================
// Async Video Writer
// ============================================================================

class AsyncVideoWriter {
public:
    AsyncVideoWriter(const string& path, int fourcc, double fps, Size sz)
        : writer_(path, fourcc, fps, sz), done_(false) {
        thread_ = thread([this]{ loop(); });
    }

    ~AsyncVideoWriter() {
        { unique_lock<mutex> lk(mtx_); done_ = true; }
        cv_.notify_one();
        thread_.join();
    }

    bool isOpened() const { return writer_.isOpened(); }

    void write(Mat frame) {
        unique_lock<mutex> lk(mtx_);
        cvFull_.wait(lk, [this]{ return queue_.size() < 12 || done_; });
        queue_.push(std::move(frame));
        cv_.notify_one();
    }

private:
    void loop() {
        while (true) {
            Mat frame;
            {
                unique_lock<mutex> lk(mtx_);
                cv_.wait(lk, [this]{ return !queue_.empty() || done_; });
                if (queue_.empty() && done_) break;
                frame = std::move(queue_.front());
                queue_.pop();
                cvFull_.notify_one();
            }
            writer_.write(frame);
        }
        while (!queue_.empty()) { writer_.write(queue_.front()); queue_.pop(); }
    }

    VideoWriter writer_;
    queue<Mat>  queue_;
    mutex       mtx_;
    condition_variable cv_, cvFull_;
    thread      thread_;
    bool        done_;
};

// ============================================================================
// Drawing helpers
// ============================================================================

// Draw sky gradient background (if no image provided)
void drawSkyBackground(Mat& frame, int width, int height) {
    for (int y = 0; y < height; y++) {
        double ratio = (double)y / height;
        uchar r = (uchar)(135 * (1-ratio) + 200 * ratio);
        uchar g = (uchar)(206 * (1-ratio) + 230 * ratio);
        uchar b = (uchar)(235 * (1-ratio) + 180 * ratio);
        frame.row(y).setTo(Scalar(b, g, r));
    }
}

// Draw ground
void drawGround(Mat& frame, int groundY, int width) {
    rectangle(frame, cv::Point(0, groundY), cv::Point(width, frame.rows),
              Scalar(100, 180, 80), -1);
    rectangle(frame, cv::Point(0, groundY), cv::Point(width, groundY+8),
              Scalar(60, 140, 50), -1);
    // Grass tufts
    for (int x = 10; x < width; x += 40) {
        cv::Point pts[3] = { {x,groundY}, {x+8,groundY-10}, {x+16,groundY} };
        fillConvexPoly(frame, pts, 3, Scalar(50,160,60));
    }
}

// Draw a pipe (top or bottom)
void drawPipe(Mat& frame, int x, int y1, int y2, int pipeWidth) {
    // Pipe body
    rectangle(frame, cv::Point(x, y1), cv::Point(x+pipeWidth, y2), Scalar(30,160,40), -1);
    // Highlight
    rectangle(frame, cv::Point(x+4, y1), cv::Point(x+12, y2), Scalar(80,200,60), -1);
    // Border
    rectangle(frame, cv::Point(x, y1), cv::Point(x+pipeWidth, y2), Scalar(10,100,20), 2);
}

// Draw pipe cap
void drawPipeCap(Mat& frame, int x, int capY, int pipeWidth, bool isTop) {
    int capH = 28;
    int capX = x - 6;
    int capW = pipeWidth + 12;
    int cy = isTop ? capY : capY - capH;
    rectangle(frame, cv::Point(capX, cy), cv::Point(capX+capW, cy+capH), Scalar(30,160,40), -1);
    rectangle(frame, cv::Point(capX+4, cy), cv::Point(capX+12, cy+capH), Scalar(80,200,60), -1);
    rectangle(frame, cv::Point(capX, cy), cv::Point(capX+capW, cy+capH), Scalar(10,100,20), 2);
}

// Draw the bird
void drawBird(Mat& frame, int bx, int by, float velocity, int radius, int frameId) {
    // Body tilt based on velocity
    double tilt = max(-35.0, min(35.0, (double)velocity * 3.0));
    (void)tilt; // Used conceptually – we draw layered circles

    // Shadow
    ellipse(frame, cv::Point(bx+4, by+4), Size(radius, radius/2+2), 0, 0, 360,
            Scalar(0,0,0,60), -1, LINE_AA);

    // Body
    circle(frame, cv::Point(bx, by), radius, Scalar(60,190,255), -1, LINE_AA);
    circle(frame, cv::Point(bx, by), radius, Scalar(20,140,200), 2,  LINE_AA);

    // Belly
    ellipse(frame, cv::Point(bx+4, by+4), Size(radius-4, radius-6), 0, 0, 360,
            Scalar(180,240,255), -1, LINE_AA);

    // Eye
    circle(frame, cv::Point(bx+radius/2, by-radius/4), radius/4, Scalar(255,255,255), -1, LINE_AA);
    circle(frame, cv::Point(bx+radius/2+2, by-radius/4), radius/6, Scalar(20,20,20), -1, LINE_AA);

    // Beak
    vector<cv::Point> beak = {
        {bx+radius-2, by},
        {bx+radius+10, by-3},
        {bx+radius+2, by+6}
    };
    fillConvexPoly(frame, beak, Scalar(20,120,200));

    // Wing flap animation
    int flapPhase = (frameId / 4) % 3;
    vector<cv::Point> wing;
    if (flapPhase == 0) {
        wing = {{bx-4, by+2}, {bx-radius+2, by+radius+4}, {bx+4, by+8}};
    } else if (flapPhase == 1) {
        wing = {{bx-4, by+2}, {bx-radius+2, by+2}, {bx+4, by+4}};
    } else {
        wing = {{bx-4, by+2}, {bx-radius+2, by-radius+4}, {bx+4, by+2}};
    }
    fillConvexPoly(frame, wing, Scalar(30,150,220));
    polylines(frame, wing, true, Scalar(10,100,160), 1, LINE_AA);
}

// Helper: vẽ rectangle bo góc (rounded rect) bằng cách ghép rect + 4 circle
static void drawRoundRect(Mat& frame, cv::Point tl, cv::Point br, int r,
                          const Scalar& color, int thickness = -1) {
    // 4 cạnh thẳng
    rectangle(frame, cv::Point(tl.x+r, tl.y), cv::Point(br.x-r, br.y), color, thickness);
    rectangle(frame, cv::Point(tl.x, tl.y+r), cv::Point(br.x, br.y-r), color, thickness);
    // 4 góc tròn
    circle(frame, cv::Point(tl.x+r, tl.y+r), r, color, thickness, LINE_AA);
    circle(frame, cv::Point(br.x-r, tl.y+r), r, color, thickness, LINE_AA);
    circle(frame, cv::Point(tl.x+r, br.y-r), r, color, thickness, LINE_AA);
    circle(frame, cv::Point(br.x-r, br.y-r), r, color, thickness, LINE_AA);
}

// Helper: vẽ 1 pill card (label nhỏ trên + số lớn dưới)
static void drawPillCard(Mat& frame,
                         int cx, int cy, int w, int h, int r,
                         const string& label, const string& value,
                         const Scalar& bgColor,   // nền card
                         const Scalar& borderColor,
                         const Scalar& labelColor,
                         const Scalar& valueColor) {
    cv::Point tl(cx - w/2, cy - h/2);
    cv::Point br(cx + w/2, cy + h/2);

    // Nền mờ
    Mat roi = frame(Rect(tl.x, tl.y, w, h));
    Mat dark(roi.size(), roi.type(), bgColor);
    addWeighted(roi, 0.25, dark, 0.75, 0, roi);

    // Bo góc
    drawRoundRect(frame, tl, br, r, borderColor, 1);

    // Label nhỏ (trên)
    int bl = 0;
    Size ls = getTextSize(label, FONT_HERSHEY_SIMPLEX, 0.42, 1, &bl);
    putText(frame, label,
            cv::Point(cx - ls.width/2, cy - 6),
            FONT_HERSHEY_SIMPLEX, 0.42, labelColor, 1, LINE_AA);

    // Value lớn (dưới)
    Size vs = getTextSize(value, FONT_HERSHEY_SIMPLEX, 0.85, 2, &bl);
    putText(frame, value,
            cv::Point(cx - vs.width/2, cy + 18),
            FONT_HERSHEY_SIMPLEX, 0.85, valueColor, 2, LINE_AA);
}

// Helper: vẽ icon tim nhỏ gọn
static void drawHeart(Mat& frame, int cx, int cy, int r, const Scalar& color) {
    circle(frame, cv::Point(cx-r/2, cy), r/2, color, -1, LINE_AA);
    circle(frame, cv::Point(cx+r/2, cy), r/2, color, -1, LINE_AA);
    vector<cv::Point> tri = {
        {cx - r, cy + 1},
        {cx + r, cy + 1},
        {cx,     cy + r + r/2}
    };
    fillConvexPoly(frame, tri, color);
}

// Draw HUD — modern flat pill cards
void drawHUD(Mat& frame, int score, int highScore, int lives, double currentTime) {
    int fw = frame.cols;

    // Màu chủ đạo
    const Scalar bgDark   (10,  10,  18 );   // nền card
    const Scalar borderDim(60,  60,  80 );   // viền mờ
    const Scalar gold     (30,  210, 255);   // score  — BGR vàng
    const Scalar silver   (190, 190, 190);   // label xám
    const Scalar cyan     (220, 220, 60 );   // best   — BGR cyan
    const Scalar white    (255, 255, 255);

    // ── Card SCORE (trái) ──────────────────────────────
    drawPillCard(frame,
                 /*cx*/90, /*cy*/44, /*w*/156, /*h*/60, /*r*/12,
                 "SCORE", to_string(score),
                 bgDark, borderDim, silver, gold);

    // ── Card BEST (giữa-trái) ──────────────────────────
    drawPillCard(frame,
                 258, 44, 156, 60, 12,
                 "BEST",  to_string(highScore),
                 bgDark, borderDim, silver, cyan);

    // ── Card TIME (giữa-phải) ─────────────────────────
    int secs = (int)currentTime;
    int mins  = secs / 60; secs %= 60;
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", mins, secs);
    drawPillCard(frame,
                 426, 44, 140, 60, 12,
                 "TIME",  string(timeBuf),
                 bgDark, borderDim, silver, white);

    // ── Lives (kicon tim) — góc phải ──────────────────
    const int maxLives = 5;
    const int heartR   = 9;
    const int heartGap = 26;
    int livesBlockW = maxLives * heartGap;
    int lx0 = fw - livesBlockW - 16;
    int ly  = 44;

    // Nền pill cho lives
    cv::Point ltl(lx0 - 10, ly - 30);
    cv::Point lbr(fw  - 8,  ly + 30);
    {
        Mat roi = frame(Rect(ltl.x, ltl.y, lbr.x-ltl.x, lbr.y-ltl.y));
        Mat dark(roi.size(), roi.type(), bgDark);
        addWeighted(roi, 0.25, dark, 0.75, 0, roi);
    }
    drawRoundRect(frame, ltl, lbr, 12, borderDim, 1);

    // Label "LIVES"
    int bl2 = 0;
    Size ls2 = getTextSize("LIVES", FONT_HERSHEY_SIMPLEX, 0.42, 1, &bl2);
    int lLabelCx = (ltl.x + lbr.x) / 2;
    putText(frame, "LIVES",
            cv::Point(lLabelCx - ls2.width/2, ly - 8),
            FONT_HERSHEY_SIMPLEX, 0.42, silver, 1, LINE_AA);

    // Các icon tim
    for (int i = 0; i < maxLives; i++) {
        int hx = lx0 + i * heartGap + heartGap/2;
        bool alive = (i < lives);
        Scalar col = alive ? Scalar(60, 60, 220) : Scalar(50, 50, 60);
        drawHeart(frame, hx, ly + 12, heartR, col);
        if (!alive)  // outline mờ cho ô trống
            drawHeart(frame, hx, ly + 12, heartR, Scalar(80,80,100));
    }
}

// Draw overlay message (death / level clear)
void drawOverlayMessage(Mat& frame, const string& line1, const string& line2,
                        int width, int height, const Scalar& color) {
    Mat overlay; frame.copyTo(overlay);
    rectangle(overlay, cv::Point(width/2-320,height/2-80),
              cv::Point(width/2+320,height/2+80), Scalar(10,10,10), -1);
    addWeighted(overlay, 0.82, frame, 0.18, 0, frame);
    rectangle(frame, cv::Point(width/2-320,height/2-80),
              cv::Point(width/2+320,height/2+80), color, 2);

    int baseline;
    Size s1 = getTextSize(line1, FONT_HERSHEY_SIMPLEX, 1.4, 3, &baseline);
    putText(frame, line1, cv::Point(width/2-s1.width/2, height/2-14),
            FONT_HERSHEY_SIMPLEX, 1.4, color, 3, LINE_AA);
    Size s2 = getTextSize(line2, FONT_HERSHEY_SIMPLEX, 0.78, 2, &baseline);
    putText(frame, line2, cv::Point(width/2-s2.width/2, height/2+46),
            FONT_HERSHEY_SIMPLEX, 0.78, Scalar(190,190,190), 2, LINE_AA);
}

// Draw clouds
void drawClouds(Mat& frame, int frameId, int width, int height) {
    // Simple scrolling clouds
    struct CloudDef { int y; int period; int phase; };
    static const CloudDef CLOUDS[] = {
        {80, 600, 0}, {120, 800, 200}, {55, 500, 350},
        {160, 700, 100}, {90, 900, 500}
    };
    for (const auto& c : CLOUDS) {
        int cx = width - ((frameId + c.phase) % c.period) * width / c.period;
        // Wrap
        if (cx < -200) cx += width + 200;
        ellipse(frame, cv::Point(cx,    c.y), Size(70,35), 0, 0, 360, Scalar(255,255,255,180), -1, LINE_AA);
        ellipse(frame, cv::Point(cx-40, c.y+5),  Size(50,25), 0, 0, 360, Scalar(245,245,245), -1, LINE_AA);
        ellipse(frame, cv::Point(cx+40, c.y+5),  Size(55,28), 0, 0, 360, Scalar(245,245,245), -1, LINE_AA);
    }
}

// ============================================================================
// AI controller: decide when to flap
// ============================================================================

bool shouldFlap(float birdY, float birdVY, const vector<Pipe>& pipes,
                int birdX, int birdRadius, int pipeWidth, int pipeGap,
                int height, int groundY) {
    // Find the nearest upcoming pipe
    const Pipe* nearest = nullptr;
    for (const auto& p : pipes) {
        if ((int)p.x + pipeWidth > birdX - birdRadius) {
            if (nearest == nullptr || p.x < nearest->x) nearest = &p;
        }
    }

    float targetY;
    if (nearest == nullptr) {
        targetY = height * 0.5f;
    } else {
        targetY = (float)nearest->gapY;
    }

    // Estimate where bird will be in ~8 frames (lookahead)
    float lookahead = 8.0f;
    float futureY = birdY + birdVY * lookahead + 0.5f * 0.5f * lookahead * lookahead;

    // If bird is above target and falling too fast, no flap needed
    // If below or approaching, flap
    bool tooLow  = futureY > targetY + pipeGap * 0.25f;
    bool hitFloor = birdY + birdRadius + birdVY * 4 > groundY - 10;
    bool hitCeiling = birdY - birdRadius < 30 && birdVY < 0;

    return (tooLow || hitFloor) && !hitCeiling;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    Config cfg = parseArguments(argc, argv);

    cout << "Flappy Bird Simulator\n"
         << "  Output:      " << cfg.outputPath << "\n"
         << "  Subtitles:   " << cfg.subtitleFile << "\n"
         << "  Resolution:  " << cfg.width << "x" << cfg.height << "\n"
         << "  FPS:         " << cfg.fps << "\n"
         << "  Gravity:     " << cfg.gravity << "\n"
         << "  Flap:        " << cfg.flapStrength << "\n"
         << "  Pipe speed:  " << cfg.pipeSpeed << "\n"
         << "  Pipe gap:    " << cfg.pipeGap << "\n"
         << "  Threads:     " << cfg.numThreads << "\n\n";

    setNumThreads(cfg.numThreads);

    // Background
    Mat staticBg;
    BackgroundManager* bgMgr = nullptr;
    bool useFolderBg = false;

    if (!cfg.backgroundFolder.empty()) {
        bgMgr = new BackgroundManager(cfg.backgroundFolder, cfg.width, cfg.height,
                                      cfg.bgOpacity, cfg.seed < 0 ? 42 : cfg.seed);
        if (bgMgr->has()) useFolderBg = true;
        else { delete bgMgr; bgMgr = nullptr; }
    }
    if (!useFolderBg && !cfg.backgroundFile.empty()) {
        staticBg = imread(cfg.backgroundFile);
        if (!staticBg.empty()) {
            resize(staticBg, staticBg, Size(cfg.width, cfg.height));
            if (cfg.bgOpacity < 1.0) staticBg.convertTo(staticBg, -1, cfg.bgOpacity, 0);
        }
    }

    // Font
    { ifstream f(cfg.fontFile); if (f.good()) initFreeType(cfg.fontFile); }

    // Subtitles
    vector<Subtitle> subs = parseSRT(cfg.subtitleFile);
    if (subs.empty()) { cerr << "No subtitles loaded!\n"; delete bgMgr; return 1; }
    cout << "Loaded " << subs.size() << " subtitles\n";

    double maxTime = 0;
    for (const auto& s : subs) maxTime = max(maxTime, s.end);
    int totalFrames = (int)(maxTime * cfg.fps);

    // Pre-compute subtitle per frame
    vector<int> frameSub(totalFrames, -1);
    for (int fi = 0; fi < totalFrames; fi++) {
        double t = (double)fi / cfg.fps;
        for (int si = 0; si < (int)subs.size(); si++)
            if (subs[si].start <= t && t <= subs[si].end) { frameSub[fi] = si; break; }
    }

    // Video writer
    AsyncVideoWriter video(cfg.outputPath,
                           VideoWriter::fourcc('m','p','4','v'),
                           cfg.fps, Size(cfg.width, cfg.height));
    if (!video.isOpened()) { cerr << "Cannot open video writer!\n"; delete bgMgr; return 1; }

    // RNG
    random_device rd;
    mt19937 rng(cfg.seed < 0 ? rd() : cfg.seed);

    // Game constants
    const int groundY = cfg.height - 80;
    const int minGapCenter = cfg.pipeGap / 2 + 60;
    const int maxGapCenter = groundY - cfg.pipeGap / 2 - 20;

    // Game state
    float birdY   = cfg.height * 0.45f;
    float birdVY  = 0.0f;
    int   score   = 0;
    int   highScore = 0;
    int   lives   = 3;
    bool  dead    = false;
    int   overlayFrames = 0;
    bool  showOverlay = false;
    string ovLine1, ovLine2;
    Scalar ovColor;

    // Initial pipes
    uniform_int_distribution<int> gapDist(minGapCenter, maxGapCenter);
    vector<Pipe> pipes;
    for (int i = 0; i < 3; i++) {
        float px = (float)(cfg.width + 100 + i * cfg.pipeSpacing);
        pipes.push_back({px, gapDist(rng), false});
    }

    Mat frame(cfg.height, cfg.width, CV_8UC3);
    Mat currentBg;
    if (useFolderBg && bgMgr) currentBg = bgMgr->next();

    const int OVERLAY_FRAMES = cfg.fps * 2;

    cout << "Rendering " << totalFrames << " frames (" << (int)maxTime << "s)...\n";

    for (int frameId = 0; frameId < totalFrames; frameId++) {
        double currentTime = (double)frameId / cfg.fps;

        // Change background if folder mode
        if (useFolderBg && bgMgr && frameId % cfg.bgChangeInterval == 0)
            currentBg = bgMgr->next();

        // ====== Game Logic ======

        if (!showOverlay) {
            // AI flap decision
            if (shouldFlap(birdY, birdVY, pipes, cfg.birdX, cfg.birdRadius,
                           cfg.pipeWidth, cfg.pipeGap, cfg.height, groundY)) {
                birdVY = cfg.flapStrength;
            }

            // Physics
            birdVY += cfg.gravity;
            birdY  += birdVY;

            // Move pipes & spawn new ones
            for (auto& p : pipes) {
                p.x -= cfg.pipeSpeed;
                // Score when bird passes pipe
                if (!p.passed && p.x + cfg.pipeWidth < cfg.birdX - cfg.birdRadius) {
                    p.passed = true;
                    score++;
                    if (score > highScore) highScore = score;
                }
            }
            // Remove off-screen pipes, add new
            pipes.erase(remove_if(pipes.begin(), pipes.end(),
                [](const Pipe& p){ return p.x + 100 < 0; }), pipes.end());
            while (pipes.size() < 4) {
                float lastX = 0;
                for (const auto& p : pipes) lastX = max(lastX, p.x);
                pipes.push_back({lastX + (float)cfg.pipeSpacing, gapDist(rng), false});
            }

            // Collision detection
            bool hitGround  = birdY + cfg.birdRadius >= groundY;
            bool hitCeiling = birdY - cfg.birdRadius <= 0;
            bool hitPipe    = false;
            for (const auto& p : pipes) {
                int px = (int)p.x, py = (int)p.x + cfg.pipeWidth;
                int bx = cfg.birdX;
                // Bird X overlap with pipe
                if (bx + cfg.birdRadius > px && bx - cfg.birdRadius < py) {
                    int gapTop    = p.gapY - cfg.pipeGap/2;
                    int gapBottom = p.gapY + cfg.pipeGap/2;
                    if (birdY - cfg.birdRadius < gapTop || birdY + cfg.birdRadius > gapBottom) {
                        hitPipe = true;
                        break;
                    }
                }
            }

            if (hitGround || hitCeiling || hitPipe) {
                lives--;
                if (lives <= 0) {
                    // Game over — full reset
                    ovLine1  = "GAME OVER";
                    ovLine2  = "Score: " + to_string(score) + "  Best: " + to_string(highScore) + "  Restarting...";
                    ovColor  = Scalar(40, 40, 220);
                    showOverlay  = true;
                    overlayFrames = OVERLAY_FRAMES;
                    score = 0; lives = 3;
                    birdY = cfg.height * 0.45f; birdVY = 0;
                    pipes.clear();
                    for (int i = 0; i < 3; i++) {
                        float px = (float)(cfg.width + 100 + i * cfg.pipeSpacing);
                        pipes.push_back({px, gapDist(rng), false});
                    }
                } else {
                    // Lost a life — respawn bird and push ALL pipes far to the right
                    // so bird has at least (pipeSpacing) pixels of clear runway
                    ovLine1  = "OUCH!";
                    ovLine2  = "Lives left: " + to_string(lives) + "   Continuing...";
                    ovColor  = Scalar(60, 160, 220);
                    showOverlay  = true;
                    overlayFrames = OVERLAY_FRAMES / 2;
                    birdY = cfg.height * 0.45f; birdVY = 0;

                    // Clear existing pipes and spawn fresh ones well ahead of bird
                    const float safeStart = (float)(cfg.birdX + cfg.pipeSpacing + 100);
                    pipes.clear();
                    for (int i = 0; i < 3; i++) {
                        float px = safeStart + (float)(i * cfg.pipeSpacing);
                        pipes.push_back({px, gapDist(rng), false});
                    }
                }
            }
        } else {
            overlayFrames--;
            if (overlayFrames <= 0) showOverlay = false;
        }

        // ====== Render ======

        // Background
        if (useFolderBg && !currentBg.empty())     currentBg.copyTo(frame);
        else if (!staticBg.empty())                 staticBg.copyTo(frame);
        else                                        drawSkyBackground(frame, cfg.width, cfg.height);

        // Clouds (only if no custom bg)
        if (staticBg.empty() && !useFolderBg)
            drawClouds(frame, frameId, cfg.width, cfg.height);

        // Pipes (draw bottom first, then top, so cap appears on top)
        for (const auto& p : pipes) {
            int px    = (int)p.x;
            int gapTop    = p.gapY - cfg.pipeGap / 2;
            int gapBottom = p.gapY + cfg.pipeGap / 2;
            // Bottom pipe (from gap bottom to ground)
            drawPipe(frame, px, gapBottom, groundY, cfg.pipeWidth);
            drawPipeCap(frame, px, gapBottom, cfg.pipeWidth, false);
            // Top pipe (from 0 to gap top)
            drawPipe(frame, px, 0, gapTop, cfg.pipeWidth);
            drawPipeCap(frame, px, gapTop, cfg.pipeWidth, true);
        }

        // Ground
        drawGround(frame, groundY, cfg.width);

        // Bird
        drawBird(frame, cfg.birdX, (int)birdY, birdVY, cfg.birdRadius, frameId);

        // HUD
        drawHUD(frame, score, highScore, lives, currentTime);

        // Subtitle
        int si = frameSub[frameId];
        if (si >= 0)
            drawSubtitle(frame, subs[si].text, cfg.width, cfg.height,
                         cfg.fontSize, cfg.subtitleY, cfg.subtitleOffset);

        // Overlay message
        if (showOverlay)
            drawOverlayMessage(frame, ovLine1, ovLine2, cfg.width, cfg.height, ovColor);

        video.write(frame.clone());

        if (frameId % (cfg.fps * 10) == 0) {
            printf("[%5.1f%%] frame %d/%d | score %d | best %d | lives %d\n",
                   100.0*frameId/totalFrames, frameId, totalFrames,
                   score, highScore, lives);
        }
    }

    delete bgMgr;
    cout << "\nDone! => " << cfg.outputPath << "\n";
    cout << "Final best score: " << highScore << "\n";
    return 0;
}
