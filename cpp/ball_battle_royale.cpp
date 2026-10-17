#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <random>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <dirent.h>

using namespace cv;
using namespace std;

// ============================================================================
// Ball Battle Royale — "X vs Y vs Z — Who Will Win?" video generator
//
// Physics-based auto-battle simulation: 2-3 colored ball characters bounce
// around a circular arena. Weapons spawn on the ground; whichever ball picks
// one up carries it around from then on, and any other ball that touches the
// carried weapon loses one heart. HP is shown as heart rows. When only one
// character remains, that round's winner is announced, the game freezes for
// a configurable pause (default 30s) with an on-screen countdown, then a
// fresh round begins. Rendering keeps going until the requested video
// duration is reached.
// ============================================================================

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
struct Config {
    string outputPath       = "ball_battle.mp4";
    string backgroundFile   = "";
    string backgroundFolder = "";
    int    bgChangeInterval = 300;
    double bgOpacity        = 1.0;

    int    width            = 1280;
    int    height            = 720;
    int    fps               = 24;
    double duration          = 60.0;   // total video length in seconds
    int    seed              = -1;
    int    numThreads        = max(1, (int)thread::hardware_concurrency());

    // Battle setup
    int    numCharacters     = 3;      // 2-10
    vector<string> names     = {"BLUE", "YELLOW", "RED"}; // used only if user passes --names
    vector<Scalar> colors    = {Scalar(230,120,40), Scalar(0,215,255), Scalar(50,50,230)}; // used only if user passes --colors
    bool   colorsUserProvided = false; // if false, colors are randomized fresh every round
    bool   namesUserProvided  = false; // if false, names are derived from each round's random color
    vector<string> avatarPaths;        // optional per-character avatar images; empty = draw default ball

    int    maxHP             = 10;     // hearts per character
    double pauseDuration      = 30.0;  // seconds frozen after a winner is found
    int    numWeapons         = 2;     // weapons present at the start of each round
    int    maxWeaponsInPlay    = 6;     // hard cap on weapons that can exist at once (ground + carried)
    int    maxWeaponsPerBall   = 4;     // a single ball can carry at most this many weapons
    double weaponSpawnMinInterval = 2.5; // new weapon appears at a random spot every 2.5-5s
    double weaponSpawnMaxInterval = 5.0;
    int    weaponDropCooldownFrames = 20; // grace period before a dropped/spawned weapon can be picked up again
    double arenaRatio         = 0.40;  // arena radius as fraction of min(w,h)

    float  ballRadius         = 40.0f;
    float  minSpeed           = 2.2f;
    float  maxSpeed            = 6.0f;
    float  steerStrength       = 0.18f;

    int    touchDamage         = 1;    // hearts lost when touching an enemy's carried weapon
    int    hitCooldownFrames   = 18;

    // Heart pickups — random extra-life items that appear over time
    int    maxHeartsInPlay      = 2;    // hard cap on heart pickups lying in the arena at once
    double heartSpawnMinInterval = 6.0; // a new heart appears at a random spot every 6-12s
    double heartSpawnMaxInterval = 12.0;
    int    heartHealAmount      = 1;    // hearts restored per pickup
};

// ----------------------------------------------------------------------------
// Entities
// ----------------------------------------------------------------------------
struct Character {
    string name;
    Scalar color;
    Point2f pos, vel;
    float   radius;
    int     hp, maxHp;
    bool    alive;
    bool    armed;       // true while this character is carrying at least one weapon
    int     hitCooldown;
    int     weaponDropCooldown; // prevents losing multiple weapons in one prolonged bump
    int     flashTimer;
    float   wanderAngle;
};

struct Weapon {
    Point2f pos;
    float   radius;
    int     ownerIdx;    // -1 = lying free on the ground; otherwise index into chars, carried
    float   spinAngle;
    int     pickupCooldown; // frames before a freshly spawned/dropped weapon can be grabbed
    bool    toRemove;    // marked true when knocked out of play; only the timed spawner brings a replacement
};

struct HeartPickup {
    Point2f pos;
    float   radius;
    int     pickupCooldown; // brief materialize delay before it can be grabbed
    bool    toRemove;       // consumed this frame — swept out after use
    float   bob;            // animation phase for a gentle floating bob
};

struct Particle {
    Point2f pos, vel;
    Scalar  color;
    float   radius;
    int     life, maxLife;
};

// ----------------------------------------------------------------------------
// Arg parsing helpers
// ----------------------------------------------------------------------------
static vector<string> splitStr(const string& s, char delim) {
    vector<string> out;
    stringstream ss(s);
    string item;
    while (getline(ss, item, delim)) out.push_back(item);
    return out;
}

static Scalar parseColor(const string& s) {
    vector<string> parts = splitStr(s, ',');
    if (parts.size() < 3) return Scalar(200,200,200);
    return Scalar(stoi(parts[0]), stoi(parts[1]), stoi(parts[2]));
}

void printUsage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "  -o, --output <path>        Output video (default: ball_battle.mp4)\n"
         << "  -w, --width <int>          Video width (default: 1280)\n"
         << "  -h, --height <int>         Video height (default: 720)\n"
         << "  -f, --fps <int>            FPS (default: 24)\n"
         << "  -d, --duration <seconds>   Total video duration in seconds (default: 60)\n"
         << "  --seed <int>               Random seed\n"
         << "  --characters <n>           Number of fighters, 2-10 (default: 3)\n"
         << "  --names \"Goku,Vegeta,Piccolo\" Names, comma separated (default: derived from color)\n"
         << "  --colors \"b,g,r;b,g,r;...\" BGR colors per character (default: random each round)\n"
         << "  --avatars \"goku.png,vegeta.png\" Avatar image per character (default: drawn ball)\n"
         << "  --max-hp <int>             Hearts per character (default: 10)\n"
         << "  --pause-duration <sec>     Freeze time after a winner (default: 30)\n"
         << "  --weapons <int>            Weapons present at round start (default: 2)\n"
         << "  --max-weapons-in-play <n>  Max weapons alive at once, ground+carried (default: 6)\n"
         << "  --max-weapons-per-ball <n> Max weapons one ball can carry (default: 4)\n"
         << "  --weapon-spawn-min <sec>   Min seconds between new weapon spawns (default: 2.5)\n"
         << "  --weapon-spawn-max <sec>   Max seconds between new weapon spawns (default: 5.0)\n"
         << "  --max-hearts-in-play <n>   Max heart pickups in the arena at once (default: 2)\n"
         << "  --heart-spawn-min <sec>    Min seconds between new heart spawns (default: 6.0)\n"
         << "  --heart-spawn-max <sec>    Max seconds between new heart spawns (default: 12.0)\n"
         << "  --heal-amount <int>        Hearts restored per heart pickup (default: 1)\n"
         << "  --arena-ratio <float>      Arena radius ratio (default: 0.40)\n"
         << "  -b, --background <path>    Background image\n"
         << "  --bg-folder <path>         Folder with background images\n"
         << "  --bg-interval <int>        Change BG every N frames (default: 300)\n"
         << "  --bg-opacity <0.0-1.0>     Background opacity (default: 1.0)\n"
         << "  --threads <int>            Number of threads\n"
         << "  --help                     Show help\n";
}

