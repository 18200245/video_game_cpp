// ============================================================================
// OpenCV Snake Game Simulator — AI Video Generator
// Usage: ./snake_simulator -s subtitles.srt -o output.mp4 [options]
// Build: g++ snake_simulator.cpp -o snake_simulator \
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
// 1. Structs & Constants
// ============================================================================

struct Subtitle {
    double start, end;
    string text;
};

struct Config {
    // I/O
    string outputPath       = "snake.mp4";
    string subtitleFile     = "";
    string fontFile         = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    string backgroundFile   = "";
    string backgroundFolder = "";
    int    bgChangeInterval = 300;
    double bgOpacity        = 1.0;

    // Video
    int    width            = 1280;
    int    height           = 720;
    int    fps              = 60;
    int    duration         = 60;
    int    seed             = -1;
    int    numThreads       = max(1, (int)thread::hardware_concurrency());

    // Font / Subtitle
    int    fontSize         = 32;
    int    subtitleY        = -1;
    int    subtitleOffset   = -1;

    // Physics & gameplay (Snake specific)
    float  gravity          = 0.0f; // Not used in top-down, but kept for struct compliance
    int    lives            = 3;
    float  initialSpeed     = 8.0f; // Cells per second
    float  speedIncrement   = 0.1f; // Speed increase per apple
    float  maxSpeed         = 25.0f;
    int    cellSize         = 36;
    int    hudHeight        = 80;
};

struct Point2D {
    int x, y;
    bool operator==(const Point2D& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Point2D& o) const { return x != o.x || y != o.y; }
};

enum ObstacleType { OBSTACLE_ROCK, OBSTACLE_WALL };

struct Obstacle {
    Point2D pos;
    ObstacleType type;
};

struct DustParticle { float x, y, vx, vy, alpha, size; };
struct ScorePop { int x; float y; int value; float alpha; };
struct Cloud { float x; int y; int w; float speed; };

// ============================================================================
// 2. Parse Arguments
// ============================================================================

void printUsage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "  -o, --output <path>        Output video (default: snake.mp4)\n"
         << "  -s, --subtitles <path>     Subtitle SRT file (optional)\n"
         << "  --duration <int>           Duration in seconds if no subtitle (default: 60)\n"
         << "  -b, --background <path>    Static background image\n"
         << "  --bg-folder <path>         Folder with backgrounds\n"
         << "  --bg-interval <int>        Change BG every N frames (default: 300)\n"
         << "  --bg-opacity <float>       Background opacity 0.0-1.0 (default: 1.0)\n"
         << "  --font <path>              Font file\n"
         << "  --font-size <int>          Font size (default: 32)\n"
         << "  -w, --width <int>          Video width (default: 1280)\n"
         << "  -h, --height <int>         Video height (default: 720)\n"
         << "  -f, --fps <int>            FPS (default: 60)\n"
         << "  --seed <int>               Random seed\n"
         << "  --threads <int>            Number of render threads\n"
         << "  --subtitle-y <int>         Subtitle Y margin from bottom\n"
         << "  --help                     Show this help\n";
}

Config parseArguments(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if      (arg=="-o"||arg=="--output")     { if(i+1<argc) cfg.outputPath      = argv[++i]; }
        else if (arg=="-s"||arg=="--subtitles")  { if(i+1<argc) cfg.subtitleFile    = argv[++i]; }
        else if (arg=="--duration")              { if(i+1<argc) cfg.duration        = stoi(argv[++i]); }
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
        else if (arg=="--threads")               { if(i+1<argc) cfg.numThreads      = stoi(argv[++i]); }
        else if (arg=="--subtitle-y")            { if(i+1<argc) cfg.subtitleY       = stoi(argv[++i]); }
        else if (arg=="--subtitle-offset")       { if(i+1<argc) cfg.subtitleOffset  = stoi(argv[++i]); }
        else if (arg=="--help")                  { printUsage(argv[0]); exit(0); }
        else { cerr << "Unknown argument: " << arg << "\n"; printUsage(argv[0]); exit(1); }
    }
    return cfg;
}

