// ============================================================================
// Chrome Dino Runner Simulator — OpenCV + FreeType2
// Usage: ./dino_runner -s subtitles.srt -o output.mp4 [options]
// Build: g++ dino_runner.cpp -o dino_runner \
//          $(pkg-config --cflags --libs opencv4) \
//          -std=c++17 -O2 -pthread
// ============================================================================

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
    double start, end;
    string text;
};

struct Config {
    string outputPath       = "dino_runner.mp4";
    string subtitleFile     = "subtitles.srt";
    string fontFile         = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
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

    // Dino physics
    float  gravity          = 1.1f;
    float  jumpStrength     = -18.0f;
    float  initialSpeed     = 6.0f;
    float  speedIncrement   = 0.0015f;   // speed increases each frame
    float  maxSpeed         = 20.0f;
    int    dinoX            = 180;
    int    dinoW            = 56;
    int    dinoH            = 60;
    int    duckH            = 36;        // height when ducking
    int    lives            = 3;
    int    duration         = 60;   // seconds, used when no subtitle file
};

// Obstacle types
enum ObstacleType { CACTUS_SMALL, CACTUS_LARGE, CACTUS_CLUSTER, PTERODACTYL };

struct Obstacle {
    float x;
    ObstacleType type;
    int w, h;
    float flyY;        // for pterodactyl: Y center
    int flapPhase;
    bool passed;
};

struct Cloud {
    float x;
    int   y;
    int   w;
    float speed;
};

struct StarParticle {
    float x, y, vx, vy, alpha, size;
};

// ============================================================================
// Parse Arguments
// ============================================================================

void printUsage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "  -o, --output <path>        Output video (default: dino_runner.mp4)\n"
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
         << "  --gravity <float>          Gravity (default: 1.1)\n"
         << "  --jump <float>             Jump strength (default: -18.0)\n"
         << "  --speed <float>            Initial speed (default: 6.0)\n"
         << "  --lives <int>              Starting lives (default: 3)\n"
         << "  --subtitle-y <int>         Subtitle Y margin from bottom\n"
         << "  --duration <int>           Video duration in seconds (used when no -s, default: 60)\n"
         << "  --threads <int>            Number of threads\n"
         << "  --help                     Show this help\n";
}

Config parseArguments(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if      (arg=="-o"||arg=="--output")     { if(i+1<argc) cfg.outputPath      = argv[++i]; }
        else if (arg=="-s"||arg=="--subtitles")  { if(i+1<argc) cfg.subtitleFile    = argv[++i]; }
        else if (arg=="-b"||arg=="--background") { if(i+1<argc) cfg.backgroundFile  = argv[++i]; }
        else if (arg=="--bg-folder")             { if(i+1<argc) cfg.backgroundFolder= argv[++i]; }
        else if (arg=="--bg-interval")           { if(i+1<argc) cfg.bgChangeInterval= stoi(argv[++i]); }
        else if (arg=="--bg-opacity")            { if(i+1<argc) cfg.bgOpacity       = stod(argv[++i]); }
        else if (arg=="--font")                  { if(i+1<argc) cfg.fontFile        = argv[++i]; }
        else if (arg=="--font-size")             { if(i+1<argc) cfg.fontSize        = stoi(argv[++i]); }
        else if (arg=="-w"||arg=="--width")      { if(i+1<argc) cfg.width           = stoi(argv[++i]); }
        else if (arg=="-h"||arg=="--height")     { if(i+1<argc) cfg.height          = stoi(argv[++i]); }
        else if (arg=="-f"||arg=="--fps")        { if(i+1<argc) cfg.fps             = stoi(argv[++i]); }
        else if (arg=="--seed")                  { if(i+1<argc) cfg.seed            = stoi(argv[++i]); }
        else if (arg=="--gravity")               { if(i+1<argc) cfg.gravity         = stof(argv[++i]); }
        else if (arg=="--jump")                  { if(i+1<argc) cfg.jumpStrength    = stof(argv[++i]); }
        else if (arg=="--speed")                 { if(i+1<argc) cfg.initialSpeed    = stof(argv[++i]); }
        else if (arg=="--lives")                 { if(i+1<argc) cfg.lives           = stoi(argv[++i]); }
        else if (arg=="--subtitle-y")            { if(i+1<argc) cfg.subtitleY       = stoi(argv[++i]); }
        else if (arg=="--subtitle-offset")       { if(i+1<argc) cfg.subtitleOffset  = stoi(argv[++i]); }
        else if (arg=="--duration")              { if(i+1<argc) cfg.duration        = stoi(argv[++i]); }
        else if (arg=="--threads")               { if(i+1<argc) cfg.numThreads      = stoi(argv[++i]); }
        else if (arg=="--help")                  { printUsage(argv[0]); exit(0); }
        else { cerr << "Unknown: " << arg << "\n"; printUsage(argv[0]); exit(1); }
    }
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
        DIR* dir = opendir(folder.c_str()); if (!dir) return;
        struct dirent* e;
        while ((e = readdir(dir)) != nullptr) {
            string n = e->d_name;
            if (n.size() > 4) {
                string ext = n.substr(n.size()-4);
                transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext==".jpg"||ext==".png"||ext==".bmp"||ext==".jpeg")
                    imgs_.push_back(folder+"/"+n);
            }
        }
        closedir(dir);
        shuffle(imgs_.begin(), imgs_.end(), rng_);
    }
    Mat next() {
        if (imgs_.empty()) return Mat();
        Mat raw = imread(imgs_[idx_]); idx_ = (idx_+1) % imgs_.size();
        if (raw.empty()) return next();
        Mat bg; resize(raw, bg, Size(width_, height_));
        if (opacity_ < 1.0) bg.convertTo(bg, -1, opacity_, 0);
        return bg;
    }
    bool has() const { return !imgs_.empty(); }
private:
    vector<string> imgs_; size_t idx_=0;
    int width_, height_; double opacity_; mt19937 rng_;
};

// ============================================================================
// Parse SRT
// ============================================================================

string readFileUTF8(const string& fn) {
    ifstream f(fn, ios::binary); if (!f) return "";
    return {istreambuf_iterator<char>(f), istreambuf_iterator<char>()};
}