Config parseArguments(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if      (arg=="-o"||arg=="--output")        { if(i+1<argc) cfg.outputPath       = argv[++i]; }
        else if (arg=="-w"||arg=="--width")         { if(i+1<argc) cfg.width            = stoi(argv[++i]); }
        else if (arg=="-h"||arg=="--height")        { if(i+1<argc) cfg.height           = stoi(argv[++i]); }
        else if (arg=="-f"||arg=="--fps")           { if(i+1<argc) cfg.fps              = stoi(argv[++i]); }
        else if (arg=="-d"||arg=="--duration")      { if(i+1<argc) cfg.duration         = stod(argv[++i]); }
        else if (arg=="--seed")                     { if(i+1<argc) cfg.seed             = stoi(argv[++i]); }
        else if (arg=="--characters")               { if(i+1<argc) cfg.numCharacters    = stoi(argv[++i]); }
        else if (arg=="--names")                    { if(i+1<argc) { cfg.names = splitStr(argv[++i], ','); cfg.namesUserProvided = true; } }
        else if (arg=="--colors")                   {
            if (i+1<argc) {
                vector<string> parts = splitStr(argv[++i], ';');
                cfg.colors.clear();
                for (auto& p : parts) cfg.colors.push_back(parseColor(p));
                cfg.colorsUserProvided = true;
            }
        }
        else if (arg=="--avatars")                  { if(i+1<argc) cfg.avatarPaths      = splitStr(argv[++i], ','); }
        else if (arg=="--max-hp")                   { if(i+1<argc) cfg.maxHP            = stoi(argv[++i]); }
        else if (arg=="--pause-duration")           { if(i+1<argc) cfg.pauseDuration    = stod(argv[++i]); }
        else if (arg=="--weapons")                  { if(i+1<argc) cfg.numWeapons       = stoi(argv[++i]); }
        else if (arg=="--max-weapons-in-play")      { if(i+1<argc) cfg.maxWeaponsInPlay  = stoi(argv[++i]); }
        else if (arg=="--max-weapons-per-ball")     { if(i+1<argc) cfg.maxWeaponsPerBall = stoi(argv[++i]); }
        else if (arg=="--weapon-spawn-min")         { if(i+1<argc) cfg.weaponSpawnMinInterval = stod(argv[++i]); }
        else if (arg=="--weapon-spawn-max")         { if(i+1<argc) cfg.weaponSpawnMaxInterval = stod(argv[++i]); }
        else if (arg=="--max-hearts-in-play")       { if(i+1<argc) cfg.maxHeartsInPlay   = stoi(argv[++i]); }
        else if (arg=="--heart-spawn-min")          { if(i+1<argc) cfg.heartSpawnMinInterval = stod(argv[++i]); }
        else if (arg=="--heart-spawn-max")          { if(i+1<argc) cfg.heartSpawnMaxInterval = stod(argv[++i]); }
        else if (arg=="--heal-amount")              { if(i+1<argc) cfg.heartHealAmount   = stoi(argv[++i]); }
        else if (arg=="--arena-ratio")               { if(i+1<argc) cfg.arenaRatio        = stod(argv[++i]); }
        else if (arg=="-b"||arg=="--background")    { if(i+1<argc) cfg.backgroundFile   = argv[++i]; }
        else if (arg=="--bg-folder")                { if(i+1<argc) cfg.backgroundFolder = argv[++i]; }
        else if (arg=="--bg-interval")              { if(i+1<argc) cfg.bgChangeInterval = stoi(argv[++i]); }
        else if (arg=="--bg-opacity")                { if(i+1<argc) cfg.bgOpacity         = stod(argv[++i]); }
        else if (arg=="--threads")                   { if(i+1<argc) cfg.numThreads        = stoi(argv[++i]); }
        else if (arg=="--help")                      { printUsage(argv[0]); exit(0); }
        else { cerr << "Unknown argument: " << arg << endl; printUsage(argv[0]); exit(1); }
    }
    cfg.numCharacters = max(2, min(10, cfg.numCharacters));
    if (cfg.namesUserProvided) {
        while ((int)cfg.names.size()  < cfg.numCharacters)  cfg.names.push_back("P" + to_string(cfg.names.size()+1));
        cfg.names.resize(cfg.numCharacters);
    }
    if (cfg.colorsUserProvided) {
        while ((int)cfg.colors.size() < cfg.numCharacters)  cfg.colors.push_back(Scalar(200,200,200));
        cfg.colors.resize(cfg.numCharacters);
    }
    cfg.bgOpacity        = max(0.0, min(1.0, cfg.bgOpacity));
    cfg.bgChangeInterval = max(1, cfg.bgChangeInterval);
    cfg.numThreads       = max(1, cfg.numThreads);
    cfg.duration          = max(1.0, cfg.duration);
    cfg.maxWeaponsPerBall = max(1, min(4, cfg.maxWeaponsPerBall));
    cfg.maxWeaponsInPlay  = max(cfg.numWeapons, cfg.maxWeaponsInPlay);
    if (cfg.weaponSpawnMaxInterval < cfg.weaponSpawnMinInterval)
        cfg.weaponSpawnMaxInterval = cfg.weaponSpawnMinInterval;
    cfg.maxHeartsInPlay = max(0, cfg.maxHeartsInPlay);
    if (cfg.heartSpawnMaxInterval < cfg.heartSpawnMinInterval)
        cfg.heartSpawnMaxInterval = cfg.heartSpawnMinInterval;
    return cfg;
}

// ----------------------------------------------------------------------------
// Background Manager (folder of images, reused pattern)
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// Async Video Writer (background thread so encoding never stalls sim/render)
// ----------------------------------------------------------------------------
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
        cvFull_.wait(lk, [this]{ return queue_.size() < 16 || done_; });
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

// ----------------------------------------------------------------------------
// Drawing helpers
// ----------------------------------------------------------------------------

// Precomputed static background (gradient + faint radial grid) — built ONCE
// and cloned every frame instead of being redrawn pixel-by-pixel, which is
// the single biggest render-speed win for this renderer.
Mat buildStaticBackground(int width, int height, Point2f arenaCenter, float arenaRadius) {
    Mat bg(height, width, CV_8UC3);
    for (int y = 0; y < height; y++) {
        double ratio = (double)y / height;
        uchar r = (uchar)(10  * (1-ratio) + 26 * ratio);
        uchar g = (uchar)(10  * (1-ratio) + 18 * ratio);
        uchar b = (uchar)(30  * (1-ratio) + 46 * ratio);
        bg.row(y).setTo(Scalar(b, g, r));
    }
    // Faint grid lines inside the arena
    Scalar gridColor(70, 55, 40);
    int step = 60;
    for (int gx = (int)(arenaCenter.x - arenaRadius); gx < arenaCenter.x + arenaRadius; gx += step)
        line(bg, cv::Point(gx, 0), cv::Point(gx, height), gridColor, 1, LINE_AA);
    for (int gy = (int)(arenaCenter.y - arenaRadius); gy < arenaCenter.y + arenaRadius; gy += step)
        line(bg, cv::Point(0, gy), cv::Point(width, gy), gridColor, 1, LINE_AA);
    // Vignette corners
    Mat overlay = bg.clone();
    rectangle(overlay, cv::Point(0,0), cv::Point(width,height), Scalar(0,0,0), -1);
    addWeighted(bg, 0.9, overlay, 0.1, 0, bg);
    return bg;
}