// ============================================================================
// 3. Background Manager
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
// 4. Parse SRT
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
    if (data.empty()) return subs;
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
// 5. FreeType2 subtitle
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
    
    // AddWeighted ROI (70% dark)
    { int rx1=max(0,boxX),ry1=max(0,boxY),rx2=min(width,boxX+boxW),ry2=min(height,boxY+boxH);
      if(rx2>rx1&&ry2>ry1){Mat roi=frame(Rect(rx1,ry1,rx2-rx1,ry2-ry1));
        Mat dark(roi.size(),roi.type(),Scalar(0,0,0));addWeighted(roi,.30,dark,.70,0,roi);}}
    // Border
    rectangle(frame,Point(boxX,boxY),Point(boxX+boxW,boxY+boxH),Scalar(100,100,100),1);
    
    int offset=(subtitleOffset>=0)?subtitleOffset:fontSize;
    int baseY = boxY+padYTop+fm.ascender-offset;
    for(int i=0;i<nL;i++){
        int tx=(width-lw[i])/2, ty=baseY+i*lineH;
        if(g_ft2) g_ft2->putText(frame,lines[i],Point(tx,ty),fontSize,Scalar(255,255,255),-1,LINE_AA,false);
        else putText(frame,lines[i],Point(tx,ty),FONT_HERSHEY_SIMPLEX,fontSize/30.0,Scalar(255,255,255),2,LINE_AA);
    }
}

// ============================================================================
// 6. AsyncVideoWriter
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
// 7. Drawing Helpers
// ============================================================================

void drawRoundRect(Mat& frame, Point tl, Point br, int r, const Scalar& col, int thickness=-1) {
    rectangle(frame, Point(tl.x+r,tl.y), Point(br.x-r,br.y), col, thickness);
    rectangle(frame, Point(tl.x,tl.y+r), Point(br.x,br.y-r), col, thickness);
    circle(frame, Point(tl.x+r,tl.y+r), r, col, thickness, LINE_AA);
    circle(frame, Point(br.x-r,tl.y+r), r, col, thickness, LINE_AA);
    circle(frame, Point(tl.x+r,br.y-r), r, col, thickness, LINE_AA);
    circle(frame, Point(br.x-r,br.y-r), r, col, thickness, LINE_AA);
}

void drawPillCard(Mat& frame, int cx, int cy, int w, int h, int r,
                  const string& label, const string& value,
                  Scalar bgColor, Scalar borderColor,
                  Scalar labelColor, Scalar valueColor) {
    Point tl(cx-w/2,cy-h/2), br(cx+w/2,cy+h/2);
    Mat roi = frame(Rect(tl.x,tl.y,w,h));
    Mat dk(roi.size(),roi.type(),bgColor); addWeighted(roi,.20,dk,.80,0,roi);
    drawRoundRect(frame,tl,br,r,borderColor,1);
    int bl=0;
    Size ls=getTextSize(label,FONT_HERSHEY_SIMPLEX,.42,1,&bl);
    putText(frame,label,Point(cx-ls.width/2,cy-6),FONT_HERSHEY_SIMPLEX,.42,labelColor,1,LINE_AA);
    Size vs=getTextSize(value,FONT_HERSHEY_SIMPLEX,.85,2,&bl);
    putText(frame,value,Point(cx-vs.width/2,cy+18),FONT_HERSHEY_SIMPLEX,.85,valueColor,2,LINE_AA);
}