double timeToSec(const string& t) {
    int h,m,s,ms; char dot;
    stringstream ss(t); ss>>h; ss.get(); ss>>m; ss.get(); ss>>s; ss.get(dot); ss>>ms;
    return h*3600+m*60+s+ms/1000.0;
}

vector<Subtitle> parseSRT(const string& fn) {
    vector<Subtitle> subs;
    string data = readFileUTF8(fn);
    if (data.empty()) { cerr << "Cannot open subtitle: " << fn << "\n"; return subs; }
    replace(data.begin(), data.end(), '\r', '\n');
    size_t pos;
    while ((pos = data.find("\n\n\n")) != string::npos) data.replace(pos,3,"\n\n");
    auto parseBlock = [&](const string& blk){
        stringstream ss(blk); string idx,line,tl;
        getline(ss,idx);
        while (getline(ss,line) && line.find(" --> ")==string::npos) {}
        tl = line;
        string text; while(getline(ss,line)){if(!text.empty())text+=" ";text+=line;}
        size_t ap = tl.find(" --> "); if (ap==string::npos) return;
        string st = tl.substr(0,ap), en = tl.substr(ap+5);
        auto trim=[](string& s){size_t f=s.find_first_not_of(" \t"),l=s.find_last_not_of(" \t");
                                if(f!=string::npos)s=s.substr(f,l-f+1);};
        trim(st); trim(en);
        subs.push_back({timeToSec(st), timeToSec(en), text});
    };
    stringstream cs(data); string blk,line;
    while(getline(cs,line)){
        if(line.empty()){if(!blk.empty()){parseBlock(blk);blk.clear();}}
        else{if(!blk.empty())blk+="\n";blk+=line;}
    }
    if(!blk.empty()) parseBlock(blk);
    return subs;
}

// ============================================================================
// FreeType2 subtitle
// ============================================================================

static Ptr<freetype::FreeType2> g_ft2;

void initFreeType(const string& f) {
    g_ft2 = freetype::createFreeType2();
    g_ft2->loadFontData(f, 0);
}

struct FtMetrics { int ascender, descender; };
static FtMetrics getFtMetrics(int sz) {
    static map<int,FtMetrics> cache;
    auto it=cache.find(sz); if(it!=cache.end()) return it->second;
    FtMetrics m;
    if(g_ft2){ int bl=0; Size s=g_ft2->getTextSize("Ágy|",sz,-1,&bl); m.ascender=s.height; m.descender=bl; }
    else { m.ascender=(int)(sz*.75); m.descender=(int)(sz*.25); }
    cache[sz]=m; return m;
}

void drawSubtitle(Mat& frame, const string& text, int width, int height,
                  int fontSize, int fixedY, int subtitleOffset) {
    vector<string> lines; string current;
    int maxWidth = width - 120;
    { stringstream ws(text); string word;
      while(ws>>word){
          string test = current.empty() ? word : current+" "+word;
          int mw = g_ft2 ? g_ft2->getTextSize(test,fontSize,-1,nullptr).width : (int)test.size()*(fontSize/2);
          if(mw<=maxWidth) current=test;
          else{if(!current.empty())lines.push_back(current);current=word;}
      }
      if(!current.empty()) lines.push_back(current);
    }
    if(lines.empty()) return;
    FtMetrics fm = getFtMetrics(fontSize);
    int nL = (int)lines.size(), maxLW = 0;
    vector<int> lw(nL);
    for(int i=0;i<nL;i++){
        lw[i]=g_ft2?g_ft2->getTextSize(lines[i],fontSize,-1,nullptr).width:(int)lines[i].size()*(fontSize/2);
        maxLW=max(maxLW,lw[i]);
    }
    const int padX=20, padYTop=10, padYBot=12, leading=6;
    int lineH = fm.ascender+fm.descender+leading;
    int boxW = min(maxLW+padX*2, width-40);
    int boxH = padYTop+nL*lineH-leading+padYBot;
    int boxX = (width-boxW)/2;
    int bm = (fixedY>=0)?fixedY:80;
    int boxY = max(4, height-bm-boxH);
    { int rx1=max(0,boxX),ry1=max(0,boxY),rx2=min(width,boxX+boxW),ry2=min(height,boxY+boxH);
      if(rx2>rx1&&ry2>ry1){Mat roi=frame(Rect(rx1,ry1,rx2-rx1,ry2-ry1));
        Mat dark(roi.size(),roi.type(),Scalar(0,0,0));addWeighted(roi,.30,dark,.70,0,roi);}}
    rectangle(frame,cv::Point(boxX,boxY),cv::Point(boxX+boxW,boxY+boxH),Scalar(120,120,120),1);
    int offset=(subtitleOffset>=0)?subtitleOffset:fontSize;
    int baseY = boxY+padYTop+fm.ascender-offset;
    for(int i=0;i<nL;i++){
        int tx=(width-lw[i])/2, ty=baseY+i*lineH;
        if(g_ft2) g_ft2->putText(frame,lines[i],cv::Point(tx,ty),fontSize,Scalar(255,255,255),-1,LINE_AA,false);
        else putText(frame,lines[i],cv::Point(tx,ty),FONT_HERSHEY_SIMPLEX,fontSize/30.0,Scalar(255,255,255),2,LINE_AA);
    }
}

// ============================================================================
// Async Video Writer
// ============================================================================

class AsyncVideoWriter {
public:
    AsyncVideoWriter(const string& p, int fourcc, double fps, Size sz)
        : writer_(p,fourcc,fps,sz), done_(false) {
        thread_ = thread([this]{ loop(); });
    }
    ~AsyncVideoWriter() {
        {unique_lock<mutex> lk(mtx_); done_=true;} cv_.notify_one(); thread_.join();
    }
    bool isOpened() const { return writer_.isOpened(); }
    void write(Mat frame) {
        unique_lock<mutex> lk(mtx_);
        cvFull_.wait(lk,[this]{return queue_.size()<12||done_;});
        queue_.push(move(frame)); cv_.notify_one();
    }
private:
    void loop() {
        while(true){
            Mat f; {unique_lock<mutex> lk(mtx_); cv_.wait(lk,[this]{return!queue_.empty()||done_;});
                if(queue_.empty()&&done_) break; f=move(queue_.front()); queue_.pop(); cvFull_.notify_one();}
            writer_.write(f);
        }
        while(!queue_.empty()){writer_.write(queue_.front());queue_.pop();}
    }
    VideoWriter writer_; queue<Mat> queue_; mutex mtx_;
    condition_variable cv_, cvFull_; thread thread_; bool done_;
};