void drawArenaBoundary(Mat& frame, Point2f center, float radius) {
    circle(frame, cv::Point((int)center.x,(int)center.y), (int)radius, Scalar(210,210,220), 2, LINE_AA);
    circle(frame, cv::Point((int)center.x,(int)center.y), (int)radius+4, Scalar(60,60,80), 1, LINE_AA);
}

static void drawRoundRect(Mat& frame, cv::Point tl, cv::Point br, int r, const Scalar& color, int thickness=-1) {
    rectangle(frame, cv::Point(tl.x+r, tl.y), cv::Point(br.x-r, br.y), color, thickness);
    rectangle(frame, cv::Point(tl.x, tl.y+r), cv::Point(br.x, br.y-r), color, thickness);
    circle(frame, cv::Point(tl.x+r, tl.y+r), r, color, thickness, LINE_AA);
    circle(frame, cv::Point(br.x-r, tl.y+r), r, color, thickness, LINE_AA);
    circle(frame, cv::Point(tl.x+r, br.y-r), r, color, thickness, LINE_AA);
    circle(frame, cv::Point(br.x-r, br.y-r), r, color, thickness, LINE_AA);
}

// One row of hearts for a single character
void drawHeartRow(Mat& frame, int x0, int y, int hp, int maxHp, const Scalar& color) {
    int gap = 26;
    for (int i = 0; i < maxHp; i++) {
        int cx = x0 + i*gap;
        bool filled = i < hp;
        Scalar col = filled ? color : Scalar(45,45,55);
        int r = 8;
        circle(frame, cv::Point(cx-r/2, y), r/2, col, -1, LINE_AA);
        circle(frame, cv::Point(cx+r/2, y), r/2, col, -1, LINE_AA);
        vector<cv::Point> tri = { {cx-r, y+1}, {cx+r, y+1}, {cx, y+r+r/2} };
        fillConvexPoly(frame, tri, col);
    }
}

void drawHeartsHUD(Mat& frame, const vector<Character>& chars, int width) {
    int rowH = 28;
    int y0 = 24;
    for (size_t i = 0; i < chars.size(); i++) {
        int textW = 0; // hearts start after small margin, left aligned block per row, centered as a whole
        int rowWidth = chars[i].maxHp * 26;
        int x0 = width/2 - rowWidth/2 + 10;
        drawHeartRow(frame, x0, y0 + (int)i*rowH, chars[i].hp, chars[i].maxHp, chars[i].color);
    }
}

// "BLUE vs YELLOW vs RED" title, each name drawn in its own color
void drawTitle(Mat& frame, const vector<Character>& chars, int width, int y) {
    int thick = 2, baseline = 0;
    string sep = " vs ";
    vector<string> parts;
    for (size_t i = 0; i < chars.size(); i++) {
        parts.push_back(chars[i].name);
        if (i+1 < chars.size()) parts.push_back(sep);
    }
    // Start at a comfortable scale and shrink until the title fits the frame,
    // so more characters (and longer names) don't overflow off-screen.
    double scale = 0.95;
    vector<Size> sizes;
    int totalW = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        sizes.clear(); totalW = 0;
        for (auto& p : parts) { Size s = getTextSize(p, FONT_HERSHEY_DUPLEX, scale, thick, &baseline); sizes.push_back(s); totalW += s.width; }
        if (totalW <= width - 40 || scale <= 0.35) break;
        scale -= 0.08;
    }
    int x = width/2 - totalW/2;
    for (size_t i = 0; i < parts.size(); i++) {
        bool isSep = (parts[i] == sep);
        Scalar col = isSep ? Scalar(220,220,220) : chars[i/2].color;
        putText(frame, parts[i], cv::Point(x, y), FONT_HERSHEY_DUPLEX, scale, col, thick, LINE_AA);
        x += sizes[i].width;
    }
}

void drawWeapon(Mat& frame, const Weapon& w) {
    cv::Point c((int)w.pos.x, (int)w.pos.y);
    // glow
    circle(frame, c, (int)w.radius+10, Scalar(255,255,255), 1, LINE_AA);
    // knife blade + handle, rotated
    float a = w.spinAngle;
    Point2f dir(cos(a), sin(a));
    Point2f tip  = w.pos + dir*18.0f;
    Point2f tail = w.pos - dir*14.0f;
    Point2f perp(-dir.y, dir.x);
    vector<cv::Point> blade = {
        cv::Point((int)tip.x, (int)tip.y),
        cv::Point((int)(w.pos.x+perp.x*4), (int)(w.pos.y+perp.y*4)),
        cv::Point((int)(w.pos.x-perp.x*4), (int)(w.pos.y-perp.y*4))
    };
    fillConvexPoly(frame, blade, Scalar(220,220,230));
    line(frame, cv::Point((int)w.pos.x,(int)w.pos.y), cv::Point((int)tail.x,(int)tail.y), Scalar(40,90,150), 4, LINE_AA);
}

// A floating heart pickup that restores HP — gently bobs and pulses so it
// reads clearly as a different kind of item from the weapons.
void drawHeartPickup(Mat& frame, const HeartPickup& h) {
    float bobOffset = sin(h.bob) * 4.0f;
    cv::Point c((int)h.pos.x, (int)(h.pos.y + bobOffset));
    float pulse = 1.0f + 0.08f*sin(h.bob*2.0f);
    int r = (int)(h.radius * pulse);

    circle(frame, c, r+10, Scalar(180,180,255), 1, LINE_AA); // soft glow ring
    Scalar col(70,60,235); // warm pink-red, BGR
    circle(frame, cv::Point(c.x-r/2, c.y), r/2, col, -1, LINE_AA);
    circle(frame, cv::Point(c.x+r/2, c.y), r/2, col, -1, LINE_AA);
    vector<cv::Point> tri = {
        {c.x - r, c.y + 1},
        {c.x + r, c.y + 1},
        {c.x,     c.y + r + r/2}
    };
    fillConvexPoly(frame, tri, col);
    // small highlight for shine
    circle(frame, cv::Point(c.x-r/2, c.y-r/4), max(1,r/5), Scalar(200,180,255), -1, LINE_AA);
}

void drawParticle(Mat& frame, const Particle& p) {
    double t = 1.0 - (double)p.life / p.maxLife;
    float r = max(1.0f, p.radius * (float)(1.0 - t));
    circle(frame, cv::Point((int)p.pos.x,(int)p.pos.y), (int)r, p.color, -1, LINE_AA);
}