void drawHUD(Mat& frame, int score, int highScore, int lives, float speed, double t) {
    int fw = frame.cols;
    const Scalar bgDark(20,25,30), bdDim(70,80,100);
    const Scalar gold(30,215,255), silver(180,180,180), cyan(220,220,50), white(255,255,255);

    drawPillCard(frame, 90,44,160,60,12, "SCORE",to_string(score), bgDark,bdDim,silver,gold);
    drawPillCard(frame,262,44,160,60,12, "BEST", to_string(highScore), bgDark,bdDim,silver,cyan);

    char spBuf[16]; snprintf(spBuf,sizeof(spBuf),"%.1f",speed);
    drawPillCard(frame,434,44,140,60,12, "SPEED",string(spBuf)+"c/s", bgDark,bdDim,silver,white);

    int secs=(int)t, mins=secs/60; secs%=60;
    char tb[16]; snprintf(tb,sizeof(tb),"%02d:%02d",mins,secs);
    drawPillCard(frame,586,44,140,60,12, "TIME",string(tb), bgDark,bdDim,silver,silver);

    // Lives
    int maxL=5, dotR=9, dotGap=26;
    int livesW=maxL*dotGap, lx0=fw-livesW-16, ly=44;
    Point ltl(lx0-10,ly-30), lbr(fw-8,ly+30);
    { Mat roi=frame(Rect(ltl.x,ltl.y,lbr.x-ltl.x,lbr.y-ltl.y));
      Mat dk(roi.size(),roi.type(),bgDark); addWeighted(roi,.20,dk,.80,0,roi); }
    drawRoundRect(frame,ltl,lbr,12,bdDim,1);
    int bl2=0; Size ls2=getTextSize("LIVES",FONT_HERSHEY_SIMPLEX,.42,1,&bl2);
    putText(frame,"LIVES",Point((ltl.x+lbr.x)/2-ls2.width/2,ly-8),FONT_HERSHEY_SIMPLEX,.42,silver,1,LINE_AA);
    for(int i=0;i<maxL;i++){
        int hx=lx0+i*dotGap+dotGap/2;
        Scalar col = (i<lives) ? Scalar(60,220,60) : Scalar(50,60,70);
        circle(frame, Point(hx, ly+12), dotR, col,-1,LINE_AA);
    }
}

void drawGridBackground(Mat& frame, int width, int height, int score, int cs, int offY) {
    // Day/Night cycle background shift
    double cycle = fmod(score / 150.0, 1.0); 
    uchar gDay = 150, gNight = 40;
    uchar bDay = 100, bNight = 30;
    uchar rDay = 110, rNight = 25;
    
    double t = (sin(cycle * 2 * CV_PI) + 1.0) / 2.0; // Smooth transition
    Scalar bgCol(bNight + (bDay-bNight)*t, gNight + (gDay-gNight)*t, rNight + (rDay-rNight)*t);
    frame.setTo(bgCol);

    // Grid Lines (Subtle)
    Scalar lineCol(bgCol[0]+15, bgCol[1]+15, bgCol[2]+15);
    for(int x=0; x<=width; x+=cs) line(frame, Point(x,offY), Point(x,height), lineCol, 1);
    for(int y=offY; y<=height; y+=cs) line(frame, Point(0,y), Point(width,y), lineCol, 1);
}

void spawnDust(vector<DustParticle>& dust, float x, float y, mt19937& rng) {
    uniform_real_distribution<float> d(-1.5f, 1.5f);
    uniform_real_distribution<float> sz(3.0f, 7.0f);
    for (int i = 0; i < 4; i++)
        dust.push_back({x, y, d(rng), d(rng), 1.0f, sz(rng)});
}

void updateDrawDust(Mat& frame, vector<DustParticle>& dust) {
    for (auto& p : dust) {
        p.x += p.vx; p.y += p.vy; p.alpha -= 0.04f;
        if (p.alpha <= 0) continue;
        uchar v = (uchar)(180 * p.alpha);
        circle(frame, Point((int)p.x,(int)p.y), (int)p.size, Scalar(v,v,v),-1,LINE_AA);
    }
    dust.erase(remove_if(dust.begin(),dust.end(),[](const DustParticle& p){return p.alpha<=0;}), dust.end());
}