// ============================================================================
// Drawing Helpers
// ============================================================================

// ── Desert sky gradient (day/night cycle based on score) ────────────────────
void drawDesertBackground(Mat& frame, int width, int height, int score) {
    // Shift from warm afternoon → dusk → night as score increases
    double cycle = fmod(score / 200.0, 1.0); // 0=day, 0.5=dusk, 1=night
    for (int y = 0; y < height; y++) {
        double r = (double)y / height;
        uchar br, bg, bb;
        if (cycle < 0.4) {
            // Day: bright sky
            br = (uchar)(max(0.0, 220*(1-r) + 255*r - cycle*200));
            bg = (uchar)(max(0.0, 190*(1-r) + 220*r - cycle*80));
            bb = (uchar)(max(0.0, 250*(1-r) + 200*r));
        } else if (cycle < 0.7) {
            // Dusk: orange/purple
            double d = (cycle-0.4)/0.3;
            br = (uchar)(180*(1-d)+80*d);
            bg = (uchar)(120*(1-d)+50*d);
            bb = (uchar)(160*(1-d)+120*d);
        } else {
            // Night: dark blue
            double n = (cycle-0.7)/0.3;
            br = (uchar)(80*(1-n)+15*n);
            bg = (uchar)(50*(1-n)+20*n);
            bb = (uchar)(120*(1-n)+50*n);
        }
        frame.row(y).setTo(Scalar(bb, bg, br));
    }
}

// ── Stars (night only) ───────────────────────────────────────────────────────
void drawStars(Mat& frame, const vector<pair<int,int>>& stars, int width, int height, int score) {
    double cycle = fmod(score / 200.0, 1.0);
    if (cycle < 0.5) return;
    double alpha = min(1.0, (cycle-0.5)/0.3);
    for (auto& [sx,sy] : stars) {
        uchar brightness = (uchar)(200 * alpha);
        if (sy >= 0 && sy < height && sx >= 0 && sx < width)
            circle(frame, cv::Point(sx,sy), 1, Scalar(brightness,brightness,brightness), -1, LINE_AA);
    }
}

// ── Ground + scrolling track lines ──────────────────────────────────────────
void drawGround(Mat& frame, int groundY, int width, int scrollOffset) {
    // Desert sand color
    rectangle(frame, cv::Point(0, groundY),
              cv::Point(width, frame.rows), Scalar(180, 200, 210), -1);
    // Top edge dark line
    rectangle(frame, cv::Point(0, groundY),
              cv::Point(width, groundY+3), Scalar(100, 120, 130), -1);
    // Scrolling pebbles/cracks
    for (int x = (-scrollOffset % 80 + 80) % 80; x < width; x += 80) {
        circle(frame, cv::Point(x+10, groundY+12), 3, Scalar(150,168,178), -1);
        circle(frame, cv::Point(x+50, groundY+20), 2, Scalar(155,173,183), -1);
    }
    // Subtle horizon shimmer line
    line(frame, cv::Point(0, groundY-1), cv::Point(width, groundY-1),
         Scalar(220,230,235), 1);
}

// ── Draw pixel-art style cactus ──────────────────────────────────────────────
void drawCactus(Mat& frame, int x, int groundY, ObstacleType type, int w, int h) {
    Scalar dark(30,  120,  50);
    Scalar mid (50,  160,  70);
    Scalar lite(100, 200, 110);

    auto drawCactusUnit = [&](int cx, int baseY, int cw, int ch) {
        // Trunk
        rectangle(frame, cv::Point(cx+cw/3, baseY-ch), cv::Point(cx+2*cw/3, baseY), dark, -1);
        rectangle(frame, cv::Point(cx+cw/3+2, baseY-ch), cv::Point(cx+cw/3+5, baseY), lite, -1);
        // Top crown
        ellipse(frame, cv::Point(cx+cw/2, baseY-ch), Size(cw/2, cw/3), 0,0,360, dark,-1,LINE_AA);
        // Arms
        int armY = baseY - ch*6/10;
        // Left arm
        rectangle(frame, cv::Point(cx, armY-cw/3), cv::Point(cx+cw/3, armY), dark, -1);
        rectangle(frame, cv::Point(cx, armY-cw/3+1), cv::Point(cx+2, armY), lite, -1);
        ellipse(frame, cv::Point(cx, armY-cw/3), Size(cw/4, cw/5), 0,0,360, dark,-1,LINE_AA);
        // Right arm
        rectangle(frame, cv::Point(cx+2*cw/3, armY-cw/4), cv::Point(cx+cw, armY), dark, -1);
        ellipse(frame, cv::Point(cx+cw, armY-cw/4), Size(cw/4, cw/5), 0,0,360, dark,-1,LINE_AA);
    };

    if (type == CACTUS_SMALL) {
        drawCactusUnit(x, groundY, w, h);
    } else if (type == CACTUS_LARGE) {
        drawCactusUnit(x, groundY, w, h);
        // Extra top spikes
        for (int i = 0; i < 5; i++)
            line(frame, cv::Point(x+w/2+i*3-6, groundY-h),
                 cv::Point(x+w/2+i*3-6, groundY-h-8), lite, 2);
    } else { // CLUSTER
        int unit = w/3;
        drawCactusUnit(x,      groundY, unit, h-10);
        drawCactusUnit(x+unit, groundY, unit, h+8);
        drawCactusUnit(x+2*unit, groundY, unit, h-5);
    }
}

