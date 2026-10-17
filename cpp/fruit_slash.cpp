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
    string outputPath       = "fruit_slash.mp4";
    string subtitleFile     = "subtitles.srt";
    string fontFile         = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    string backgroundFile   = "";
    string backgroundFolder = "";
    int    bgChangeInterval = 300;
    int    fontSize         = 32;
    int    width            = 1280;
    int    height           = 720;
    int    fps              = 30;
    int    seed             = -1;
    double bgOpacity        = 1.0;
    int    subtitleY        = -1;
    int    subtitleOffset   = -1;
    int    numThreads       = max(1, (int)thread::hardware_concurrency());

    // Game specific
    float  gravity          = 0.25f;
    float  fruitSpeedMin    = 8.0f;
    float  fruitSpeedMax    = 16.0f;
    int    spawnInterval    = 35;   // frames between spawns
    int    maxMisses        = 3;    // bombs allowed miss => bomb hit life
    int    lives            = 5;
    int    slashTrailLen    = 24;
    int    numBombsPerWave  = 1;
};

// Fruit types
enum FruitType { WATERMELON=0, ORANGE, MANGO, STRAWBERRY, PINEAPPLE, BOMB, STAR_FRUIT, NUM_TYPES };

static const Scalar FRUIT_COLORS[][2] = {
    // [outer, inner]
    { Scalar(40,  180, 80 ), Scalar(50,  230, 100) }, // WATERMELON
    { Scalar(20,  120, 255), Scalar(40,  160, 255) }, // ORANGE
    { Scalar(20,  180, 255), Scalar(60,  210, 255) }, // MANGO
    { Scalar(60,  50,  200), Scalar(80,  70,  240) }, // STRAWBERRY
    { Scalar(30,  180, 240), Scalar(80,  210, 255) }, // PINEAPPLE
    { Scalar(20,  20,  20 ), Scalar(60,  60,  60 ) }, // BOMB
    { Scalar(20,  200, 200), Scalar(80,  240, 240) }, // STAR_FRUIT
};

static const string FRUIT_NAMES[] = {
    "Watermelon","Orange","Mango","Strawberry","Pineapple","Bomb","Star"
};

struct Fruit {
    float x, y;        // position
    float vx, vy;      // velocity
    float rot;         // rotation angle (deg)
    float rotSpeed;    // deg/frame
    int   radius;
    FruitType type;
    bool  active;
    bool  slashed;
    int   slashFrame;  // frame when slashed (for split animation)
    // halves after slash
    float hx1, hy1, hvx1, hvy1, hrot1;
    float hx2, hy2, hvx2, hvy2, hrot2;
};

struct SlashPoint {
    cv::Point pt;
    int age; // frames alive
};

struct Particle {
    float x, y, vx, vy;
    int   life, maxLife;
    Scalar color;
    float size;
};

struct ScorePopup {
    int x, y, value;
    int life;
    Scalar color;
};

// ============================================================================
// Parse Arguments
// ============================================================================

void printUsage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "  -o, --output <path>        Output video (default: fruit_slash.mp4)\n"
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
         << "  --gravity <float>          Gravity (default: 0.25)\n"
         << "  --speed-min <float>        Min fruit launch speed (default: 8.0)\n"
         << "  --speed-max <float>        Max fruit launch speed (default: 16.0)\n"
         << "  --spawn-interval <int>     Frames between spawns (default: 35)\n"
         << "  --lives <int>              Starting lives (default: 5)\n"
         << "  --subtitle-y <int>         Subtitle Y margin from bottom\n"
         << "  --subtitle-offset <int>    Baseline offset in box\n"
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
        else if (arg=="--speed-min")              { if(i+1<argc) cfg.fruitSpeedMin     = stof(argv[++i]); }
        else if (arg=="--speed-max")              { if(i+1<argc) cfg.fruitSpeedMax     = stof(argv[++i]); }
        else if (arg=="--spawn-interval")         { if(i+1<argc) cfg.spawnInterval     = stoi(argv[++i]); }
        else if (arg=="--lives")                  { if(i+1<argc) cfg.lives             = stoi(argv[++i]); }
        else if (arg=="--subtitle-y")             { if(i+1<argc) cfg.subtitleY         = stoi(argv[++i]); }
        else if (arg=="--subtitle-offset")        { if(i+1<argc) cfg.subtitleOffset    = stoi(argv[++i]); }
        else if (arg=="--threads")                { if(i+1<argc) cfg.numThreads        = stoi(argv[++i]); }
        else if (arg=="--help")                   { printUsage(argv[0]); exit(0); }
        else { cerr << "Unknown argument: " << arg << "\n"; printUsage(argv[0]); exit(1); }
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
    if (data.empty()) { cerr << "Cannot open subtitle file: " << filename << "\n"; return subs; }
    replace(data.begin(), data.end(), '\r', '\n');
    size_t pos;
    while ((pos = data.find("\n\n\n")) != string::npos) data.replace(pos, 3, "\n\n");

    auto parseBlock = [&](const string& block) {
        stringstream ss(block);
        string line, timeLine;
        getline(ss, line); // index
        while (getline(ss, line) && line.find(" --> ") == string::npos) {}
        timeLine = line;
        string text;
        while (getline(ss, line)) { if (!text.empty()) text += " "; text += line; }
        size_t ap = timeLine.find(" --> ");
        if (ap == string::npos) return;
        string start = timeLine.substr(0, ap);
        string end   = timeLine.substr(ap + 5);
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
        if (line.empty()) { if (!block.empty()) { parseBlock(block); block.clear(); } }
        else              { if (!block.empty()) block += "\n"; block += line; }
    }
    if (!block.empty()) parseBlock(block);
    return subs;
}

// ============================================================================
// FreeType subtitle
// ============================================================================

static Ptr<freetype::FreeType2> g_ft2;