void drawScorePops(Mat& frame, vector<ScorePop>& pops) {
    for (auto& sp : pops) {
        sp.y -= 1.5f; sp.alpha -= 0.025f;
        if (sp.alpha <= 0) continue;
        uchar a = (uchar)(255*sp.alpha);
        string s = "+" + to_string(sp.value);
        int bl=0; Size sz=getTextSize(s,FONT_HERSHEY_SIMPLEX,0.7,2,&bl);
        putText(frame, s, Point(sp.x-sz.width/2,(int)sp.y),
                FONT_HERSHEY_SIMPLEX, 0.7, Scalar(a,255,a), 2, LINE_AA);
    }
    pops.erase(remove_if(pops.begin(),pops.end(),[](const ScorePop& p){return p.alpha<=0;}),pops.end());
}

void drawOverlayMessage(Mat& frame, const string& l1, const string& l2, int w, int h, const Scalar& col) {
    Mat ov; frame.copyTo(ov);
    rectangle(ov,Point(w/2-340,h/2-90),Point(w/2+340,h/2+90),Scalar(5,5,5),-1);
    addWeighted(ov,.82,frame,.18,0,frame);
    rectangle(frame,Point(w/2-340,h/2-90),Point(w/2+340,h/2+90),col,2);
    int bl=0;
    Size s1=getTextSize(l1,FONT_HERSHEY_SIMPLEX,1.5,3,&bl);
    putText(frame,l1,Point(w/2-s1.width/2,h/2-16),FONT_HERSHEY_SIMPLEX,1.5,col,3,LINE_AA);
    Size s2=getTextSize(l2,FONT_HERSHEY_SIMPLEX,.78,2,&bl);
    putText(frame,l2,Point(w/2-s2.width/2,h/2+50),FONT_HERSHEY_SIMPLEX,.78,Scalar(190,190,190),2,LINE_AA);
}

void drawMilestoneFlash(Mat& frame, int framesSince, int w, int h, int milestone) {
    if (framesSince > 40) return;
    double alpha = 1.0 - framesSince/40.0;
    string msg = "SCORE " + to_string(milestone) + "!";
    int bl=0; Size s=getTextSize(msg,FONT_HERSHEY_SIMPLEX,1.2,3,&bl);
    uchar a = (uchar)(255*alpha);
    putText(frame, msg, Point(w/2-s.width/2, h/2-70),
            FONT_HERSHEY_SIMPLEX, 1.2, Scalar(a, 220*alpha, a), 3, LINE_AA);
}