// ── Draw Pterodactyl ─────────────────────────────────────────────────────────
void drawPterodactyl(Mat& frame, int x, int y, int w, int h, int flapPhase) {
    Scalar body(60, 60, 100);
    Scalar wing(80, 80, 130);
    Scalar lite(120,120,180);

    int cx = x + w/2, cy = y;

    // Body
    ellipse(frame, cv::Point(cx, cy), Size(w/3, h/3), 0,0,360, body,-1,LINE_AA);

    // Head/beak
    ellipse(frame, cv::Point(cx+w/3, cy-h/5), Size(h/3, h/4), -20,0,360, body,-1,LINE_AA);
    // Beak
    vector<cv::Point> beak = {{cx+w/2, cy-h/4}, {cx+w/2+16, cy-h/5}, {cx+w/2, cy-h/8}};
    fillConvexPoly(frame, beak, body);
    // Eye
    circle(frame, cv::Point(cx+w/3+4, cy-h/4), 3, Scalar(255,255,255),-1,LINE_AA);
    circle(frame, cv::Point(cx+w/3+5, cy-h/4), 2, Scalar(20,20,20),-1,LINE_AA);

    // Wings — animated flap
    vector<cv::Point> leftWing, rightWing;
    int wingFlapY = (flapPhase==0) ? h/3 : (flapPhase==1) ? 0 : -h/3;
    leftWing  = {{cx-2, cy}, {cx-w/2-8, cy+wingFlapY+4}, {cx-w/2, cy+h/5}};
    rightWing = {};  // pterodactyl only shows 1 wing side (2D)
    fillConvexPoly(frame, leftWing, wing);
    polylines(frame, leftWing, true, lite, 1, LINE_AA);

    // Tail
    vector<cv::Point> tail = {{cx-w/3, cy}, {cx-w/2-4, cy+h/4}, {cx-w/3+4, cy+h/5}};
    fillConvexPoly(frame, tail, body);
}

// ── Draw the Dino ─────────────────────────────────────────────────────────────
void drawDino(Mat& frame, int x, int groundY, float dinoY, bool ducking,
              int dinoW, int dinoH, int duckH, int frameId, bool isDead) {
    int fy = (int)dinoY;
    int dh = ducking ? duckH : dinoH;
    int dw = ducking ? dinoW + 20 : dinoW;
    int top = fy - dh;

    Scalar body(55,  55,  55);
    Scalar lite(90,  90,  90);
    Scalar dark(30,  30,  30);
    Scalar white(240,240,240);
    Scalar accent(160,160,160);

    if (isDead) {
        // Flash red on death
        body = Scalar(60,60,180);
        lite = Scalar(80,80,220);
    }

    // Body
    rectangle(frame, cv::Point(x, top), cv::Point(x+dw, fy), body, -1);
    // Highlight strip
    rectangle(frame, cv::Point(x+4, top+2), cv::Point(x+8, fy-4), lite, -1);

    if (!ducking) {
        // Head
        int headW = dw-4, headH = dh/3;
        int hx = x + dw/2 - headW/2;
        int hy = top - headH;
        rectangle(frame, cv::Point(hx,hy), cv::Point(hx+headW, top), body, -1);
        rectangle(frame, cv::Point(hx+2,hy+2), cv::Point(hx+4, top-2), lite, -1);
        // Eye (blink if dead)
        if (!isDead) {
            circle(frame, cv::Point(hx+headW-10, hy+headH/2), 5, white,-1,LINE_AA);
            circle(frame, cv::Point(hx+headW-8,  hy+headH/2), 3, dark,-1,LINE_AA);
        } else {
            // X eyes
            line(frame, cv::Point(hx+headW-13,hy+headH/2-3), cv::Point(hx+headW-7,hy+headH/2+3), Scalar(60,60,200), 2);
            line(frame, cv::Point(hx+headW-7, hy+headH/2-3), cv::Point(hx+headW-13,hy+headH/2+3), Scalar(60,60,200), 2);
        }
        // Mouth dashes
        line(frame, cv::Point(hx+headW-2,hy+headH*2/3), cv::Point(hx+headW+4,hy+headH*2/3), dark, 2);

        // Neck bump
        rectangle(frame, cv::Point(x+dw-8, top), cv::Point(x+dw+4, top+dh/5), body,-1);
    } else {
        // Ducking head — more to the right
        int hx = x+dw-16; int hy = top-16;
        rectangle(frame, cv::Point(hx,hy), cv::Point(hx+dw/2+4, top), body,-1);
        circle(frame, cv::Point(hx+dw/2-4, hy+8), 4, white,-1,LINE_AA);
        circle(frame, cv::Point(hx+dw/2-2, hy+8), 3, dark,-1,LINE_AA);
    }

    // Legs — walking animation
    int legPhase = (frameId/4) % 2;
    int legW = 12, legH = dh/3;
    if (!ducking) {
        // Front leg
        int lx1 = x + dw/2 + 4;
        rectangle(frame, cv::Point(lx1, fy),
                  cv::Point(lx1+legW, fy + (legPhase==0?legH:legH/2)), body,-1);
        // Back leg
        int lx2 = x + dw/4 - 4;
        rectangle(frame, cv::Point(lx2, fy),
                  cv::Point(lx2+legW, fy + (legPhase==1?legH:legH/2)), body,-1);
    }

    // Tiny tail
    vector<cv::Point> tail = {{x-4,top+dh/2},{x-14,top+dh*2/3},{x,top+dh*3/4}};
    fillConvexPoly(frame, tail, body);
}

// ── Pixel-art digit display ─────────────────────────────────────────────────
// 7-segment-ish HUD using putText with a monospace retro vibe
static void drawRoundRect(Mat& frame, cv::Point tl, cv::Point br, int r,
                          const Scalar& col, int thickness=-1) {
    rectangle(frame, cv::Point(tl.x+r,tl.y), cv::Point(br.x-r,br.y), col, thickness);
    rectangle(frame, cv::Point(tl.x,tl.y+r), cv::Point(br.x,br.y-r), col, thickness);
    circle(frame, cv::Point(tl.x+r,tl.y+r), r, col, thickness, LINE_AA);
    circle(frame, cv::Point(br.x-r,tl.y+r), r, col, thickness, LINE_AA);
    circle(frame, cv::Point(tl.x+r,br.y-r), r, col, thickness, LINE_AA);
    circle(frame, cv::Point(br.x-r,br.y-r), r, col, thickness, LINE_AA);
}