void initFreeType(const string& fontFile) {
    g_ft2 = freetype::createFreeType2();
    g_ft2->loadFontData(fontFile, 0);
}

struct FtMetrics { int ascender; int descender; };

static FtMetrics getFtMetrics(int fontSize) {
    static map<int, FtMetrics> cache;
    auto it = cache.find(fontSize);
    if (it != cache.end()) return it->second;
    FtMetrics m;
    if (g_ft2) {
        int bl = 0;
        Size s = g_ft2->getTextSize("Ágy|", fontSize, -1, &bl);
        m.ascender  = s.height;
        m.descender = bl;
    } else {
        m.ascender  = (int)(fontSize * 0.75);
        m.descender = (int)(fontSize * 0.25);
    }
    cache[fontSize] = m;
    return m;
}

void drawSubtitle(Mat& frame, const string& text, int width, int height,
                  int fontSize, int fixedY, int subtitleOffset) {
    vector<string> lines;
    string current;
    int maxWidth = width - 120;
    {
        stringstream ws(text); string word;
        while (ws >> word) {
            string test = current.empty() ? word : current + " " + word;
            int mw = g_ft2 ? g_ft2->getTextSize(test, fontSize, -1, nullptr).width
                           : (int)test.size() * (fontSize / 2);
            if (mw <= maxWidth) current = test;
            else { if (!current.empty()) lines.push_back(current); current = word; }
        }
        if (!current.empty()) lines.push_back(current);
    }
    if (lines.empty()) return;

    FtMetrics fm = getFtMetrics(fontSize);
    int nLines   = (int)lines.size();
    int maxLineW = 0;
    vector<int> lw(nLines);
    for (int i = 0; i < nLines; i++) {
        lw[i] = g_ft2 ? g_ft2->getTextSize(lines[i], fontSize, -1, nullptr).width
                      : (int)lines[i].size() * (fontSize / 2);
        maxLineW = max(maxLineW, lw[i]);
    }
    const int padX = 20, padYTop = 10, padYBot = 12, leading = 6;
    int lineH  = fm.ascender + fm.descender + leading;
    int boxW   = min(maxLineW + padX * 2, width - 40);
    int boxH   = padYTop + nLines * lineH - leading + padYBot;
    int boxX   = (width - boxW) / 2;
    int botMgn = (fixedY >= 0) ? fixedY : 80;
    int boxY   = max(4, height - botMgn - boxH);

    {
        int rx1 = max(0,boxX), ry1 = max(0,boxY);
        int rx2 = min(width,boxX+boxW), ry2 = min(height,boxY+boxH);
        if (rx2>rx1 && ry2>ry1) {
            Mat roi = frame(Rect(rx1,ry1,rx2-rx1,ry2-ry1));
            Mat dark(roi.size(), roi.type(), Scalar(0,0,0));
            addWeighted(roi, 0.30, dark, 0.70, 0, roi);
        }
    }
    rectangle(frame, cv::Point(boxX,boxY), cv::Point(boxX+boxW,boxY+boxH), Scalar(100,100,100), 1);

    int offset    = (subtitleOffset >= 0) ? subtitleOffset : fontSize;
    int baselineY = boxY + padYTop + fm.ascender - offset;
    for (int i = 0; i < nLines; i++) {
        int tx = (width - lw[i]) / 2;
        int ty = baselineY + i * lineH;
        if (g_ft2) g_ft2->putText(frame, lines[i], cv::Point(tx,ty), fontSize, Scalar(255,255,255), -1, LINE_AA, false);
        else putText(frame, lines[i], cv::Point(tx,ty), FONT_HERSHEY_SIMPLEX, fontSize/30.0, Scalar(255,255,255), 2, LINE_AA);
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
    queue<Mat> queue_;
    mutex mtx_;
    condition_variable cv_, cvFull_;
    thread thread_;
    bool done_;
};

// ============================================================================
// Math helpers
// ============================================================================

static inline cv::Point rotPt(float ox, float oy, float dx, float dy, float angle) {
    float rad = angle * CV_PI / 180.f;
    float c = cosf(rad), s = sinf(rad);
    return cv::Point((int)(ox + dx*c - dy*s), (int)(oy + dx*s + dy*c));
}

// ============================================================================
// Drawing: Background
// ============================================================================

void drawDarkBackground(Mat& frame, int w, int h) {
    // Deep rich ink-black to deep navy gradient
    for (int y = 0; y < h; y++) {
        double r = (double)y / h;
        uchar R = (uchar)(4  + r * 6);
        uchar G = (uchar)(6  + r * 10);
        uchar B = (uchar)(18 + r * 28);
        frame.row(y).setTo(Scalar(B, G, R));
    }
    // Subtle radial glow at center
    for (int y = 0; y < h; y += 4) {
        for (int x = 0; x < w; x += 4) {
            float dx = (x - w/2.f) / (w/2.f);
            float dy = (y - h/2.f) / (h/2.f);
            float dist = sqrtf(dx*dx + dy*dy);
            float glow = max(0.f, 1.f - dist * 1.4f) * 0.08f;
            for (int yy = y; yy < min(h, y+4); yy++)
                for (int xx = x; xx < min(w, x+4); xx++) {
                    auto& px = frame.at<Vec3b>(yy, xx);
                    px[0] = (uchar)min(255, (int)(px[0] + glow * 40));
                    px[2] = (uchar)min(255, (int)(px[2] + glow * 15));
                }
        }
    }
}

void drawStars(Mat& frame, int frameId, int w, int h, mt19937& rng) {
    // Static stars (based on fixed seed)
    static vector<cv::Point> starPts;
    static vector<float> starBri;
    if (starPts.empty()) {
        mt19937 srng(12345);
        uniform_int_distribution<int> rx(0, w-1), ry(0, h/2);
        uniform_real_distribution<float> rb(0.3f, 1.0f);
        for (int i = 0; i < 120; i++) {
            starPts.push_back({rx(srng), ry(srng)});
            starBri.push_back(rb(srng));
        }
    }
    for (int i = 0; i < (int)starPts.size(); i++) {
        float twinkle = 0.5f + 0.5f * sinf((frameId + i*17) * 0.05f);
        uchar v = (uchar)(starBri[i] * twinkle * 200);
        int r = starBri[i] > 0.8f ? 2 : 1;
        circle(frame, starPts[i], r, Scalar(v,v,v), -1, LINE_AA);
    }
}

// ============================================================================
// Drawing: Fruit
// ============================================================================

// Draw a stylized fruit as layered circles + details
void drawFruit(Mat& frame, const Fruit& f, int frameId) {
    int cx = (int)f.x, cy = (int)f.y;
    int r  = f.radius;
    const Scalar& outerC = FRUIT_COLORS[f.type][0];
    const Scalar& innerC = FRUIT_COLORS[f.type][1];

    if (f.type == BOMB) {
        // Dark sphere bomb
        // Glow
        for (int gr = r+14; gr > r; gr -= 3) {
            float alpha = (float)(r+14-gr)/14.f * 0.25f;
            circle(frame, cv::Point(cx,cy), gr, Scalar(0, (uchar)(40*alpha), (uchar)(60*alpha)), -1, LINE_AA);
        }
        // Body
        circle(frame, cv::Point(cx,cy), r, Scalar(25,25,28), -1, LINE_AA);
        circle(frame, cv::Point(cx,cy), r, Scalar(60,60,80), 2,  LINE_AA);
        // Shine
        circle(frame, cv::Point(cx-r/3,cy-r/3), r/4, Scalar(100,100,120), -1, LINE_AA);
        // Fuse
        cv::Point fuseBase(cx, cy - r);
        cv::Point fuseTip(cx + r/2, cy - r - r/2 - 5);
        line(frame, fuseBase, fuseTip, Scalar(60, 80, 120), 3, LINE_AA);
        // Spark
        int sparkPhase = (frameId / 2) % 6;
        for (int sp = 0; sp < 6; sp++) {
            float sa = (sp * 60 + frameId*8) * CV_PI / 180.f;
            int sl = (sp == sparkPhase) ? r/2 : r/4;
            cv::Point sparkEnd(fuseTip.x + (int)(cosf(sa)*sl),
                               fuseTip.y + (int)(sinf(sa)*sl));
            line(frame, fuseTip, sparkEnd,
                 Scalar(50, 180+(sp==sparkPhase?60:0), 255), 2, LINE_AA);
        }
        // Skull icon (simplified)
        circle(frame, cv::Point(cx,cy-1), r/3, Scalar(200,200,220), -1, LINE_AA);
        circle(frame, cv::Point(cx-r/6,cy-3), r/8, Scalar(25,25,28), -1, LINE_AA);
        circle(frame, cv::Point(cx+r/6,cy-3), r/8, Scalar(25,25,28), -1, LINE_AA);
        rectangle(frame, cv::Point(cx-r/5, cy+r/8), cv::Point(cx+r/5, cy+r/4),
                  Scalar(200,200,220), -1);
        return;
    }

    // Fruit glow
    for (int gr = r+12; gr > r; gr -= 3) {
        float a = (float)(r+12-gr)/12.f * 0.15f;
        circle(frame, cv::Point(cx,cy), gr,
               Scalar((uchar)(outerC[0]*a),(uchar)(outerC[1]*a),(uchar)(outerC[2]*a)),
               -1, LINE_AA);
    }

    // Shadow
    ellipse(frame, cv::Point(cx+3,cy+4), Size(r,r/2+2), 0,0,360, Scalar(0,0,0), -1, LINE_AA);

    // Main body
    circle(frame, cv::Point(cx,cy), r,   outerC,  -1, LINE_AA);
    circle(frame, cv::Point(cx,cy), r,   Scalar(outerC[0]*0.5, outerC[1]*0.5, outerC[2]*0.5), 2, LINE_AA);

    // Inner flesh highlight
    circle(frame, cv::Point(cx,cy), (int)(r*0.62f), innerC, -1, LINE_AA);

    // Type-specific details
    switch (f.type) {
        case WATERMELON: {
            // Green stripes on outer ring (rotate with rot)
            for (int i = 0; i < 5; i++) {
                float a = (f.rot + i * 36) * CV_PI / 180.f;
                cv::Point s(cx + (int)(r*0.65f*cosf(a)), cy + (int)(r*0.65f*sinf(a)));
                cv::Point e(cx + (int)(r*0.92f*cosf(a)), cy + (int)(r*0.92f*sinf(a)));
                line(frame, s, e, Scalar(30,140,50), 3, LINE_AA);
            }
            // Seeds
            for (int i = 0; i < 6; i++) {
                float a = (f.rot + i * 60) * CV_PI / 180.f;
                int sx = cx + (int)(r*0.35f*cosf(a));
                int sy = cy + (int)(r*0.35f*sinf(a));
                ellipse(frame, cv::Point(sx,sy), Size(3,6), f.rot+i*60, 0,360, Scalar(20,30,40), -1, LINE_AA);
            }
            break;
        }
        case ORANGE: {
            // Segments lines
            for (int i = 0; i < 8; i++) {
                float a = (f.rot + i * 45) * CV_PI / 180.f;
                line(frame, cv::Point(cx,cy),
                     cv::Point(cx+(int)(r*0.6f*cosf(a)), cy+(int)(r*0.6f*sinf(a))),
                     Scalar(20,100,200), 1, LINE_AA);
            }
            // Center dot
            circle(frame, cv::Point(cx,cy), 4, Scalar(20,80,180), -1, LINE_AA);
            break;
        }
        case MANGO: {
            // Oval shape + fiber lines
            ellipse(frame, cv::Point(cx,cy), Size((int)(r*0.5f),(int)(r*0.35f)), f.rot,0,360,Scalar(20,160,240),-1,LINE_AA);
            for (int i = -2; i <= 2; i++) {
                float a = (f.rot + 90) * CV_PI / 180.f;
                int ox = (int)(i * 5 * cosf(a+CV_PI/2));
                int oy = (int)(i * 5 * sinf(a+CV_PI/2));
                line(frame, cv::Point(cx+ox,cy+oy-r/3),
                     cv::Point(cx+ox,cy+oy+r/3),
                     Scalar(30,180,255), 1, LINE_AA);
            }
            break;
        }
        case STRAWBERRY: {
            // Seeds dots
            for (int i = 0; i < 8; i++) {
                float a = (f.rot + i*45) * CV_PI/180.f;
                int dx=(int)(r*0.4f*cosf(a)), dy=(int)(r*0.4f*sinf(a));
                circle(frame, cv::Point(cx+dx,cy+dy), 3, Scalar(200,220,240), -1, LINE_AA);
            }
            // Leaf cap
            for (int i = 0; i < 3; i++) {
                float a = (f.rot + i*120 - 90) * CV_PI/180.f;
                vector<cv::Point> leaf = {
                    cv::Point(cx,cy-r),
                    cv::Point(cx+(int)(r/2*cosf(a-0.4f)),cy-r+(int)(r/2*sinf(a-0.4f))),
                    cv::Point(cx+(int)(r/2*cosf(a+0.4f)),cy-r+(int)(r/2*sinf(a+0.4f)))
                };
                fillConvexPoly(frame, leaf, Scalar(40,160,60));
            }
            break;
        }
        case PINEAPPLE: {
            // Diamond grid pattern
            for (int i = -3; i <= 3; i++)
                for (int j = -3; j <= 3; j++) {
                    float a = f.rot * CV_PI / 180.f;
                    float dx = i*r/3.f, dy = j*r/3.f + (i%2)*r/6.f;
                    float nx = dx*cosf(a) - dy*sinf(a);
                    float ny = dx*sinf(a) + dy*cosf(a);
                    if (sqrtf(nx*nx+ny*ny) < r*0.55f)
                        rectangle(frame,
                                  cv::Point(cx+(int)nx-3, cy+(int)ny-3),
                                  cv::Point(cx+(int)nx+3, cy+(int)ny+3),
                                  Scalar(20,150,200), 1);
                }
            // Crown
            for (int i = -2; i <= 2; i++) {
                float a = (f.rot + i*18 - 90) * CV_PI/180.f;
                line(frame, cv::Point(cx,cy-r),
                     cv::Point(cx+(int)(r*0.5f*cosf(a)),cy-r+(int)(r*0.5f*sinf(a))),
                     Scalar(40,180,60), 3, LINE_AA);
            }
            break;
        }
        case STAR_FRUIT: {
            // 5-pointed star shape
            vector<cv::Point> star;
            for (int i = 0; i < 10; i++) {
                float a = (f.rot + i*36 - 90) * CV_PI/180.f;
                float rr = (i%2==0) ? r*0.95f : r*0.45f;
                star.push_back(cv::Point(cx+(int)(rr*cosf(a)), cy+(int)(rr*sinf(a))));
            }
            fillConvexPoly(frame, star, innerC);
            polylines(frame, star, true, Scalar(outerC[0]*0.6,outerC[1]*0.6,outerC[2]*0.6), 2, LINE_AA);
            break;
        }
        default: break;
    }

    // Shine highlight
    ellipse(frame, cv::Point(cx-r/4, cy-r/4), Size(r/4,r/6), f.rot-30, 0,360,
            Scalar(230,230,230), -1, LINE_AA);
}

// Draw slashed half
void drawFruitHalf(Mat& frame, float hx, float hy, float hrot, int r,
                   FruitType type, bool top) {
    int cx=(int)hx, cy=(int)hy;
    const Scalar& outerC = FRUIT_COLORS[type][0];
    const Scalar& innerC = FRUIT_COLORS[type][1];

    if (type == BOMB) {
        // Explosion debris
        circle(frame, cv::Point(cx,cy), r/2, Scalar(20,80,200), -1, LINE_AA);
        circle(frame, cv::Point(cx,cy), r/3, Scalar(40,120,255), -1, LINE_AA);
        return;
    }

    // Draw half circle (clip with rect mask is expensive, use ellipse)
    float a = hrot * CV_PI / 180.f;
    // Outer half
    ellipse(frame, cv::Point(cx,cy), Size(r,r), hrot, top?180:0, top?360:180, outerC, -1, LINE_AA);
    // Inner flesh
    Scalar fleshColor = Scalar(
        min(255,(int)(innerC[0]+60)),
        min(255,(int)(innerC[1]+40)),
        min(255,(int)(innerC[2]+40)));
    ellipse(frame, cv::Point(cx,cy), Size((int)(r*0.85f),(int)(r*0.85f)),
            hrot, top?180:0, top?360:180, fleshColor, -1, LINE_AA);
    // Cut face (flat edge)
    float nx = cosf(a+CV_PI/2), ny = sinf(a+CV_PI/2);
    cv::Point p1(cx-(int)(r*nx), cy-(int)(r*ny));
    cv::Point p2(cx+(int)(r*nx), cy+(int)(r*ny));
    line(frame, p1, p2, Scalar(220,230,255), 2, LINE_AA);

    // Juice drips
    for (int i = -1; i <= 1; i++) {
        float da = i * 0.3f;
        float dripA = a + (top ? 0 : CV_PI) + da;
        cv::Point dripStart(cx + (int)(r*0.5f*cosf(a+CV_PI/2+da)),
                            cy + (int)(r*0.5f*sinf(a+CV_PI/2+da)));
        cv::Point dripEnd(dripStart.x + (int)(r*0.4f*cosf(dripA)),
                          dripStart.y + (int)(r*0.4f*sinf(dripA)));
        line(frame, dripStart, dripEnd,
             Scalar(min(255,(int)(fleshColor[0]+20)),
                    min(255,(int)(fleshColor[1]+20)),
                    min(255,(int)(fleshColor[2]+20))), 2, LINE_AA);
    }
}

// ============================================================================
// Drawing: Slash Trail
// ============================================================================

void drawSlashTrail(Mat& frame, const deque<SlashPoint>& trail, int maxLen) {
    if (trail.size() < 2) return;
    int n = (int)trail.size();
    for (int i = 1; i < n; i++) {
        float t = (float)i / maxLen;
        float alpha = t * t;
        int thickness = max(1, (int)(alpha * 6));
        // Color: white -> cyan -> blue fade
        uchar R = (uchar)(alpha * 60);
        uchar G = (uchar)(alpha * 220);
        uchar B = (uchar)(alpha * 255);
        line(frame, trail[i-1].pt, trail[i].pt, Scalar(R,G,B), thickness, LINE_AA);
    }
    // Bright tip
    if (!trail.empty()) {
        circle(frame, trail.back().pt, 5, Scalar(220,255,255), -1, LINE_AA);
        circle(frame, trail.back().pt, 8, Scalar(100,200,255), 1,  LINE_AA);
    }
}

// ============================================================================
// Drawing: Particles
// ============================================================================

void spawnSlashParticles(vector<Particle>& particles, float x, float y,
                         FruitType type, mt19937& rng) {
    const Scalar& c1 = FRUIT_COLORS[type][0];
    const Scalar& c2 = FRUIT_COLORS[type][1];
    int count = (type == BOMB) ? 40 : 20;
    uniform_real_distribution<float> ang(0.f, 2.f*CV_PI);
    uniform_real_distribution<float> spd(1.f, 9.f);
    uniform_real_distribution<float> sz(2.f, 6.f);
    uniform_int_distribution<int>    lf(15, 40);
    for (int i = 0; i < count; i++) {
        float a  = ang(rng), s = spd(rng);
        Particle p;
        p.x = x; p.y = y;
        p.vx = cosf(a)*s; p.vy = sinf(a)*s - 2.f;
        p.life = lf(rng); p.maxLife = p.life;
        p.color = (i % 2 == 0) ? c1 : c2;
        if (type == BOMB) p.color = Scalar(30, 120+i*3, 255);
        p.size = sz(rng);
        particles.push_back(p);
    }
    // Juice droplets
    if (type != BOMB) {
        Scalar juiceC = Scalar(
            min(255,(int)(c2[0]+80)),
            min(255,(int)(c2[1]+60)),
            min(255,(int)(c2[2]+60)));
        uniform_real_distribution<float> jspd(3.f, 12.f);
        for (int i = 0; i < 8; i++) {
            float a = ang(rng), s = jspd(rng);
            Particle p;
            p.x=x; p.y=y;
            p.vx=cosf(a)*s; p.vy=sinf(a)*s-4.f;
            p.life=30; p.maxLife=30;
            p.color=juiceC;
            p.size=3.f;
            particles.push_back(p);
        }
    }
}

void updateAndDrawParticles(Mat& frame, vector<Particle>& particles, float gravity) {
    for (auto& p : particles) {
        if (p.life <= 0) continue;
        float alpha = (float)p.life / p.maxLife;
        // Draw
        uchar R = (uchar)(p.color[2]*alpha);
        uchar G = (uchar)(p.color[1]*alpha);
        uchar B = (uchar)(p.color[0]*alpha);
        int px=(int)p.x, py=(int)p.y;
        if (px>=0 && py>=0 && px<frame.cols && py<frame.rows)
            circle(frame, cv::Point(px,py), (int)(p.size*alpha+1), Scalar(B,G,R), -1, LINE_AA);
        // Update
        p.x  += p.vx;
        p.y  += p.vy;
        p.vy += gravity * 1.5f;
        p.vx *= 0.97f;
        p.life--;
    }
    particles.erase(remove_if(particles.begin(), particles.end(),
        [](const Particle& p){ return p.life <= 0; }), particles.end());
}

// ============================================================================
// Drawing: Score Popups
// ============================================================================

void updateAndDrawScorePopups(Mat& frame, vector<ScorePopup>& popups) {
    for (auto& p : popups) {
        if (p.life <= 0) continue;
        float a = (float)p.life / 30.f;
        int y = p.y - (30 - p.life) * 1;
        string txt = (p.value > 0 ? "+" : "") + to_string(p.value);
        uchar R=(uchar)(p.color[2]*a), G=(uchar)(p.color[1]*a), B=(uchar)(p.color[0]*a);
        int bl=0;
        Size s=getTextSize(txt, FONT_HERSHEY_SIMPLEX, 0.9, 2, &bl);
        putText(frame, txt, cv::Point(p.x-s.width/2, y),
                FONT_HERSHEY_SIMPLEX, 0.9, Scalar(B,G,R), 2, LINE_AA);
        p.life--;
    }
    popups.erase(remove_if(popups.begin(), popups.end(),
        [](const ScorePopup& p){ return p.life<=0; }), popups.end());
}

// ============================================================================
// Drawing: HUD
// ============================================================================

static void drawRoundRect(Mat& frame, cv::Point tl, cv::Point br, int r, const Scalar& c, int t=-1) {
    rectangle(frame, cv::Point(tl.x+r,tl.y), cv::Point(br.x-r,br.y), c, t);
    rectangle(frame, cv::Point(tl.x,tl.y+r), cv::Point(br.x,br.y-r), c, t);
    circle(frame, cv::Point(tl.x+r,tl.y+r), r, c, t, LINE_AA);
    circle(frame, cv::Point(br.x-r,tl.y+r), r, c, t, LINE_AA);
    circle(frame, cv::Point(tl.x+r,br.y-r), r, c, t, LINE_AA);
    circle(frame, cv::Point(br.x-r,br.y-r), r, c, t, LINE_AA);
}

void drawHUD(Mat& frame, int score, int highScore, int combo, int lives, int maxLives, double t) {
    int fw = frame.cols;

    const Scalar bgDark   (8,   8,   16 );
    const Scalar borderDim(50,  50,  80 );
    const Scalar gold     (30,  210, 255);
    const Scalar silver   (160, 160, 160);
    const Scalar cyan     (220, 200, 40 );
    const Scalar comboC   (100, 80,  255);
    const Scalar white    (255, 255, 255);

    auto drawCard = [&](int cx, int cy, int w, int h, int r,
                        const string& label, const string& value,
                        const Scalar& vc) {
        cv::Point tl(cx-w/2, cy-h/2), br(cx+w/2, cy+h/2);
        Mat roi = frame(Rect(tl.x, tl.y, w, h));
        Mat dark(roi.size(), roi.type(), bgDark);
        addWeighted(roi, 0.2, dark, 0.8, 0, roi);
        drawRoundRect(frame, tl, br, r, borderDim, 1);
        int bl=0;
        Size ls=getTextSize(label, FONT_HERSHEY_SIMPLEX, 0.40, 1, &bl);
        putText(frame, label, cv::Point(cx-ls.width/2, cy-5), FONT_HERSHEY_SIMPLEX, 0.40, silver, 1, LINE_AA);
        Size vs=getTextSize(value, FONT_HERSHEY_SIMPLEX, 0.88, 2, &bl);
        putText(frame, value, cv::Point(cx-vs.width/2, cy+18), FONT_HERSHEY_SIMPLEX, 0.88, vc, 2, LINE_AA);
    };

    drawCard(90,  44, 156, 58, 12, "SCORE",    to_string(score),     gold);
    drawCard(258, 44, 156, 58, 12, "BEST",     to_string(highScore), cyan);

    // Combo card (pulsate if combo > 2)
    Scalar comboColor = (combo > 2) ? Scalar(80, 80, 255) : silver;
    if (combo > 4) comboColor = Scalar(60, 60, 255);
    float pulse = (combo > 2) ? 1.f + 0.08f * sinf((float)t * 10.f) : 1.f;
    string comboStr = (combo > 1) ? to_string(combo) + "x!" : to_string(combo) + "x";
    drawCard(426, 44, 140, 58, 12, "COMBO", comboStr, comboColor);

    // Time card
    int secs = (int)t; int mins = secs/60; secs %= 60;
    char timeBuf[16]; snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", mins, secs);
    drawCard(578, 44, 140, 58, 12, "TIME", string(timeBuf), white);

    // Lives — heart icons
    const int heartR = 9, heartGap = 26;
    int livesBlockW = maxLives * heartGap;
    int lx0 = fw - livesBlockW - 16;
    int ly  = 44;
    cv::Point ltl(lx0-10, ly-30), lbr(fw-8, ly+30);
    {
        Mat roi = frame(Rect(ltl.x, ltl.y, lbr.x-ltl.x, lbr.y-ltl.y));
        Mat dark(roi.size(), roi.type(), bgDark);
        addWeighted(roi, 0.2, dark, 0.8, 0, roi);
    }
    drawRoundRect(frame, ltl, lbr, 12, borderDim, 1);
    int bl2=0;
    Size ls2=getTextSize("LIVES", FONT_HERSHEY_SIMPLEX, 0.40, 1, &bl2);
    int lcx=(ltl.x+lbr.x)/2;
    putText(frame,"LIVES", cv::Point(lcx-ls2.width/2, ly-8), FONT_HERSHEY_SIMPLEX, 0.40, silver, 1, LINE_AA);

    // Draw fruit-themed life hearts
    for (int i = 0; i < maxLives; i++) {
        int hx = lx0 + i*heartGap + heartGap/2;
        bool alive = (i < lives);
        // Small fruit circle as life indicator
        Scalar lifeC = alive ? FRUIT_COLORS[WATERMELON][0] : Scalar(40,40,60);
        circle(frame, cv::Point(hx, ly+12), heartR,   lifeC, -1, LINE_AA);
        circle(frame, cv::Point(hx, ly+12), heartR,   Scalar(80,80,100), 1, LINE_AA);
        if (alive) {
            circle(frame, cv::Point(hx-3, ly+9), 3, Scalar(180,240,180), -1, LINE_AA);
        }
    }
}

// ============================================================================
// Drawing: Overlay messages
// ============================================================================

void drawOverlayMessage(Mat& frame, const string& line1, const string& line2,
                        int w, int h, const Scalar& color) {
    Mat overlay; frame.copyTo(overlay);
    rectangle(overlay, cv::Point(w/2-340,h/2-90), cv::Point(w/2+340,h/2+90), Scalar(5,5,12), -1);
    addWeighted(overlay, 0.82, frame, 0.18, 0, frame);
    drawRoundRect(frame, cv::Point(w/2-340,h/2-90), cv::Point(w/2+340,h/2+90), 18, color, 2);

    int bl;
    Size s1=getTextSize(line1, FONT_HERSHEY_SIMPLEX, 1.5, 3, &bl);
    putText(frame, line1, cv::Point(w/2-s1.width/2, h/2-10), FONT_HERSHEY_SIMPLEX, 1.5, color, 3, LINE_AA);
    Size s2=getTextSize(line2, FONT_HERSHEY_SIMPLEX, 0.75, 2, &bl);
    putText(frame, line2, cv::Point(w/2-s2.width/2, h/2+50), FONT_HERSHEY_SIMPLEX, 0.75, Scalar(160,160,180), 2, LINE_AA);
}

// ============================================================================
// AI Slash Controller
// ============================================================================

// Returns the "slash cursor" position and whether a slash happens this frame
// Simple strategy: move toward nearest non-bomb active fruit
cv::Point aiSlashTarget(const vector<Fruit>& fruits, int w, int h) {
    const Fruit* best = nullptr;
    float bestScore = -1.f;

    for (const auto& f : fruits) {
        if (!f.active || f.slashed) continue;
        if (f.type == BOMB) continue; // avoid bombs
        // Score: prefer fruits that are on their way up (vy < 0) or near peak
        // and not too close to edges
        float margin = 60.f;
        if (f.x < margin || f.x > w-margin) continue;
        // Predict where it'll be in ~5 frames
        float py = f.y + f.vy * 5 + 0.5f * 0.25f * 25;
        float score = (float)(h - f.y); // prefer higher fruits
        if (f.vy > 0) score *= 0.5f;    // less urgent if falling already
        if (score > bestScore) { bestScore = score; best = &f; }
    }

    if (!best) {
        // Default: center
        return cv::Point(w/2, h/2);
    }
    return cv::Point((int)best->x, (int)best->y);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    Config cfg = parseArguments(argc, argv);

    cout << "Fruit Slash Simulator\n"
         << "  Output:       " << cfg.outputPath << "\n"
         << "  Subtitles:    " << cfg.subtitleFile << "\n"
         << "  Resolution:   " << cfg.width << "x" << cfg.height << "\n"
         << "  FPS:          " << cfg.fps << "\n"
         << "  Gravity:      " << cfg.gravity << "\n"
         << "  Lives:        " << cfg.lives << "\n"
         << "  Spawn every:  " << cfg.spawnInterval << " frames\n"
         << "  Threads:      " << cfg.numThreads << "\n\n";

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

    // Distributions
    uniform_real_distribution<float> spawnX(100.f, (float)(cfg.width-100));
    uniform_real_distribution<float> spawnVX(-3.f, 3.f);
    uniform_real_distribution<float> spawnVY(-cfg.fruitSpeedMax, -cfg.fruitSpeedMin);
    uniform_real_distribution<float> rotSpd(-6.f, 6.f);
    uniform_int_distribution<int>    fruitTypeDist(0, (int)STAR_FRUIT);
    uniform_real_distribution<float> bombChance(0.f, 1.f);

    // Game state
    vector<Fruit>     fruits;
    vector<Particle>  particles;
    vector<ScorePopup> scorePopups;
    deque<SlashPoint> slashTrail;

    int   score     = 0;
    int   highScore = 0;
    int   lives     = cfg.lives;
    int   combo     = 0;
    int   comboTimer= 0;
    bool  showOverlay    = false;
    int   overlayFrames  = 0;
    string ovLine1, ovLine2;
    Scalar ovColor;
    int   missedFruits = 0;

    // AI slash state
    cv::Point slashCursor(cfg.width/2, cfg.height/2);
    cv::Point slashTarget(cfg.width/2, cfg.height/2);
    int       slashCooldown = 0;

    Mat frame(cfg.height, cfg.width, CV_8UC3);
    Mat currentBg;
    if (useFolderBg && bgMgr) currentBg = bgMgr->next();

    const int OVERLAY_FRAMES = cfg.fps * 2;
    const int maxLives = cfg.lives;

    cout << "Rendering " << totalFrames << " frames (" << (int)maxTime << "s)...\n";

    for (int frameId = 0; frameId < totalFrames; frameId++) {
        double currentTime = (double)frameId / cfg.fps;

        if (useFolderBg && bgMgr && frameId % cfg.bgChangeInterval == 0)
            currentBg = bgMgr->next();

        // ====== Game Logic ======

        if (!showOverlay) {

            // Spawn fruits
            if (frameId % cfg.spawnInterval == 0) {
                // Decide how many to spawn (1-3 cluster)
                int count = 1 + (frameId > cfg.fps*10 ? 1 : 0) + (frameId > cfg.fps*25 ? 1 : 0);
                for (int ci = 0; ci < count; ci++) {
                    Fruit f;
                    f.x  = spawnX(rng);
                    f.y  = (float)(cfg.height + 30);
                    f.vx = spawnVX(rng);
                    f.vy = spawnVY(rng);
                    f.rot = 0.f;
                    f.rotSpeed = rotSpd(rng);
                    f.radius = 30 + (int)(rng() % 10);
                    f.slashed = false;
                    f.active  = true;
                    f.slashFrame = -1;

                    // Chance of bomb increases over time
                    float bc = 0.06f + (float)frameId / totalFrames * 0.12f;
                    if (bombChance(rng) < bc)
                        f.type = BOMB;
                    else
                        f.type = (FruitType)fruitTypeDist(rng);

                    fruits.push_back(f);
                }
            }

            // Physics update
            for (auto& f : fruits) {
                if (!f.active) continue;
                if (!f.slashed) {
                    f.vy += cfg.gravity;
                    f.x  += f.vx;
                    f.y  += f.vy;
                    f.rot += f.rotSpeed;
                    // Deactivate if fallen off screen
                    if (f.y > cfg.height + 80) {
                        f.active = false;
                        if (f.type != BOMB) {
                            missedFruits++;
                            combo = 0; comboTimer = 0;
                            if (missedFruits >= 3) {
                                missedFruits = 0;
                                lives--;
                                scorePopups.push_back({cfg.width/2, cfg.height/2-60, -10, 30, Scalar(60,60,220)});
                                if (lives <= 0) {
                                    ovLine1 = "GAME OVER";
                                    ovLine2 = "Score: " + to_string(score) + "   Best: " + to_string(highScore) + "   Restarting...";
                                    ovColor = Scalar(40,40,200);
                                    showOverlay = true; overlayFrames = OVERLAY_FRAMES;
                                    score=0; lives=cfg.lives; combo=0; missedFruits=0;
                                    fruits.clear();
                                }
                            }
                        }
                    }
                } else {
                    // Animate halves
                    int df = frameId - f.slashFrame;
                    f.hx1 += f.hvx1; f.hy1 += f.hvy1; f.hvy1 += cfg.gravity*1.2f; f.hrot1 += f.rotSpeed * 1.5f;
                    f.hx2 += f.hvx2; f.hy2 += f.hvy2; f.hvy2 += cfg.gravity*1.2f; f.hrot2 -= f.rotSpeed * 1.5f;
                    if (df > 40 || f.hy1 > cfg.height+80) f.active = false;
                }
            }
            fruits.erase(remove_if(fruits.begin(), fruits.end(),
                [](const Fruit& f){ return !f.active; }), fruits.end());

            // Combo timer
            if (comboTimer > 0) comboTimer--;
            else if (combo > 0) combo = 0;

            // AI slash
            slashTarget = aiSlashTarget(fruits, cfg.width, cfg.height);
            // Smoothly move cursor toward target
            float dx = slashTarget.x - slashCursor.x;
            float dy = slashTarget.y - slashCursor.y;
            float dist = sqrtf(dx*dx+dy*dy);
            float speed = min(dist, 45.f);
            if (dist > 1.f) {
                slashCursor.x += (int)(dx/dist * speed);
                slashCursor.y += (int)(dy/dist * speed);
            }

            // Trail
            slashTrail.push_back({slashCursor, cfg.slashTrailLen});
            if ((int)slashTrail.size() > cfg.slashTrailLen) slashTrail.pop_front();
            for (auto& sp : slashTrail) sp.age--;

            // Check slash collision
            if (slashCooldown > 0) { slashCooldown--; }
            else if (dist < 50.f) {
                // Near target — check hit
                for (auto& f : fruits) {
                    if (!f.active || f.slashed) continue;
                    float fdx = f.x - slashCursor.x;
                    float fdy = f.y - slashCursor.y;
                    float fd  = sqrtf(fdx*fdx+fdy*fdy);
                    if (fd < f.radius + 12) {
                        if (f.type == BOMB) {
                            // Hit bomb!
                            lives--;
                            combo = 0; comboTimer = 0;
                            f.slashed = true; f.slashFrame = frameId;
                            spawnSlashParticles(particles, f.x, f.y, BOMB, rng);
                            scorePopups.push_back({(int)f.x,(int)f.y-30, -25, 30, Scalar(40,40,200)});
                            if (lives <= 0) {
                                ovLine1 = "GAME OVER";
                                ovLine2 = "Score: "+to_string(score)+"   Best: "+to_string(highScore)+"   Restarting...";
                                ovColor = Scalar(40,40,200);
                                showOverlay = true; overlayFrames = OVERLAY_FRAMES;
                                score=0; lives=cfg.lives; combo=0; missedFruits=0;
                                fruits.clear();
                            }
                        } else {
                            // Slash fruit!
                            f.slashed = true; f.slashFrame = frameId;
                            // Setup halves
                            float slashAngle = atan2f((float)(slashCursor.y - slashTarget.y),
                                                      (float)(slashCursor.x - slashTarget.x));
                            float perpX = cosf(slashAngle + CV_PI/2);
                            float perpY = sinf(slashAngle + CV_PI/2);
                            f.hx1=f.x; f.hy1=f.y;
                            f.hvx1 = f.vx + perpX*3.f; f.hvy1 = f.vy - 2.f;
                            f.hrot1 = f.rot;
                            f.hx2=f.x; f.hy2=f.y;
                            f.hvx2 = f.vx - perpX*3.f; f.hvy2 = f.vy - 2.f;
                            f.hrot2 = f.rot;

                            combo++;
                            comboTimer = 45;
                            int pts = (combo > 4) ? 30 : (combo > 2) ? 20 : 10;
                            score += pts;
                            if (score > highScore) highScore = score;
                            missedFruits = 0;

                            spawnSlashParticles(particles, f.x, f.y, f.type, rng);
                            Scalar popC = (pts >= 30) ? Scalar(40,200,255) :
                                          (pts >= 20) ? Scalar(80,200,100) :
                                                        Scalar(200,200,200);
                            scorePopups.push_back({(int)f.x,(int)f.y-30, pts, 30, popC});
                        }
                        slashCooldown = 4;
                        break;
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
        else                                        drawDarkBackground(frame, cfg.width, cfg.height);

        // Stars (only if no custom bg)
        if (staticBg.empty() && !useFolderBg)
            drawStars(frame, frameId, cfg.width, cfg.height, rng);

        // Particles (behind fruits)
        updateAndDrawParticles(frame, particles, cfg.gravity);

        // Draw fruits
        for (const auto& f : fruits) {
            if (!f.active) continue;
            if (!f.slashed) {
                drawFruit(frame, f, frameId);
            } else {
                // Draw halves
                drawFruitHalf(frame, f.hx1, f.hy1, f.hrot1, f.radius, f.type, true);
                drawFruitHalf(frame, f.hx2, f.hy2, f.hrot2, f.radius, f.type, false);
            }
        }

        // Slash trail
        drawSlashTrail(frame, slashTrail, cfg.slashTrailLen);

        // Score popups
        updateAndDrawScorePopups(frame, scorePopups);

        // HUD
        drawHUD(frame, score, highScore, combo, lives, maxLives, currentTime);

        // Subtitle
        int si = frameSub[frameId];
        if (si >= 0)
            drawSubtitle(frame, subs[si].text, cfg.width, cfg.height,
                         cfg.fontSize, cfg.subtitleY, cfg.subtitleOffset);

        // Overlay
        if (showOverlay)
            drawOverlayMessage(frame, ovLine1, ovLine2, cfg.width, cfg.height, ovColor);

        video.write(frame.clone());

        if (frameId % (cfg.fps * 10) == 0) {
            printf("[%5.1f%%] frame %d/%d | score %d | best %d | lives %d | combo %d\n",
                   100.0*frameId/totalFrames, frameId, totalFrames,
                   score, highScore, lives, combo);
        }
    }

    delete bgMgr;
    cout << "\nDone! => " << cfg.outputPath << "\n";
    cout << "Final best score: " << highScore << "\n";
    return 0;
}