void drawEntities(Mat& frame, const deque<Point2D>& prevSnake, const deque<Point2D>& currSnake,
                  Point2D apple, const vector<Obstacle>& obstacles,
                  float interp, int cs, int offY, int frameId, bool isDead) {
    
    // Draw Rocks
    for (const auto& obs : obstacles) {
        int rx = obs.pos.x * cs + cs/2;
        int ry = obs.pos.y * cs + offY + cs/2;
        Scalar rCol(100,100,100);
        vector<Point> pts = {
            Point(rx-cs*0.35, ry-cs*0.15), Point(rx-cs*0.1, ry-cs*0.4),
            Point(rx+cs*0.3, ry-cs*0.3), Point(rx+cs*0.4, ry+cs*0.1),
            Point(rx+cs*0.1, ry+cs*0.4), Point(rx-cs*0.25, ry+cs*0.3)
        };
        fillConvexPoly(frame, pts, rCol);
        polylines(frame, pts, true, Scalar(70,70,70), 2, LINE_AA);
    }

    // Draw Apple (pulsing)
    int ax = apple.x * cs + cs/2;
    int ay = apple.y * cs + offY + cs/2;
    int pulse = (int)(sin(frameId * 0.1) * 3);
    circle(frame, Point(ax,ay), cs/2 - 4 + pulse, Scalar(40,40,220), -1, LINE_AA);
    circle(frame, Point(ax,ay), cs/2 - 8, Scalar(80,80,255), -1, LINE_AA);
    // Apple leaf
    ellipse(frame, Point(ax+4, ay-cs/2+6), Size(6,3), -30, 0, 360, Scalar(40,200,40), -1, LINE_AA);

    // Draw Smooth Snake
    vector<Point> visualBody;
    for (size_t i = 0; i < currSnake.size(); i++) {
        float px = prevSnake[i].x; float py = prevSnake[i].y;
        float cx = currSnake[i].x; float cy = currSnake[i].y;
        float vx = (px * (1.0f - interp) + cx * interp) * cs + cs/2;
        float vy = (py * (1.0f - interp) + cy * interp) * cs + offY + cs/2;
        visualBody.push_back(Point((int)vx, (int)vy));
    }

    Scalar sBody(180, 220, 60);
    Scalar sBorder(100, 150, 40);
    if (isDead) {
        sBody = Scalar(100,100,220);
        sBorder = Scalar(50,50,150);
    }

    // Body segments (thick lines)
    if (visualBody.size() > 1) {
        for (size_t i = 0; i < visualBody.size()-1; i++) {
            line(frame, visualBody[i], visualBody[i+1], sBody, cs*0.7, LINE_AA);
        }
        for (size_t i = 1; i < visualBody.size()-1; i++) {
            circle(frame, visualBody[i], cs*0.35, sBody, -1, LINE_AA);
        }
    }

    // Head
    Point headPt = visualBody.front();
    circle(frame, headPt, cs*0.45, sBody, -1, LINE_AA);
    circle(frame, headPt, cs*0.45, sBorder, 2, LINE_AA);
    
    // Eyes
    float dirX = currSnake[0].x - (currSnake.size()>1 ? currSnake[1].x : currSnake[0].x);
    float dirY = currSnake[0].y - (currSnake.size()>1 ? currSnake[1].y : currSnake[0].y);
    if (dirX == 0 && dirY == 0) { dirX = 1; }
    
    int ex1 = headPt.x + dirX*cs*0.1 - dirY*cs*0.2;
    int ey1 = headPt.y + dirY*cs*0.1 + dirX*cs*0.2;
    int ex2 = headPt.x + dirX*cs*0.1 + dirY*cs*0.2;
    int ey2 = headPt.y + dirY*cs*0.1 - dirX*cs*0.2;
    
    circle(frame, Point(ex1,ey1), 4, Scalar(255,255,255), -1, LINE_AA);
    circle(frame, Point(ex2,ey2), 4, Scalar(255,255,255), -1, LINE_AA);
    if (!isDead) {
        circle(frame, Point(ex1+dirX*2,ey1+dirY*2), 2, Scalar(0,0,0), -1, LINE_AA);
        circle(frame, Point(ex2+dirX*2,ey2+dirY*2), 2, Scalar(0,0,0), -1, LINE_AA);
    } else {
        line(frame, Point(ex1-2,ey1-2), Point(ex1+2,ey1+2), Scalar(0,0,255), 1);
        line(frame, Point(ex1-2,ey1+2), Point(ex1+2,ey1-2), Scalar(0,0,255), 1);
        line(frame, Point(ex2-2,ey2-2), Point(ex2+2,ey2+2), Scalar(0,0,255), 1);
        line(frame, Point(ex2-2,ey2+2), Point(ex2+2,ey2-2), Scalar(0,0,255), 1);
    }
}

// ============================================================================
// 8. AI Controller (BFS Lookahead)
// ============================================================================

bool isSafe(int x, int y, int gw, int gh, const deque<Point2D>& snake, const vector<Obstacle>& rocks) {
    if (x < 0 || x >= gw || y < 0 || y >= gh) return false;
    for (const auto& r : rocks) if (r.pos.x == x && r.pos.y == y) return false;
    // Don't count tail as obstacle if it will move away, but to be safe, avoid whole body
    for (size_t i = 0; i < snake.size() - 1; i++) 
        if (snake[i].x == x && snake[i].y == y) return false;
    return true;
}