void drawCharacter(Mat& frame, const Character& c, int frameId,
                   const Mat* avatar = nullptr, const Mat* avatarGray = nullptr, const Mat* circleMask = nullptr) {
    cv::Point pc((int)c.pos.x, (int)c.pos.y);
    int r = (int)c.radius;

    bool hasAvatar = avatar && !avatar->empty() && circleMask && !circleMask->empty();

    if (hasAvatar) {
        // shadow (same as the default ball, drawn underneath)
        ellipse(frame, cv::Point(pc.x+3,pc.y+3), Size(r, r/2+2), 0, 0, 360, Scalar(0,0,0), -1, LINE_AA);

        Rect roi(pc.x-r, pc.y-r, 2*r, 2*r);
        Rect frameRect(0, 0, frame.cols, frame.rows);
        Rect clipped = roi & frameRect;
        if (clipped.area() > 0) {
            int offX = clipped.x - roi.x, offY = clipped.y - roi.y;
            Rect sub(offX, offY, clipped.width, clipped.height);
            const Mat& src = (!c.alive && avatarGray && !avatarGray->empty()) ? *avatarGray : *avatar;
            Mat srcSub  = src(sub);
            Mat maskSub = (*circleMask)(sub);
            Mat dstRoi  = frame(clipped);
            srcSub.copyTo(dstRoi, maskSub);

            if (c.alive && c.flashTimer > 0) {
                Mat white(dstRoi.size(), dstRoi.type(), Scalar(255,255,255));
                double alpha = 0.55 * (c.flashTimer / 6.0);
                Mat blended;
                addWeighted(dstRoi, 1.0-alpha, white, alpha, 0, blended);
                blended.copyTo(dstRoi, maskSub);
            }
        }

        circle(frame, pc, r, c.alive ? Scalar(20,20,25) : Scalar(90,90,100), 2, LINE_AA);
        if (c.armed && c.alive) circle(frame, pc, r+8, Scalar(0,215,255), 2, LINE_AA);

        if (!c.alive) {
            int ex = r/2, ey = r/3;
            Scalar xc(230,230,230);
            line(frame, cv::Point(pc.x-ex-5,pc.y-ey-5), cv::Point(pc.x-ex+5,pc.y-ey+5), xc, 2, LINE_AA);
            line(frame, cv::Point(pc.x-ex-5,pc.y-ey+5), cv::Point(pc.x-ex+5,pc.y-ey-5), xc, 2, LINE_AA);
            line(frame, cv::Point(pc.x+ex-5,pc.y-ey-5), cv::Point(pc.x+ex+5,pc.y-ey+5), xc, 2, LINE_AA);
            line(frame, cv::Point(pc.x+ex-5,pc.y-ey+5), cv::Point(pc.x+ex+5,pc.y-ey-5), xc, 2, LINE_AA);
        }
        return;
    }

    // ---- default drawn ball (used whenever no avatar image is supplied) ----
    if (!c.alive) {
        // dim, eliminated ghost with X X eyes
        Scalar dim(70,70,80);
        circle(frame, pc, r, dim, -1, LINE_AA);
        circle(frame, pc, r, Scalar(30,30,35), 2, LINE_AA);
        int ex = r/2, ey = r/3;
        Scalar xc(200,200,205);
        line(frame, cv::Point(pc.x-ex-5,pc.y-ey-5), cv::Point(pc.x-ex+5,pc.y-ey+5), xc, 2, LINE_AA);
        line(frame, cv::Point(pc.x-ex-5,pc.y-ey+5), cv::Point(pc.x-ex+5,pc.y-ey-5), xc, 2, LINE_AA);
        line(frame, cv::Point(pc.x+ex-5,pc.y-ey-5), cv::Point(pc.x+ex+5,pc.y-ey+5), xc, 2, LINE_AA);
        line(frame, cv::Point(pc.x+ex-5,pc.y-ey+5), cv::Point(pc.x+ex+5,pc.y-ey-5), xc, 2, LINE_AA);
        return;
    }

    // soft glow ring when armed
    if (c.armed) circle(frame, pc, r+8, Scalar(0,215,255), 2, LINE_AA);

    // shadow
    ellipse(frame, cv::Point(pc.x+3,pc.y+3), Size(r, r/2+2), 0, 0, 360, Scalar(0,0,0), -1, LINE_AA);

    // body
    Scalar bodyCol = c.color;
    if (c.flashTimer > 0) bodyCol = Scalar(240,240,250); // hit flash
    circle(frame, pc, r, bodyCol, -1, LINE_AA);
    circle(frame, pc, r, Scalar(20,20,25), 2, LINE_AA);

    // belly highlight (small, soft — keeps the character's true color dominant)
    ellipse(frame, cv::Point(pc.x-6,pc.y-6), Size(r/2, r/2-4), 0, 0, 360, Scalar(255,255,255), -1, LINE_AA);

    // face
    double hpFrac = (double)c.hp / max(1, c.maxHp);
    int ex = r/3, ey = -r/4;
    circle(frame, cv::Point(pc.x-ex,pc.y+ey), r/4, Scalar(255,255,255), -1, LINE_AA);
    circle(frame, cv::Point(pc.x+ex,pc.y+ey), r/4, Scalar(255,255,255), -1, LINE_AA);
    circle(frame, cv::Point(pc.x-ex,pc.y+ey), r/7, Scalar(20,20,20), -1, LINE_AA);
    circle(frame, cv::Point(pc.x+ex,pc.y+ey), r/7, Scalar(20,20,20), -1, LINE_AA);

    if (hpFrac > 0.4) {
        ellipse(frame, cv::Point(pc.x,pc.y+r/3), Size(r/3,r/5), 0, 0, 180, Scalar(20,20,20), 2, LINE_AA);
    } else {
        ellipse(frame, cv::Point(pc.x,pc.y+r/3+6), Size(r/3,r/5), 0, 180, 360, Scalar(20,20,20), 2, LINE_AA);
    }
}

void drawWinnerOverlay(Mat& frame, const string& winnerName, const Scalar& winnerColor,
                       int secondsLeft, int width, int height) {
    Mat overlay; frame.copyTo(overlay);
    cv::Point tl(width/2-360, height/2-140), br(width/2+360, height/2+140);
    rectangle(overlay, tl, br, Scalar(10,10,15), -1);
    addWeighted(overlay, 0.80, frame, 0.20, 0, frame);
    drawRoundRect(frame, tl, br, 16, winnerColor, 3);

    string line1 = winnerName + " WINS!";
    int baseline = 0;
    Size s1 = getTextSize(line1, FONT_HERSHEY_DUPLEX, 1.5, 3, &baseline);
    putText(frame, line1, cv::Point(width/2-s1.width/2, height/2-40), FONT_HERSHEY_DUPLEX, 1.5, winnerColor, 3, LINE_AA);

    string line2 = "Next round in " + to_string(secondsLeft) + "s";
    Size s2 = getTextSize(line2, FONT_HERSHEY_SIMPLEX, 0.85, 2, &baseline);
    putText(frame, line2, cv::Point(width/2-s2.width/2, height/2+30), FONT_HERSHEY_SIMPLEX, 0.85, Scalar(220,220,220), 2, LINE_AA);

    // big countdown number
    string num = to_string(secondsLeft);
    Size s3 = getTextSize(num, FONT_HERSHEY_DUPLEX, 1.8, 3, &baseline);
    putText(frame, num, cv::Point(width/2-s3.width/2, height/2+100), FONT_HERSHEY_DUPLEX, 1.8, Scalar(255,255,255), 3, LINE_AA);
}