static void drawPillCard(Mat& frame, int cx, int cy, int w, int h, int r,
                         const string& label, const string& value,
                         const Scalar& bgC, const Scalar& bdC,
                         const Scalar& lbC, const Scalar& vlC) {
    cv::Point tl(cx-w/2,cy-h/2), br(cx+w/2,cy+h/2);
    Mat roi = frame(Rect(tl.x,tl.y,w,h));
    Mat dk(roi.size(),roi.type(),bgC); addWeighted(roi,.20,dk,.80,0,roi);
    drawRoundRect(frame,tl,br,r,bdC,1);
    int bl=0;
    Size ls=getTextSize(label,FONT_HERSHEY_SIMPLEX,.42,1,&bl);
    putText(frame,label,cv::Point(cx-ls.width/2,cy-6),FONT_HERSHEY_SIMPLEX,.42,lbC,1,LINE_AA);
    Size vs=getTextSize(value,FONT_HERSHEY_SIMPLEX,.85,2,&bl);
    putText(frame,value,cv::Point(cx-vs.width/2,cy+18),FONT_HERSHEY_SIMPLEX,.85,vlC,2,LINE_AA);
}

void drawHUD(Mat& frame, int score, int highScore, int lives, float speed, double t) {
    int fw = frame.cols;
    const Scalar bgDark(10,10,18), bdDim(70,70,90);
    const Scalar gold(30,215,255), silver(180,180,180), cyan(220,220,50), white(255,255,255), red(80,80,220);

    drawPillCard(frame, 90,44,160,60,12, "SCORE",to_string(score), bgDark,bdDim,silver,gold);
    drawPillCard(frame,262,44,160,60,12, "BEST", to_string(highScore), bgDark,bdDim,silver,cyan);

    char spBuf[16]; snprintf(spBuf,sizeof(spBuf),"%.1f",speed);
    drawPillCard(frame,434,44,140,60,12, "SPEED",string(spBuf)+"x", bgDark,bdDim,silver,white);

    int secs=(int)t, mins=secs/60; secs%=60;
    char tb[16]; snprintf(tb,sizeof(tb),"%02d:%02d",mins,secs);
    drawPillCard(frame,586,44,140,60,12, "TIME",string(tb), bgDark,bdDim,silver,silver);

    // Lives — dino icon dots
    const int maxL=5, dotR=9, dotGap=26;
    int livesW=maxL*dotGap, lx0=fw-livesW-16, ly=44;
    cv::Point ltl(lx0-10,ly-30), lbr(fw-8,ly+30);
    { Mat roi=frame(Rect(ltl.x,ltl.y,lbr.x-ltl.x,lbr.y-ltl.y));
      Mat dk(roi.size(),roi.type(),bgDark); addWeighted(roi,.20,dk,.80,0,roi); }
    drawRoundRect(frame,ltl,lbr,12,bdDim,1);
    int bl2=0; Size ls2=getTextSize("LIVES",FONT_HERSHEY_SIMPLEX,.42,1,&bl2);
    putText(frame,"LIVES",cv::Point((ltl.x+lbr.x)/2-ls2.width/2,ly-8),FONT_HERSHEY_SIMPLEX,.42,silver,1,LINE_AA);
    for(int i=0;i<maxL;i++){
        int hx=lx0+i*dotGap+dotGap/2;
        // Mini dino silhouette dot
        Scalar col = (i<lives) ? Scalar(60,180,60) : Scalar(50,50,60);
        circle(frame, cv::Point(hx, ly+12), dotR, col,-1,LINE_AA);
        if(i<lives)
            circle(frame, cv::Point(hx+3, ly+8), 3, Scalar(180,220,180),-1,LINE_AA);
    }
}

// ── Dust particle trail ──────────────────────────────────────────────────────
struct DustParticle { float x, y, vx, vy, alpha, size; };

void spawnDust(vector<DustParticle>& dust, int x, int y, mt19937& rng) {
    uniform_real_distribution<float> dx(-2.5f, -0.5f);
    uniform_real_distribution<float> dy(-1.5f, 0.5f);
    uniform_real_distribution<float> sz(2.0f, 6.0f);
    for (int i = 0; i < 3; i++)
        dust.push_back({(float)x, (float)y, dx(rng), dy(rng), 1.0f, sz(rng)});
}

void updateDrawDust(Mat& frame, vector<DustParticle>& dust) {
    for (auto& p : dust) {
        p.x += p.vx; p.y += p.vy; p.alpha -= 0.06f;
        if (p.alpha <= 0) continue;
        uchar v = (uchar)(180 * p.alpha);
        circle(frame, cv::Point((int)p.x,(int)p.y), (int)p.size, Scalar(v,v,v),-1,LINE_AA);
    }
    dust.erase(remove_if(dust.begin(),dust.end(),[](const DustParticle& p){return p.alpha<=0;}), dust.end());
}

// ── Score pop effect ─────────────────────────────────────────────────────────
struct ScorePop { int x,y,score; float alpha; };

void drawScorePops(Mat& frame, vector<ScorePop>& pops) {
    for (auto& sp : pops) {
        sp.y -= 1.5f; sp.alpha -= 0.025f;
        if (sp.alpha <= 0) continue;
        uchar a = (uchar)(255*sp.alpha);
        string s = "+" + to_string(sp.score);
        int bl=0; Size sz=getTextSize(s,FONT_HERSHEY_SIMPLEX,0.7,2,&bl);
        putText(frame, s, cv::Point(sp.x-sz.width/2,(int)sp.y),
                FONT_HERSHEY_SIMPLEX, 0.7, Scalar(a,a,30), 2, LINE_AA);
    }
    pops.erase(remove_if(pops.begin(),pops.end(),[](const ScorePop& p){return p.alpha<=0;}),pops.end());
}

