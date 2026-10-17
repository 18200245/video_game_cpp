/**
 * make_clip.cpp
 * Tạo video tĩnh (intro / outro) tương thích hoàn toàn với flappy_bird_sim.
 * Codec: mp4v  |  Color: BGR  |  Cùng width/height/fps → ffmpeg concat không lỗi.
 *
 * Compile:
 *   g++ -O2 -std=c++17 make_clip.cpp -o make_clip \
 *       $(pkg-config --cflags --libs opencv4) -lpthread
 *
 * Ví dụ:
 *   ./make_clip -o intro.mp4 --bg intro.jpg --time 5 --fade-in 1.0
 *   ./make_clip -o outro.mp4 --bg outro.jpg --time 8 --fade-out 2.0
 *
 * Ghép với ffmpeg:
 *   printf "file 'intro.mp4'\nfile 'flappy.mp4'\nfile 'outro.mp4'\n" > list.txt
 *   ffmpeg -f concat -safe 0 -i list.txt -c copy final.mp4
 */

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cmath>

using namespace cv;
using namespace std;

// ============================================================================
// Config
// ============================================================================

struct ClipConfig {
    string output     = "clip.mp4";
    string bgImage    = "";
    int    width      = 1280;
    int    height     = 720;
    int    fps        = 24;
    double duration   = 10.0;   // giây
    double fadeIn     = 0.5;    // giây fade-in từ đen, 0 = tắt
    double fadeOut    = 0.5;    // giây fade-out sang đen, 0 = tắt
    double bgOpacity  = 1.0;



    int    numThreads = max(1, (int)thread::hardware_concurrency());
};

// ============================================================================
// Parse arguments
// ============================================================================

void printUsage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "  -o, --output <path>       Output file (default: clip.mp4)\n"
         << "  --bg <path>               Background image\n"
         << "  --time <seconds>          Duration in seconds (default: 10)\n"
         << "  --fade-in <seconds>       Fade in from black (default: 0.5)\n"
         << "  --fade-out <seconds>      Fade out to black (default: 0.5)\n"
         << "  --bg-opacity <0.0-1.0>    Background opacity (default: 1.0)\n"
         << "  -w, --width <int>         Width  (default: 1280)\n"
         << "  -h, --height <int>        Height (default: 720)\n"
         << "  -f, --fps <int>           FPS    (default: 24)\n"
         << "  --threads <int>           Threads (default: auto)\n"
         << "  --help\n";
}

ClipConfig parseArgs(int argc, char* argv[]) {
    ClipConfig cfg;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if      (a=="-o"||a=="--output")       { if(i+1<argc) cfg.output      = argv[++i]; }
        else if (a=="--bg")                    { if(i+1<argc) cfg.bgImage     = argv[++i]; }
        else if (a=="--time")                  { if(i+1<argc) cfg.duration    = stod(argv[++i]); }
        else if (a=="--fade-in")               { if(i+1<argc) cfg.fadeIn      = stod(argv[++i]); }
        else if (a=="--fade-out")              { if(i+1<argc) cfg.fadeOut     = stod(argv[++i]); }
        else if (a=="--bg-opacity")            { if(i+1<argc) cfg.bgOpacity   = stod(argv[++i]); }
        else if (a=="-w"||a=="--width")        { if(i+1<argc) cfg.width       = stoi(argv[++i]); }
        else if (a=="-h"||a=="--height")       { if(i+1<argc) cfg.height      = stoi(argv[++i]); }
        else if (a=="-f"||a=="--fps")          { if(i+1<argc) cfg.fps         = stoi(argv[++i]); }
        else if (a=="--threads")               { if(i+1<argc) cfg.numThreads  = stoi(argv[++i]); }
        else if (a=="--help")                  { printUsage(argv[0]); exit(0); }
        else { cerr << "Unknown argument: " << a << "\n"; printUsage(argv[0]); exit(1); }
    }
    cfg.bgOpacity = max(0.0, min(1.0, cfg.bgOpacity));
    cfg.duration  = max(0.1, cfg.duration);
    cfg.fadeIn    = max(0.0, min(cfg.duration * 0.5, cfg.fadeIn));
    cfg.fadeOut   = max(0.0, min(cfg.duration * 0.5, cfg.fadeOut));
    return cfg;
}