Point2D aiDecide(const deque<Point2D>& snake, Point2D apple, const vector<Obstacle>& rocks, int gw, int gh, Point2D currentDir) {
    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};
    
    // Lookahead approach: BFS to find shortest path to apple
    vector<int> dist(gw * gh, -1);
    vector<int> parent(gw * gh, -1);
    queue<Point2D> q;
    
    Point2D head = snake.front();
    q.push(head);
    dist[head.y * gw + head.x] = 0;
    
    bool found = false;
    while (!q.empty()) {
        Point2D curr = q.front(); q.pop();
        if (curr == apple) { found = true; break; }
        
        for (int i = 0; i < 4; i++) {
            int nx = curr.x + dx[i];
            int ny = curr.y + dy[i];
            if (isSafe(nx, ny, gw, gh, snake, rocks)) {
                if (dist[ny * gw + nx] == -1) {
                    dist[ny * gw + nx] = dist[curr.y * gw + curr.x] + 1;
                    parent[ny * gw + nx] = i; // store direction taken to reach here
                    q.push({nx, ny});
                }
            }
        }
    }
    
    if (found) {
        // Backtrack to find first move
        Point2D curr = apple;
        int firstDir = -1;
        while (curr != head) {
            int pDir = parent[curr.y * gw + curr.x];
            firstDir = pDir;
            curr.x -= dx[pDir];
            curr.y -= dy[pDir];
        }
        if (firstDir != -1) return {dx[firstDir], dy[firstDir]};
    }
    
    // Fallback: Pick longest open path or just any safe move (Survival mode)
    Point2D bestMove = currentDir;
    int maxOpen = -1;
    for (int i = 0; i < 4; i++) {
        int nx = head.x + dx[i];
        int ny = head.y + dy[i];
        if (isSafe(nx, ny, gw, gh, snake, rocks)) {
            // Count open neighbors heuristically
            int open = 0;
            for(int j=0; j<4; j++) if (isSafe(nx+dx[j], ny+dy[j], gw, gh, snake, rocks)) open++;
            if (open > maxOpen) {
                maxOpen = open;
                bestMove = {dx[i], dy[i]};
            }
        }
    }
    return bestMove;
}

Point2D getFreeCell(int gw, int gh, const deque<Point2D>& snake, const vector<Obstacle>& rocks, Point2D apple, mt19937& rng) {
    uniform_int_distribution<int> distX(0, gw-1);
    uniform_int_distribution<int> distY(0, gh-1);
    while (true) {
        int x = distX(rng);
        int y = distY(rng);
        Point2D p{x,y};
        if (p == apple) continue;
        bool safe = true;
        for (auto& s : snake) if (s == p) { safe = false; break; }
        for (auto& r : rocks) if (r.pos == p) { safe = false; break; }
        if (safe) return p;
    }
}

// ============================================================================
// 9. Main
// ============================================================================