void drawTimeHUD(Mat& frame, double currentTime, int round, int width, int height) {
    int secs = (int)currentTime;
    int mins = secs/60; secs %= 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    putText(frame, buf, cv::Point(width-90, height-20), FONT_HERSHEY_SIMPLEX, 0.6, Scalar(200,200,200), 1, LINE_AA);
    putText(frame, "ROUND " + to_string(round), cv::Point(20, height-20), FONT_HERSHEY_SIMPLEX, 0.6, Scalar(200,200,200), 1, LINE_AA);
}

// ----------------------------------------------------------------------------
// Physics helpers
// ----------------------------------------------------------------------------
static float dist2f(const Point2f& a, const Point2f& b) {
    return sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y));
}

static void clampSpeed(Point2f& v, float minS, float maxS) {
    float sp = sqrt(v.x*v.x + v.y*v.y);
    if (sp < 1e-4f) { v = Point2f(minS, 0); return; }
    if (sp > maxS) v *= (maxS/sp);
    else if (sp < minS) v *= (minS/sp);
}

static void spawnBurst(vector<Particle>& particles, Point2f pos, Scalar color, mt19937& rng, int count=10) {
    uniform_real_distribution<float> angD(0.0f, (float)(2*CV_PI));
    uniform_real_distribution<float> spdD(1.5f, 5.0f);
    uniform_int_distribution<int> lifeD(14, 26);
    for (int i = 0; i < count; i++) {
        float a = angD(rng), sp = spdD(rng);
        Particle p;
        p.pos = pos;
        p.vel = Point2f(cos(a)*sp, sin(a)*sp);
        p.color = color;
        p.radius = 5.0f;
        p.maxLife = lifeD(rng);
        p.life = p.maxLife;
        particles.push_back(p);
    }
}

// Random point inside the arena, guaranteed to stay at least `objRadius` away
// from the boundary so an object placed there never pokes outside the ring.
static Point2f randomPointInArena(mt19937& rng, Point2f center, float arenaRadius, float objRadius = 0.0f) {
    float maxR = max(0.0f, arenaRadius - objRadius);
    uniform_real_distribution<float> angD(0.0f, (float)(2*CV_PI));
    uniform_real_distribution<float> unitD(0.0f, 1.0f);
    float a = angD(rng);
    float r = maxR * sqrt(unitD(rng)); // sqrt(uniform) -> uniform over the disk area, not just radius
    return Point2f(center.x + cos(a)*r, center.y + sin(a)*r);
}

// Pull a point back inside the arena boundary (minus objRadius) if it strayed outside.
static Point2f clampToArena(Point2f p, Point2f center, float arenaRadius, float objRadius) {
    float maxDist = max(0.0f, arenaRadius - objRadius);
    float d = dist2f(p, center);
    if (d > maxDist && d > 1e-4f) {
        Point2f n = (p - center) * (1.0f/d);
        p = center + n * maxDist;
    }
    return p;
}

// ----------------------------------------------------------------------------
// Random per-round color palette + avatar loading
// ----------------------------------------------------------------------------