// ============================================================================
// Async Video Writer  (giống hệt flappy_bird_sim để đảm bảo pipeline tương thích)
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
        queue_.push(move(frame));
        cv_.notify_one();
    }
private:
    void loop() {
        while (true) {
            Mat f;
            { unique_lock<mutex> lk(mtx_);
              cv_.wait(lk, [this]{ return !queue_.empty() || done_; });
              if (queue_.empty() && done_) break;
              f = move(queue_.front()); queue_.pop();
              cvFull_.notify_one(); }
            writer_.write(f);
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
// Tính fade alpha tại frame fi
// ============================================================================

double calcFadeAlpha(int fi, int totalFrames, double fadeInFrames, double fadeOutFrames) {
    double alpha = 1.0;
    if (fadeInFrames  > 0 && fi < fadeInFrames)
        alpha = min(alpha, fi / fadeInFrames);
    if (fadeOutFrames > 0 && fi >= totalFrames - fadeOutFrames)
        alpha = min(alpha, (totalFrames - 1 - fi) / fadeOutFrames);
    return max(0.0, min(1.0, alpha));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    ClipConfig cfg = parseArgs(argc, argv);

    cout << "make_clip\n"
         << "  Output:    " << cfg.output   << "\n"
         << "  BG:        " << (cfg.bgImage.empty() ? "(black)" : cfg.bgImage) << "\n"
         << "  Size:      " << cfg.width << "x" << cfg.height << "\n"
         << "  FPS:       " << cfg.fps   << "\n"
         << "  Duration:  " << cfg.duration << "s\n"
         << "  Fade-in:   " << cfg.fadeIn   << "s\n"
         << "  Fade-out:  " << cfg.fadeOut  << "s\n\n";

    setNumThreads(cfg.numThreads);

    // ── Background ──────────────────────────────────────────────────────────
    Mat bgBase(cfg.height, cfg.width, CV_8UC3, Scalar(0,0,0));
    if (!cfg.bgImage.empty()) {
        Mat raw = imread(cfg.bgImage);
        if (raw.empty()) {
            cerr << "Warning: cannot load background image: " << cfg.bgImage << "\n";
        } else {
            resize(raw, bgBase, Size(cfg.width, cfg.height), 0, 0, INTER_LINEAR);
            if (cfg.bgOpacity < 1.0)
                bgBase.convertTo(bgBase, -1, cfg.bgOpacity, 0);
        }
    }

    // ── Video writer ─────────────────────────────────────────────────────────
    // QUAN TRỌNG: cùng fourcc 'mp4v' + cùng Size + cùng fps với flappy_bird_sim
    AsyncVideoWriter video(cfg.output,
                           VideoWriter::fourcc('m','p','4','v'),
                           cfg.fps,
                           Size(cfg.width, cfg.height));
    if (!video.isOpened()) {
        cerr << "Cannot open video writer: " << cfg.output << "\n";
        return 1;
    }

    int    totalFrames   = (int)(cfg.duration * cfg.fps);
    double fadeInFrames  = cfg.fadeIn  * cfg.fps;
    double fadeOutFrames = cfg.fadeOut * cfg.fps;

    cout << "Rendering " << totalFrames << " frames...\n";

    Mat frame(cfg.height, cfg.width, CV_8UC3);
    Mat black(cfg.height, cfg.width, CV_8UC3, Scalar(0,0,0));

    for (int fi = 0; fi < totalFrames; fi++) {
        // 1. Base = background
        bgBase.copyTo(frame);

        // 2. Fade in / out: blend với black theo alpha
        double alpha = calcFadeAlpha(fi, totalFrames, fadeInFrames, fadeOutFrames);
        if (alpha < 1.0 - 1e-6)
            addWeighted(frame, alpha, black, 1.0 - alpha, 0, frame);

        video.write(frame.clone());

        if (fi % (cfg.fps * 2) == 0)
            printf("  [%5.1f%%] frame %d/%d\n", 100.0*fi/totalFrames, fi, totalFrames);
    }

    cout << "\nDone! => " << cfg.output << "\n";
    cout << "\nGhep voi ffmpeg:\n"
         << "  echo \"file 'intro.mp4'\"      >  list.txt\n"
         << "  echo \"file '" << cfg.output << "'\" >> list.txt\n"
         << "  echo \"file 'outro.mp4'\"      >> list.txt\n"
         << "  ffmpeg -f concat -safe 0 -i list.txt -c copy final.mp4\n";
    return 0;
}