// ── Cloud scrolling ──────────────────────────────────────────────────────────
void updateDrawClouds(Mat& frame, vector<Cloud>& clouds, int width, float gameSpeed,
                      mt19937& rng, int score) {
    uniform_int_distribution<int> cloudY(60,200);
    uniform_int_distribution<int> cloudW(80,200);

    // Spawn clouds
    static int nextCloudX = 1400;
    static int cloudTimer = 0;
    cloudTimer++;
    if (cloudTimer > 60) {
        cloudTimer=0;
        uniform_int_distribution<int> gap(200,500);
        clouds.push_back({(float)width+50, cloudY(rng), cloudW(rng), gameSpeed*0.3f});
    }

    // Color based on day/night cycle
    double cycle = fmod(score/200.0,1.0);
    uchar cv_ = (cycle < 0.5) ? 255 : (uchar)(255*(1.0-(cycle-0.5)/0.3));
    Scalar cloudCol(cv_,cv_,cv_);

    for (auto& c : clouds) {
        c.x -= c.speed;
        int cx=(int)c.x, cy=c.y, cw=c.w;
        ellipse(frame,cv::Point(cx,   cy),    Size(cw/2,cw/5),0,0,360,cloudCol,-1,LINE_AA);
        ellipse(frame,cv::Point(cx-cw/3,cy+5),Size(cw/4,cw/6),0,0,360,cloudCol,-1,LINE_AA);
        ellipse(frame,cv::Point(cx+cw/3,cy+5),Size(cw/3,cw/6),0,0,360,cloudCol,-1,LINE_AA);
    }
    clouds.erase(remove_if(clouds.begin(),clouds.end(),[](const Cloud& c){return c.x<-250;}),clouds.end());
}

// ── Overlay (death/milestone) ────────────────────────────────────────────────
void drawOverlayMessage(Mat& frame, const string& l1, const string& l2,
                        int w, int h, const Scalar& col) {
    Mat ov; frame.copyTo(ov);
    rectangle(ov,cv::Point(w/2-340,h/2-90),cv::Point(w/2+340,h/2+90),Scalar(5,5,5),-1);
    addWeighted(ov,.82,frame,.18,0,frame);
    rectangle(frame,cv::Point(w/2-340,h/2-90),cv::Point(w/2+340,h/2+90),col,2);
    int bl=0;
    Size s1=getTextSize(l1,FONT_HERSHEY_SIMPLEX,1.5,3,&bl);
    putText(frame,l1,cv::Point(w/2-s1.width/2,h/2-16),FONT_HERSHEY_SIMPLEX,1.5,col,3,LINE_AA);
    Size s2=getTextSize(l2,FONT_HERSHEY_SIMPLEX,.78,2,&bl);
    putText(frame,l2,cv::Point(w/2-s2.width/2,h/2+50),FONT_HERSHEY_SIMPLEX,.78,Scalar(190,190,190),2,LINE_AA);
}

// ── Milestone flash (100, 200, 500...) ──────────────────────────────────────
void drawMilestoneFlash(Mat& frame, int framesSince, int w, int h, int milestone) {
    if (framesSince > 30) return;
    double alpha = 1.0 - framesSince/30.0;
    string msg = "SCORE " + to_string(milestone) + "!";
    int bl=0; Size s=getTextSize(msg,FONT_HERSHEY_SIMPLEX,1.1,3,&bl);
    uchar a = (uchar)(255*alpha);
    putText(frame, msg, cv::Point(w/2-s.width/2, h/2-60),
            FONT_HERSHEY_SIMPLEX, 1.1, Scalar(a, a, (uchar)(200*alpha)), 3, LINE_AA);
}

// ============================================================================
// AI Controller: jump or duck?
// ============================================================================

struct DinoAction { bool jump; bool duck; };

DinoAction aiDecide(float dinoY, float dinoVY, float groundY,
                    const vector<Obstacle>& obs, int dinoX, int dinoW, int dinoH,
                    float gameSpeed) {
    DinoAction a{false, false};
    const Obstacle* nearest = nullptr;
    for (const auto& o : obs) {
        if ((int)o.x + o.w > dinoX - 4) {
            if (!nearest || o.x < nearest->x) nearest = &o;
        }
    }
    if (!nearest) return a;

    float dist = nearest->x - (float)(dinoX + dinoW/2);
    float approachTime = (dist > 0) ? dist / gameSpeed : 0.0f;

    bool onGround = (dinoY >= groundY - 1.0f);

    if (nearest->type == PTERODACTYL) {
        // Duck if pterodactyl is at mid height, else jump over it
        float pty = nearest->flyY;
        float midH = (groundY * 0.5f);
        if (pty > midH) {
            // Low pterodactyl — duck under
            a.duck = (dist < gameSpeed * 20);
        } else {
            // High pterodactyl — just run under
        }
    } else {
        // Cactus: estimate future Y and jump if needed
        float lookahead = min(approachTime, 12.0f);
        float futureY = dinoY + dinoVY * lookahead + 0.5f * 1.1f * lookahead * lookahead;
        bool willCollide = (dist > -10 && dist < gameSpeed * 15 && futureY >= groundY - dinoH - 10);
        if (willCollide && onGround) a.jump = true;
    }
    return a;
}

// ============================================================================
// Spawn obstacle
// ============================================================================