// h in [0,360), s and v in [0,1] -> BGR Scalar
static Scalar hsvToBgr(float h, float s, float v) {
    float c = v * s;
    float x = c * (1 - fabs(fmod(h / 60.0f, 2.0f) - 1));
    float m = v - c;
    float r,g,b;
    if      (h <  60) { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else               { r=c; g=0; b=x; }
    return Scalar((b+m)*255.0f, (g+m)*255.0f, (r+m)*255.0f); // BGR
}

// Rough hue-bucket -> human color name, used for default names when the
// person hasn't supplied --names, so the title still reads naturally
// ("CRIMSON vs LIME vs VIOLET") even though the palette is randomized.
static string colorNameFromHue(float h) {
    if (h < 15 || h >= 345) return "RED";
    if (h < 45)  return "ORANGE";
    if (h < 70)  return "YELLOW";
    if (h < 100) return "LIME";
    if (h < 150) return "GREEN";
    if (h < 190) return "CYAN";
    if (h < 230) return "BLUE";
    if (h < 265) return "INDIGO";
    if (h < 300) return "PURPLE";
    return "PINK";
}

// Generates `n` visually distinct, vividly saturated colors (evenly spread
// around the hue wheel from a random start) plus matching default names.
static void generateRandomPalette(mt19937& rng, int n, vector<Scalar>& outColors, vector<string>& outNames) {
    outColors.clear(); outNames.clear();
    uniform_real_distribution<float> startHueD(0.0f, 360.0f);
    uniform_real_distribution<float> jitterD(-12.0f, 12.0f);
    uniform_real_distribution<float> satD(0.65f, 0.9f);
    uniform_real_distribution<float> valD(0.85f, 1.0f);
    float startHue = startHueD(rng);
    float step = 360.0f / max(1, n);
    vector<string> usedNames;
    for (int i = 0; i < n; i++) {
        float h = fmod(startHue + i*step + jitterD(rng) + 360.0f, 360.0f);
        outColors.push_back(hsvToBgr(h, satD(rng), valD(rng)));
        string base = colorNameFromHue(h);
        string name = base;
        int suffix = 2;
        while (find(usedNames.begin(), usedNames.end(), name) != usedNames.end())
            name = base + " " + to_string(suffix++);
        usedNames.push_back(name);
        outNames.push_back(name);
    }
}

// Loads an avatar image, center-crops it to a square, and resizes it to fill
// a ball of the given diameter. Returns an empty Mat if the file can't be
// read, which callers treat as "fall back to the default drawn ball".
static Mat loadAvatarSquare(const string& path, int diameter) {
    if (path.empty()) return Mat();
    Mat img = imread(path, IMREAD_COLOR);
    if (img.empty()) return Mat();
    int side = min(img.cols, img.rows);
    Rect cropRect((img.cols - side) / 2, (img.rows - side) / 2, side, side);
    Mat cropped = img(cropRect);
    Mat resized;
    resize(cropped, resized, Size(diameter, diameter));
    return resized;
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg = parseArguments(argc, argv);

    cout << "Ball Battle Royale Simulator\n"
         << "  Output:         " << cfg.outputPath << "\n"
         << "  Resolution:     " << cfg.width << "x" << cfg.height << "\n"
         << "  FPS:            " << cfg.fps << "\n"
         << "  Duration:       " << cfg.duration << "s\n"
         << "  Characters:     " << cfg.numCharacters << "\n";
    if (cfg.namesUserProvided) for (int i = 0; i < cfg.numCharacters; i++) cout << "    - " << cfg.names[i] << "\n";
    else cout << "    - (names derived from a random color palette, regenerated each round)\n";
    if (!cfg.avatarPaths.empty()) cout << "  Avatars:        " << cfg.avatarPaths.size() << " image(s) supplied\n";
    cout << "  Max HP:         " << cfg.maxHP << "\n"
         << "  Pause duration: " << cfg.pauseDuration << "s\n"
         << "  Weapons start:  " << cfg.numWeapons << "\n"
         << "  Weapons cap:    " << cfg.maxWeaponsInPlay << " in play, " << cfg.maxWeaponsPerBall << " per ball\n"
         << "  Weapon spawn:   every " << cfg.weaponSpawnMinInterval << "-" << cfg.weaponSpawnMaxInterval << "s\n"
         << "  Hearts cap:     " << cfg.maxHeartsInPlay << " in play, heal +" << cfg.heartHealAmount << " each\n"
         << "  Heart spawn:    every " << cfg.heartSpawnMinInterval << "-" << cfg.heartSpawnMaxInterval << "s\n"
         << "  Threads:        " << cfg.numThreads << "\n\n";

    setNumThreads(cfg.numThreads);

    random_device rd;
    mt19937 rng(cfg.seed < 0 ? rd() : cfg.seed);

    // Arena geometry — leave room at the top for the hearts HUD + title,
    // and a little breathing room at the bottom.
    int heartsBottom = 24 + cfg.numCharacters*28;
    int titleY        = heartsBottom + 30;
    int arenaTop       = titleY + 40;
    float arenaRadius  = (float)min(cfg.width, cfg.height) * (float)cfg.arenaRatio;
    float availableH   = (float)cfg.height - arenaTop - 30.0f;
    if (arenaRadius*2.0f > availableH) arenaRadius = max(60.0f, availableH/2.0f);
    Point2f arenaCenter((float)cfg.width/2.0f, (float)arenaTop + arenaRadius);

    // Optional custom background
    Mat staticImgBg;
    BackgroundManager* bgMgr = nullptr;
    bool useFolderBg = false;
    if (!cfg.backgroundFolder.empty()) {
        bgMgr = new BackgroundManager(cfg.backgroundFolder, cfg.width, cfg.height, cfg.bgOpacity, cfg.seed<0?42:cfg.seed);
        if (bgMgr->has()) useFolderBg = true; else { delete bgMgr; bgMgr = nullptr; }
    }
    if (!useFolderBg && !cfg.backgroundFile.empty()) {
        staticImgBg = imread(cfg.backgroundFile);
        if (!staticImgBg.empty()) {
            resize(staticImgBg, staticImgBg, Size(cfg.width, cfg.height));
            if (cfg.bgOpacity < 1.0) staticImgBg.convertTo(staticImgBg, -1, cfg.bgOpacity, 0);
        }
    }
    Mat proceduralBg = buildStaticBackground(cfg.width, cfg.height, arenaCenter, arenaRadius);
    Mat currentFolderBg;
    if (useFolderBg && bgMgr) currentFolderBg = bgMgr->next();

    // Optional avatar images — one per character slot, in the order given.
    // Missing/unreadable entries fall back to the default drawn ball, and
    // if --avatars was never passed at all every character uses the default.
    int avatarDiameter = (int)(cfg.ballRadius * 2.0f);
    vector<Mat> avatarImgs(cfg.numCharacters), avatarGrayImgs(cfg.numCharacters);
    for (int i = 0; i < cfg.numCharacters && i < (int)cfg.avatarPaths.size(); i++) {
        avatarImgs[i] = loadAvatarSquare(cfg.avatarPaths[i], avatarDiameter);
        if (!avatarImgs[i].empty()) {
            Mat grayTmp;
            cvtColor(avatarImgs[i], grayTmp, COLOR_BGR2GRAY);
            cvtColor(grayTmp, avatarGrayImgs[i], COLOR_GRAY2BGR);
            avatarGrayImgs[i].convertTo(avatarGrayImgs[i], -1, 0.5, 0); // darken for "eliminated" look
        } else {
            cerr << "Warning: could not read avatar image \"" << cfg.avatarPaths[i]
                 << "\" — character " << (i+1) << " will use the default ball.\n";
        }
    }
    Mat circleMask(avatarDiameter, avatarDiameter, CV_8UC1, Scalar(0));
    circle(circleMask, cv::Point(avatarDiameter/2, avatarDiameter/2), (int)cfg.ballRadius, Scalar(255), -1, LINE_8);

    int totalFrames = (int)(cfg.duration * cfg.fps);

    AsyncVideoWriter video(cfg.outputPath, VideoWriter::fourcc('m','p','4','v'), cfg.fps, Size(cfg.width, cfg.height));
    if (!video.isOpened()) { cerr << "Cannot open video writer!\n"; delete bgMgr; return 1; }

    // ---- Round state setup ----
    vector<Character> chars;
    vector<Weapon> weapons;
    vector<HeartPickup> hearts;
    vector<Particle> particles;

    auto spawnCharacters = [&](){
        chars.clear();
        uniform_real_distribution<float> angD(0.0f, (float)(2*CV_PI));

        // Randomize the palette fresh every round unless the person pinned
        // their own colors/names via --colors / --names.
        vector<Scalar> roundColors;
        vector<string> roundNames;
        if (!cfg.colorsUserProvided || !cfg.namesUserProvided) {
            vector<Scalar> genColors; vector<string> genNames;
            generateRandomPalette(rng, cfg.numCharacters, genColors, genNames);
            roundColors = cfg.colorsUserProvided ? cfg.colors : genColors;
            roundNames  = cfg.namesUserProvided  ? cfg.names  : genNames;
        } else {
            roundColors = cfg.colors;
            roundNames  = cfg.names;
        }

        for (int i = 0; i < cfg.numCharacters; i++) {
            Character c;
            c.name = roundNames[i];
            c.color = roundColors[i];
            float a = (float)i * (2*CV_PI/cfg.numCharacters);
            float rr = arenaRadius * 0.5f;
            c.pos = Point2f(arenaCenter.x + cos(a)*rr, arenaCenter.y + sin(a)*rr);
            float va = angD(rng);
            c.vel = Point2f(cos(va)*cfg.minSpeed, sin(va)*cfg.minSpeed);
            c.radius = cfg.ballRadius;
            c.maxHp = cfg.maxHP;
            c.hp = cfg.maxHP;
            c.alive = true;
            c.armed = false;
            c.hitCooldown = 0;
            c.weaponDropCooldown = 0;
            c.flashTimer = 0;
            c.wanderAngle = va;
            chars.push_back(c);
        }
    };

    auto spawnWeapons = [&](){
        weapons.clear();
        for (int i = 0; i < cfg.numWeapons; i++) {
            Weapon w;
            w.radius = 14.0f;
            w.pos = randomPointInArena(rng, arenaCenter, arenaRadius, w.radius);
            w.ownerIdx = -1;
            w.spinAngle = 0.0f;
            w.pickupCooldown = 0;
            w.toRemove = false;
            weapons.push_back(w);
        }
    };

    uniform_real_distribution<double> weaponSpawnGapD(cfg.weaponSpawnMinInterval, cfg.weaponSpawnMaxInterval);
    int weaponSpawnTimer = (int)(weaponSpawnGapD(rng) * cfg.fps);
    auto trySpawnExtraWeapon = [&](){
        if ((int)weapons.size() >= cfg.maxWeaponsInPlay) return;
        Weapon w;
        w.radius = 14.0f;
        w.pos = randomPointInArena(rng, arenaCenter, arenaRadius, w.radius);
        w.ownerIdx = -1;
        w.spinAngle = 0.0f;
        w.pickupCooldown = cfg.weaponDropCooldownFrames / 2; // brief materialize delay
        w.toRemove = false;
        weapons.push_back(w);
    };

    uniform_real_distribution<double> heartSpawnGapD(cfg.heartSpawnMinInterval, cfg.heartSpawnMaxInterval);
    int heartSpawnTimer = (int)(heartSpawnGapD(rng) * cfg.fps);
    auto trySpawnHeart = [&](){
        if ((int)hearts.size() >= cfg.maxHeartsInPlay) return;
        HeartPickup h;
        h.radius = 13.0f;
        h.pos = randomPointInArena(rng, arenaCenter, arenaRadius, h.radius);
        h.pickupCooldown = 15; // brief materialize delay
        h.toRemove = false;
        h.bob = 0.0f;
        hearts.push_back(h);
    };

    spawnCharacters();
    spawnWeapons();

    enum Phase { PLAYING, PAUSED };
    Phase phase = PLAYING;
    int pauseFramesLeft = 0;
    int winnerIdx = -1;
    int roundNum = 1;

    Mat frame(cfg.height, cfg.width, CV_8UC3);

    cout << "Rendering " << totalFrames << " frames (" << (int)cfg.duration << "s)...\n";

    for (int frameId = 0; frameId < totalFrames; frameId++) {
        double currentTime = (double)frameId / cfg.fps;

        if (useFolderBg && bgMgr && frameId % cfg.bgChangeInterval == 0)
            currentFolderBg = bgMgr->next();

        // ====================== Simulation ======================
        if (phase == PLAYING) {
            int aliveCount = 0, lastAliveIdx = -1;
            for (size_t i = 0; i < chars.size(); i++) if (chars[i].alive) { aliveCount++; lastAliveIdx = (int)i; }

            if (aliveCount <= 1) {
                winnerIdx = lastAliveIdx;
                phase = PAUSED;
                pauseFramesLeft = (int)(cfg.pauseDuration * cfg.fps);
            } else {
                // --- AI steering + physics ---
                for (auto& c : chars) {
                    if (!c.alive) continue;
                    if (c.hitCooldown > 0) c.hitCooldown--;
                    if (c.weaponDropCooldown > 0) c.weaponDropCooldown--;

                    // find nearest alive opponent
                    float bestD = 1e9f; Point2f target = c.pos;
                    for (auto& o : chars) {
                        if (&o == &c || !o.alive) continue;
                        float d = dist2f(c.pos, o.pos);
                        if (d < bestD) { bestD = d; target = o.pos; }
                    }
                    Point2f toTarget = target - c.pos;
                    float tlen = sqrt(toTarget.x*toTarget.x + toTarget.y*toTarget.y);
                    if (tlen > 1e-3f) toTarget *= (1.0f/tlen);

                    uniform_real_distribution<float> jitterD(-0.35f, 0.35f);
                    c.wanderAngle += jitterD(rng) * 0.3f;
                    Point2f wander(cos(c.wanderAngle), sin(c.wanderAngle));

                    Point2f desired = toTarget*cfg.steerStrength + wander*(1.0f-cfg.steerStrength);
                    c.vel += desired * 0.4f;
                    clampSpeed(c.vel, cfg.minSpeed, cfg.maxSpeed);

                    c.pos += c.vel;

                    // arena boundary bounce
                    float dc = dist2f(c.pos, arenaCenter);
                    if (dc + c.radius > arenaRadius) {
                        Point2f n = (c.pos - arenaCenter) * (1.0f/max(dc,1e-3f));
                        c.pos = arenaCenter + n * (arenaRadius - c.radius);
                        float vn = c.vel.x*n.x + c.vel.y*n.y;
                        c.vel -= n * (2*vn);
                        c.wanderAngle = atan2(c.vel.y, c.vel.x);
                    }
                }

                // --- continuous random weapon spawning over time ---
                weaponSpawnTimer--;
                if (weaponSpawnTimer <= 0) {
                    trySpawnExtraWeapon();
                    weaponSpawnTimer = (int)(weaponSpawnGapD(rng) * cfg.fps);
                }

                // --- continuous random heart (extra life) spawning over time ---
                heartSpawnTimer--;
                if (heartSpawnTimer <= 0) {
                    trySpawnHeart();
                    heartSpawnTimer = (int)(heartSpawnGapD(rng) * cfg.fps);
                }

                // hearts gently bob in place and get picked up by any alive character
                // that touches them, even at full HP — HP is simply capped at maxHp
                for (auto& h : hearts) {
                    h.bob += 0.08f;
                    if (h.pickupCooldown > 0) { h.pickupCooldown--; continue; }
                    for (auto& c : chars) {
                        if (!c.alive) continue;
                        if (dist2f(c.pos, h.pos) < c.radius + h.radius) {
                            c.hp = min(c.maxHp, c.hp + cfg.heartHealAmount);
                            h.toRemove = true;
                            spawnBurst(particles, h.pos, Scalar(90,70,235), rng, 14);
                            break;
                        }
                    }
                }
                hearts.erase(remove_if(hearts.begin(), hearts.end(),
                    [](const HeartPickup& h){ return h.toRemove; }), hearts.end());

                // tick down pickup cooldowns, reset per-frame armed flag
                for (auto& w : weapons) if (w.pickupCooldown > 0) w.pickupCooldown--;
                for (auto& c : chars) c.armed = false;

                // count weapons already owned by each character (capacity check)
                vector<int> ownedCount(chars.size(), 0);
                for (auto& w : weapons) if (w.ownerIdx >= 0) ownedCount[w.ownerIdx]++;

                // --- pickup: free weapons on the ground get grabbed on contact ---
                for (auto& w : weapons) {
                    if (w.ownerIdx != -1 || w.pickupCooldown > 0) continue;
                    for (size_t ci = 0; ci < chars.size(); ci++) {
                        Character& c = chars[ci];
                        if (!c.alive || ownedCount[ci] >= cfg.maxWeaponsPerBall) continue;
                        if (dist2f(c.pos, w.pos) < c.radius + w.radius) {
                            w.ownerIdx = (int)ci;
                            ownedCount[ci]++;
                            break;
                        }
                    }
                }

                // drop weapons whose owner died
                for (auto& w : weapons) {
                    if (w.ownerIdx != -1 && !chars[w.ownerIdx].alive) {
                        w.ownerIdx = -1;
                        w.pickupCooldown = cfg.weaponDropCooldownFrames;
                    }
                }

                // --- carry: fan carried weapons out around their owner, facing forward ---
                {
                    vector<vector<int>> ownerGroups(chars.size());
                    for (size_t wi = 0; wi < weapons.size(); wi++)
                        if (weapons[wi].ownerIdx >= 0) ownerGroups[weapons[wi].ownerIdx].push_back((int)wi);

                    for (size_t ci = 0; ci < chars.size(); ci++) {
                        auto& group = ownerGroups[ci];
                        if (group.empty()) continue;
                        Character& owner = chars[ci];
                        owner.armed = true;
                        float speed = sqrt(owner.vel.x*owner.vel.x + owner.vel.y*owner.vel.y);
                        float facing = speed > 0.1f ? atan2(owner.vel.y, owner.vel.x) : weapons[group[0]].spinAngle;
                        int n = (int)group.size();
                        float stepAngle = (float)(2*CV_PI) / max(1, n); // spread evenly around the whole ball
                        for (int k = 0; k < n; k++) {
                            Weapon& w = weapons[group[k]];
                            float a = facing + k*stepAngle;
                            w.spinAngle = a;
                            float attachDist = owner.radius + w.radius + 2.0f;
                            w.pos = owner.pos + Point2f(cos(a), sin(a)) * attachDist;
                            w.pos = clampToArena(w.pos, arenaCenter, arenaRadius, w.radius);

                            // any other alive character touching a carried weapon loses a heart
                            for (size_t oi = 0; oi < chars.size(); oi++) {
                                if (oi == ci) continue;
                                Character& c = chars[oi];
                                if (!c.alive || c.hitCooldown > 0) continue;
                                if (dist2f(c.pos, w.pos) < c.radius + w.radius) {
                                    c.hp -= cfg.touchDamage;
                                    c.hitCooldown = cfg.hitCooldownFrames;
                                    c.flashTimer = 6;
                                    spawnBurst(particles, c.pos, c.color, rng, 10);
                                    if (c.hp <= 0) { c.hp = 0; c.alive = false; spawnBurst(particles, c.pos, c.color, rng, 24); }
                                }
                            }
                        }
                    }
                }

                // --- character vs character collisions (physics bounce + weapon loss) ---
                for (size_t i = 0; i < chars.size(); i++) {
                    if (!chars[i].alive) continue;
                    for (size_t j = i+1; j < chars.size(); j++) {
                        if (!chars[j].alive) continue;
                        Character& a = chars[i];
                        Character& b = chars[j];
                        float d = dist2f(a.pos, b.pos);
                        float minD = a.radius + b.radius;
                        if (d < minD && d > 1e-4f) {
                            Point2f n = (b.pos - a.pos) * (1.0f/d);
                            float overlap = (minD - d) * 0.5f;
                            a.pos -= n * overlap;
                            b.pos += n * overlap;

                            float rel = (b.vel.x-a.vel.x)*n.x + (b.vel.y-a.vel.y)*n.y;
                            if (rel < 0) {
                                a.vel += n * rel;
                                b.vel -= n * rel;
                            }

                            // a carried ball that bumps into another ball drops one weapon —
                            // it leaves play entirely; only the timed spawner brings a new one
                            for (int idx : {(int)i, (int)j}) {
                                Character& c = chars[idx];
                                if (c.weaponDropCooldown > 0) continue;
                                for (auto& w : weapons) {
                                    if (w.ownerIdx == idx) {
                                        w.toRemove = true;
                                        c.weaponDropCooldown = cfg.weaponDropCooldownFrames;
                                        spawnBurst(particles, c.pos, Scalar(210,210,215), rng, 6);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else { // PAUSED
            pauseFramesLeft--;
            if (pauseFramesLeft <= 0) {
                roundNum++;
                spawnCharacters();
                spawnWeapons();
                weaponSpawnTimer = (int)(weaponSpawnGapD(rng) * cfg.fps);
                hearts.clear();
                heartSpawnTimer = (int)(heartSpawnGapD(rng) * cfg.fps);
                particles.clear();
                winnerIdx = -1;
                phase = PLAYING;
            }
        }

        // transient per-character visual timers always tick down, even while
        // paused, so a character frozen mid-flash doesn't stay white forever
        for (auto& c : chars) if (c.flashTimer > 0) c.flashTimer--;

        // particles always update (even during pause) so hit-effects finish fading
        for (auto& p : particles) {
            p.pos += p.vel;
            p.vel *= 0.92f;
            p.life--;
        }
        particles.erase(remove_if(particles.begin(), particles.end(),
            [](const Particle& p){ return p.life <= 0; }), particles.end());

        // weapons knocked out of play via a body collision are fully removed here;
        // only the timed spawner (weapon-spawn-min/max) brings a replacement later
        weapons.erase(remove_if(weapons.begin(), weapons.end(),
            [](const Weapon& w){ return w.toRemove; }), weapons.end());

        // ====================== Render ======================
        if (useFolderBg && !currentFolderBg.empty()) currentFolderBg.copyTo(frame);
        else if (!staticImgBg.empty())                staticImgBg.copyTo(frame);
        else                                           proceduralBg.copyTo(frame);

        drawArenaBoundary(frame, arenaCenter, arenaRadius);
        for (auto& w : weapons) drawWeapon(frame, w);
        for (auto& h : hearts) drawHeartPickup(frame, h);
        for (auto& p : particles) drawParticle(frame, p);
        for (size_t ci = 0; ci < chars.size(); ci++) {
            const Mat* av  = (ci < avatarImgs.size()     && !avatarImgs[ci].empty())     ? &avatarImgs[ci]     : nullptr;
            const Mat* avg = (ci < avatarGrayImgs.size() && !avatarGrayImgs[ci].empty()) ? &avatarGrayImgs[ci] : nullptr;
            drawCharacter(frame, chars[ci], frameId, av, avg, av ? &circleMask : nullptr);
        }

        drawHeartsHUD(frame, chars, cfg.width);
        drawTitle(frame, chars, cfg.width, titleY);
        drawTimeHUD(frame, currentTime, roundNum, cfg.width, cfg.height);

        if (phase == PAUSED && winnerIdx >= 0) {
            int secLeft = (pauseFramesLeft + cfg.fps - 1) / cfg.fps;
            drawWinnerOverlay(frame, chars[winnerIdx].name, chars[winnerIdx].color, secLeft, cfg.width, cfg.height);
        }

        video.write(frame.clone());

        if (frameId % (cfg.fps * 10) == 0) {
            int aliveCount = 0;
            for (auto& c : chars) if (c.alive) aliveCount++;
            printf("[%5.1f%%] frame %d/%d | round %d | alive %d/%d | phase %s\n",
                   100.0*frameId/totalFrames, frameId, totalFrames, roundNum, aliveCount, (int)chars.size(),
                   phase==PLAYING ? "PLAYING" : "PAUSED");
        }
    }

    delete bgMgr;
    cout << "\nDone! => " << cfg.outputPath << "\n";
    return 0;
}