int main(int argc, char* argv[]) {
    Config cfg = parseArguments(argc, argv);
    
    int gw = cfg.width / cfg.cellSize;
    int gh = (cfg.height - cfg.hudHeight) / cfg.cellSize;
    
    cout << "Snake Simulator\n"
         << "  Output:    " << cfg.outputPath << "\n"
         << "  Grid:      " << gw << "x" << gh << "\n"
         << "  FPS:       " << cfg.fps << "\n"
         << "  Speed:     " << cfg.initialSpeed << " c/s\n\n";

    setNumThreads(cfg.numThreads);

    // Font
    { ifstream f(cfg.fontFile); if(f.good()) initFreeType(cfg.fontFile); }

    // Subtitles
    vector<Subtitle> subs;
    double maxTime = (double)cfg.duration;
    if (!cfg.subtitleFile.empty()) {
        subs = parseSRT(cfg.subtitleFile);
        if (!subs.empty()) {
            for (auto& s : subs) maxTime = max(maxTime, s.end);
        }
    }
    int totalFrames = (int)(maxTime * cfg.fps);
    vector<int> frameSub(totalFrames, -1);
    for (int fi=0;fi<totalFrames;fi++){
        double t=(double)fi/cfg.fps;
        for (int si=0;si<(int)subs.size();si++)
            if(subs[si].start<=t&&t<=subs[si].end){frameSub[fi]=si;break;}
    }

    AsyncVideoWriter video(cfg.outputPath, VideoWriter::fourcc('m','p','4','v'), cfg.fps, Size(cfg.width,cfg.height));
    if (!video.isOpened()) { cerr << "Cannot open video writer!\n"; return 1; }

    random_device rd;
    mt19937 rng(cfg.seed<0?rd():cfg.seed);

    // Game state
    deque<Point2D> currSnake, prevSnake;
    Point2D apple{-1,-1};
    vector<Obstacle> rocks;
    Point2D currentDir{1, 0};
    
    int score = 0, highScore = 0, lives = cfg.lives;
    float currentSpeed = cfg.initialSpeed;
    float moveTimer = 0.0f;
    
    bool isDead = false;
    int deadFrames = 0;
    bool showOverlay = false;
    int overlayFrames = 0;
    string ovLine1, ovLine2; Scalar ovColor;
    int lastMilestone = 0;
    int milestoneFlash = -99;
    
    vector<DustParticle> dust;
    vector<ScorePop> scorePops;

    auto resetGame = [&]() {
        currSnake.clear(); prevSnake.clear();
        int sx = gw/4, sy = gh/2;
        for(int i=0; i<4; i++) {
            currSnake.push_back({sx-i, sy});
            prevSnake.push_back({sx-i, sy});
        }
        currentDir = {1, 0};
        rocks.clear();
        apple = getFreeCell(gw, gh, currSnake, rocks, {-1,-1}, rng);
        moveTimer = 0.0f;
    };
    resetGame();

    Mat frame(cfg.height, cfg.width, CV_8UC3);

    for (int frameId = 0; frameId < totalFrames; frameId++) {
        double currentTime = (double)frameId/cfg.fps;

        // ═══ GAME LOGIC ═══════════════════════════════════════════════════════
        if (!showOverlay) {
            
            // Advance logic clock
            moveTimer += currentSpeed / cfg.fps;
            
            if (moveTimer >= 1.0f) {
                moveTimer -= 1.0f; // Wrap around
                
                prevSnake = currSnake; // Snapshot for interpolation
                
                // AI Decision Lookahead
                currentDir = aiDecide(currSnake, apple, rocks, gw, gh, currentDir);
                
                Point2D nextHead = {currSnake.front().x + currentDir.x, currSnake.front().y + currentDir.y};
                
                // --- PIXEL-BASED HITBOX LOGIC AS REQUESTED (20% reduced) ---
                // We resolve grid logic, but physically test using a shrunk hitbox logic for rocks/walls
                int cs = cfg.cellSize;
                int entityX = nextHead.x * cs;
                int entityY = nextHead.y * cs + cfg.hudHeight;
                int dLeft  = entityX + cs * 0.15;
                int dRight = entityX + cs * 0.85;
                int dTop   = entityY + cs * 0.15;
                int dBot   = entityY + cs * 0.85;
                
                bool hit = false;
                
                // Wall hit check
                if (nextHead.x < 0 || nextHead.x >= gw || nextHead.y < 0 || nextHead.y >= gh) hit = true;
                
                // Rock hit check (using exactly the requested shrunk bounds logic)
                for (auto& r : rocks) {
                    int oLeft = r.pos.x * cs + cs * 0.15;
                    int oRight = r.pos.x * cs + cs * 0.85;
                    int oTop = r.pos.y * cs + cfg.hudHeight + cs * 0.15;
                    int oBot = r.pos.y * cs + cfg.hudHeight + cs * 0.85;
                    if (dRight > oLeft && dLeft < oRight && dBot > oTop && dTop < oBot) { hit = true; break; }
                }
                
                // Self hit check (ignoring the very tail which moves away)
                for (size_t i = 1; i < currSnake.size()-1; i++) {
                    int oLeft = currSnake[i].x * cs + cs * 0.15;
                    int oRight = currSnake[i].x * cs + cs * 0.85;
                    int oTop = currSnake[i].y * cs + cfg.hudHeight + cs * 0.15;
                    int oBot = currSnake[i].y * cs + cfg.hudHeight + cs * 0.85;
                    if (dRight > oLeft && dLeft < oRight && dBot > oTop && dTop < oBot) { hit = true; break; }
                }

                if (hit) {
                    lives--;
                    isDead = true; deadFrames = 20;
                    if (lives <= 0) {
                        ovLine1="GAME OVER";
                        ovLine2="Score: "+to_string(score)+"  Best: "+to_string(highScore);
                        ovColor=Scalar(40,40,220);
                        showOverlay=true; overlayFrames=cfg.fps*2;
                        score=0; lives=cfg.lives; currentSpeed=cfg.initialSpeed;
                        lastMilestone=0; resetGame();
                    } else {
                        ovLine1="OUCH!";
                        ovLine2="Lives: "+to_string(lives);
                        ovColor=Scalar(60,200,220);
                        showOverlay=true; overlayFrames=cfg.fps;
                        resetGame();
                    }
                } else {
                    // Move forward
                    currSnake.push_front(nextHead);
                    
                    // Spawn dust at old tail
                    Point2D tail = currSnake.back();
                    spawnDust(dust, tail.x*cs + cs/2, tail.y*cs + cfg.hudHeight + cs/2, rng);
                    
                    if (nextHead == apple) {
                        score++;
                        if (score > highScore) highScore = score;
                        currentSpeed = min(cfg.maxSpeed, currentSpeed + cfg.speedIncrement);
                        
                        // Score pop
                        scorePops.push_back({nextHead.x*cs + cs/2, (float)(nextHead.y*cs + cfg.hudHeight), 1, 1.0f});
                        
                        // Spawn new rock every 5 points
                        if (score % 5 == 0) {
                            rocks.push_back({getFreeCell(gw, gh, currSnake, rocks, apple, rng), OBSTACLE_ROCK});
                        }
                        apple = getFreeCell(gw, gh, currSnake, rocks, apple, rng);
                        
                        // Milestones
                        int mil = (score / 50) * 50;
                        if (mil > lastMilestone && mil > 0) {
                            lastMilestone = mil;
                            milestoneFlash = frameId;
                        }
                        // Don't pop back, we grew!
                        // Pre-pad prevSnake so interpolation doesn't jump
                        prevSnake.push_back(prevSnake.back()); 
                    } else {
                        currSnake.pop_back();
                    }
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
        drawGridBackground(frame, cfg.width, cfg.height, score, cfg.cellSize, cfg.hudHeight);
        
        // Dust
        updateDrawDust(frame, dust);

        // Grid Entities (Snake, Apple, Rocks)
        drawEntities(frame, prevSnake, currSnake, apple, rocks, moveTimer, cfg.cellSize, cfg.hudHeight, frameId, isDead);

        // Score pops
        drawScorePops(frame, scorePops);

        // Milestone flash
        if (milestoneFlash >= 0)
            drawMilestoneFlash(frame, frameId-milestoneFlash, cfg.width, cfg.height, lastMilestone);

        // HUD (always on top)
        drawHUD(frame, score, highScore, lives, currentSpeed, currentTime);

        // Subtitle
        int si=frameSub[frameId];
        if (si>=0)
            drawSubtitle(frame, subs[si].text, cfg.width, cfg.height,
                         cfg.fontSize, cfg.subtitleY, cfg.subtitleOffset);

        // Overlay
        if (showOverlay)
            drawOverlayMessage(frame, ovLine1, ovLine2, cfg.width, cfg.height, ovColor);

        video.write(frame.clone());

        if (frameId%(cfg.fps*5)==0)
            printf("[%5.1f%%] frame %d/%d | score %d | best %d | lives %d\n",
                   100.0*frameId/totalFrames, frameId, totalFrames, score, highScore, lives);
    }

    cout << "\nDone! => " << cfg.outputPath << "\n";
    cout << "Final best score: " << highScore << "\n";
    return 0;
}