Obstacle spawnObstacle(int width, int groundY, float gameSpeed, int score, mt19937& rng) {
    uniform_int_distribution<int> typeDist(0, 100);
    int roll = typeDist(rng);
    ObstacleType t;
    if (score < 30)       t = (roll < 50) ? CACTUS_SMALL : CACTUS_LARGE;
    else if (score < 80)  t = (roll < 33) ? CACTUS_SMALL : (roll < 66) ? CACTUS_LARGE : CACTUS_CLUSTER;
    else                  t = (roll < 25) ? CACTUS_SMALL : (roll < 50) ? CACTUS_LARGE :
                              (roll < 75) ? CACTUS_CLUSTER : PTERODACTYL;

    int w = (t==CACTUS_SMALL)?42 : (t==CACTUS_LARGE)?52 : (t==CACTUS_CLUSTER)?90 : 60;
    int h = (t==CACTUS_SMALL)?55 : (t==CACTUS_LARGE)?75 : (t==CACTUS_CLUSTER)?65 : 38;

    uniform_real_distribution<float> flyH(groundY*0.35f, groundY*0.6f);
    float fy = (t==PTERODACTYL) ? flyH(rng) : 0.0f;

    return {(float)(width+60), t, w, h, fy, 0, false};
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    Config cfg = parseArguments(argc, argv);

    cout << "Chrome Dino Runner Simulator\n"
         << "  Output:    " << cfg.outputPath << "\n"
         << "  Subtitles: " << cfg.subtitleFile << "\n"
         << "  Size:      " << cfg.width << "x" << cfg.height << "\n"
         << "  FPS:       " << cfg.fps << "\n"
         << "  Gravity:   " << cfg.gravity << "\n"
         << "  Jump:      " << cfg.jumpStrength << "\n"
         << "  Speed:     " << cfg.initialSpeed << "\n"
         << "  Lives:     " << cfg.lives << "\n"
         << "  Threads:   " << cfg.numThreads << "\n\n";

    setNumThreads(cfg.numThreads);

    // Background
    Mat staticBg;
    BackgroundManager* bgMgr = nullptr;
    bool useFolderBg = false;
    if (!cfg.backgroundFolder.empty()) {
        bgMgr = new BackgroundManager(cfg.backgroundFolder, cfg.width, cfg.height,
                                      cfg.bgOpacity, cfg.seed<0?42:cfg.seed);
        if (bgMgr->has()) useFolderBg=true;
        else { delete bgMgr; bgMgr=nullptr; }
    }
    if (!useFolderBg && !cfg.backgroundFile.empty()) {
        staticBg = imread(cfg.backgroundFile);
        if (!staticBg.empty()) {
            resize(staticBg,staticBg,Size(cfg.width,cfg.height));
            if (cfg.bgOpacity<1.0) staticBg.convertTo(staticBg,-1,cfg.bgOpacity,0);
        }
    }

    // Font
    { ifstream f(cfg.fontFile); if(f.good()) initFreeType(cfg.fontFile); }

    // Subtitles (optional)
    vector<Subtitle> subs;
    double maxTime = (double)cfg.duration;
    if (!cfg.subtitleFile.empty()) {
        subs = parseSRT(cfg.subtitleFile);
        if (!subs.empty()) {
            cout << "Loaded " << subs.size() << " subtitles\n";
            for (auto& s : subs) maxTime = max(maxTime, s.end);
        } else {
            cout << "Subtitle file not found or empty — rendering without subtitles\n";
        }
    } else {
        cout << "No subtitle file — rendering " << cfg.duration << "s without subtitles\n";
    }
    int totalFrames = (int)(maxTime * cfg.fps);

    // Per-frame subtitle lookup
    vector<int> frameSub(totalFrames, -1);
    if (!subs.empty()) {
        for (int fi=0;fi<totalFrames;fi++){
            double t=(double)fi/cfg.fps;
            for (int si=0;si<(int)subs.size();si++)
                if(subs[si].start<=t&&t<=subs[si].end){frameSub[fi]=si;break;}
        }
    }

    // Video writer
    AsyncVideoWriter video(cfg.outputPath, VideoWriter::fourcc('m','p','4','v'),
                           cfg.fps, Size(cfg.width,cfg.height));
    if (!video.isOpened()) { cerr << "Cannot open video writer!\n"; delete bgMgr; return 1; }

    // RNG
    random_device rd;
    mt19937 rng(cfg.seed<0?rd():cfg.seed);

    // Constants
    const int groundY  = cfg.height - 100;

    // ── Pre-generate stars (static pattern) ──────────────────────────────────
    vector<pair<int,int>> stars;
    uniform_int_distribution<int> sx(0, cfg.width), sy(20, groundY-20);
    for (int i=0;i<120;i++) stars.push_back({sx(rng), sy(rng)});

    // ── Game state ────────────────────────────────────────────────────────────
    float dinoY   = (float)groundY;
    float dinoVY  = 0.0f;
    bool  onGround= true;
    bool  ducking = false;
    int   score   = 0;
    int   highScore= 0;
    int   lives   = cfg.lives;
    float gameSpeed= cfg.initialSpeed;
    int   scrollOff= 0;
    int   deadFrames=0;
    bool  isDead  = false;
    bool  showOverlay=false;
    int   overlayFrames=0;
    string ovLine1,ovLine2; Scalar ovColor;
    int   lastMilestone=0;
    int   milestoneFlash=-99;

    vector<Obstacle> obstacles;
    vector<DustParticle> dust;
    vector<ScorePop> scorePops;
    vector<Cloud> clouds;
    Mat currentBg;
    if (useFolderBg&&bgMgr) currentBg=bgMgr->next();

    // Spawn initial obstacle gap
    uniform_int_distribution<int> gapDist(60,180);
    int nextObstacleX = cfg.width + gapDist(rng)*2;
    const int OVERLAY_FRAMES = cfg.fps*2;

    Mat frame(cfg.height, cfg.width, CV_8UC3);
    cout << "Rendering " << totalFrames << " frames (" << (int)maxTime << "s)...\n";

    for (int frameId=0;frameId<totalFrames;frameId++) {
        double currentTime = (double)frameId/cfg.fps;

        if (useFolderBg&&bgMgr&&frameId%cfg.bgChangeInterval==0)
            currentBg = bgMgr->next();

        // ═══ GAME LOGIC ═══════════════════════════════════════════════════════

        if (!showOverlay) {
            // Increase speed
            gameSpeed = min(cfg.maxSpeed, cfg.initialSpeed + frameId * cfg.speedIncrement);
            scrollOff += (int)gameSpeed;

            // AI decision
            DinoAction act = aiDecide(dinoY, dinoVY, (float)groundY,
                                      obstacles, cfg.dinoX, cfg.dinoW, cfg.dinoH, gameSpeed);
            if (act.jump && onGround) {
                dinoVY = cfg.jumpStrength;
                onGround = false;
                ducking = false;
            }
            ducking = act.duck && onGround;

            // Double-jump (when really fast, allow once)
            if (act.jump && !onGround && gameSpeed > 14.0f && dinoVY > -5.0f) {
                dinoVY = cfg.jumpStrength * 0.7f;
            }

            // Physics
            dinoVY += cfg.gravity;
            dinoY  += dinoVY;
            if (dinoY >= (float)groundY) { dinoY=(float)groundY; dinoVY=0; onGround=true; }

            // Dust when running on ground
            if (onGround && !ducking && frameId%3==0)
                spawnDust(dust, cfg.dinoX - cfg.dinoW/2, groundY, rng);

            // Move obstacles
            for (auto& o : obstacles) {
                o.x -= gameSpeed;
                o.flapPhase = (frameId/5)%3;
                if (!o.passed && o.x + o.w < cfg.dinoX) {
                    o.passed = true;
                    score++;
                    if (score > highScore) highScore = score;
                    // Score pop
                    scorePops.push_back({cfg.dinoX+40, groundY-cfg.dinoH-20, 1, 1.0f});
                    // Milestone check
                    int mil = (score / 100) * 100;
                    if (mil > lastMilestone && mil > 0) {
                        lastMilestone = mil;
                        milestoneFlash = frameId;
                    }
                }
            }
            obstacles.erase(remove_if(obstacles.begin(),obstacles.end(),
                [](const Obstacle& o){return o.x+o.w<-20;}), obstacles.end());

            // Spawn new obstacles
            while (obstacles.empty() || obstacles.back().x < cfg.width + nextObstacleX) {
                float lastX = obstacles.empty() ? cfg.width : obstacles.back().x;
                uniform_int_distribution<int> gd(300,550);
                int gap = gd(rng) + (int)(200.0f/gameSpeed); // larger gap at high speed
                nextObstacleX = gap;
                Obstacle no = spawnObstacle(cfg.width, groundY, gameSpeed, score, rng);
                no.x = lastX + gap;
                obstacles.push_back(no);
            }

            // Collision detection
            int dTop    = (int)dinoY - (ducking ? cfg.duckH : cfg.dinoH);
            int dBot    = (int)dinoY;
            int dLeft   = cfg.dinoX - cfg.dinoW/4;
            int dRight  = cfg.dinoX + (ducking ? cfg.dinoW+20 : cfg.dinoW) - cfg.dinoW/4;

            for (const auto& o : obstacles) {
                int ox=(int)o.x, ow=o.w, oh=o.h;
                int oTop, oBot, oLeft=ox+4, oRight=ox+ow-4;
                if (o.type==PTERODACTYL) { oTop=(int)o.flyY-oh/2; oBot=(int)o.flyY+oh/2; }
                else { oTop=groundY-oh; oBot=groundY; }

                bool xOverlap = dRight>oLeft && dLeft<oRight;
                bool yOverlap = dBot >oTop   && dTop <oBot;
                if (xOverlap && yOverlap) {
                    lives--;
                    isDead=true; deadFrames=10;
                    if (lives<=0) {
                        ovLine1="GAME OVER";
                        ovLine2="Score: "+to_string(score)+"  Best: "+to_string(highScore)+"  Restarting...";
                        ovColor=Scalar(40,40,220);
                        showOverlay=true; overlayFrames=OVERLAY_FRAMES;
                        score=0; lives=cfg.lives; gameSpeed=cfg.initialSpeed;
                        lastMilestone=0;
                        dinoY=(float)groundY; dinoVY=0; onGround=true; ducking=false;
                        obstacles.clear(); dust.clear(); scorePops.clear();
                    } else {
                        ovLine1="OUCH!";
                        ovLine2="Lives: "+to_string(lives)+"  Keep going!";
                        ovColor=Scalar(60,200,220);
                        showOverlay=true; overlayFrames=OVERLAY_FRAMES/2;
                        dinoY=(float)groundY; dinoVY=0; onGround=true; ducking=false;
                        obstacles.clear();
                    }
                    break;
                }
            }

            if (deadFrames>0) deadFrames--;
            else isDead=false;
        } else {
            overlayFrames--;
            if (overlayFrames<=0) showOverlay=false;
        }

        // ═══ RENDER ═══════════════════════════════════════════════════════════

        // Background
        if (useFolderBg&&!currentBg.empty())  currentBg.copyTo(frame);
        else if (!staticBg.empty())           staticBg.copyTo(frame);
        else                                  drawDesertBackground(frame, cfg.width, cfg.height, score);

        // Stars (night mode)
        if (staticBg.empty()&&!useFolderBg)
            drawStars(frame, stars, cfg.width, cfg.height, score);

        // Clouds
        if (staticBg.empty()&&!useFolderBg)
            updateDrawClouds(frame, clouds, cfg.width, gameSpeed, rng, score);

        // Dust particles
        updateDrawDust(frame, dust);

        // Ground
        drawGround(frame, groundY, cfg.width, scrollOff);

        // Obstacles (draw under dino if behind, over if in front handled naturally by order)
        for (const auto& o : obstacles) {
            if (o.type==PTERODACTYL) {
                drawPterodactyl(frame, (int)o.x, (int)o.flyY, o.w, o.h, o.flapPhase);
            } else {
                drawCactus(frame, (int)o.x, groundY, o.type, o.w, o.h);
            }
        }

        // Dino
        drawDino(frame, cfg.dinoX, groundY, dinoY, ducking,
                 cfg.dinoW, cfg.dinoH, cfg.duckH, frameId, isDead);

        // Score pops
        drawScorePops(frame, scorePops);

        // Milestone flash
        if (milestoneFlash >= 0)
            drawMilestoneFlash(frame, frameId-milestoneFlash, cfg.width, cfg.height, lastMilestone);

        // HUD
        drawHUD(frame, score, highScore, lives, gameSpeed/cfg.initialSpeed, currentTime);

        // Subtitle
        int si=frameSub[frameId];
        if (si>=0)
            drawSubtitle(frame, subs[si].text, cfg.width, cfg.height,
                         cfg.fontSize, cfg.subtitleY, cfg.subtitleOffset);

        // Overlay
        if (showOverlay)
            drawOverlayMessage(frame, ovLine1, ovLine2, cfg.width, cfg.height, ovColor);

        video.write(frame.clone());

        if (frameId%(cfg.fps*10)==0)
            printf("[%5.1f%%] frame %d/%d | score %d | best %d | lives %d | speed %.1f\n",
                   100.0*frameId/totalFrames, frameId, totalFrames,
                   score, highScore, lives, gameSpeed);
    }

    delete bgMgr;
    cout << "\nDone! => " << cfg.outputPath << "\n";
    cout << "Final best score: " << highScore << "\n";
    return 0;
}
