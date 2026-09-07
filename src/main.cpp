#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <csignal>
#include <stdexcept>
#include <random>
#include <getopt.h>
#include <sys/stat.h>
#include <ctime>
#include <mutex>

#include "frame_queue.h"
#include "glyph.h"
#include "drm_display.h"
#include "camera.h"

// ─── Debug logger ─────────────────────────────────────────────────────────────
// Writes to both stderr AND an optional log file so output survives DRM
// taking over the local console. Enable with --debug-log=/path/to/file.
static FILE*       g_logFile  = nullptr;
static std::mutex  g_logMutex;

static void dbgLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbgLog(const char* fmt, ...) {
    // timestamp prefix
    char tsbuf[32];
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    strftime(tsbuf, sizeof(tsbuf), "%H:%M:%S", tm_info);

    std::lock_guard<std::mutex> lk(g_logMutex);
    va_list ap;

    // stderr
    fprintf(stderr, "[%s] ", tsbuf);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);

    // log file (if open)
    if (g_logFile) {
        fprintf(g_logFile, "[%s] ", tsbuf);
        va_start(ap, fmt); vfprintf(g_logFile, fmt, ap); va_end(ap);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
}

// ─── Application globals ─────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static FrameQueue*       g_queue = nullptr;  // set before threads start

// ─── Signal handler ──────────────────────────────────────────────────────────
static void sigHandler(int signo) {
    dbgLog("[main] Signal %d received — initiating shutdown", signo);
    g_running = false;
    if (g_queue) g_queue->stop();
}

static void installSignalHandlers() {
    // Install for all signals 1-31 except SIGKILL(9), SIGSTOP(19), SIGWINCH(28)
    struct sigaction sa{};
    sa.sa_handler = sigHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (int sig = 1; sig <= 31; ++sig) {
        if (sig == SIGKILL || sig == SIGSTOP || sig == SIGWINCH) continue;
        sigaction(sig, &sa, nullptr);
    }
}

// ─── CellStat: per-character-cell statistics ─────────────────────────────────
struct CellStat {
    float avgLum   = 0.f;  // average luminosity 0-100 (normalized percent)
    float avgDark  = 0.f;  // avg r+g+b sum for dark pixels / total pixel count
    float avgLight = 0.f;  // avg r+g+b sum for light pixels / total pixel count
};

// ─── threadCamera ────────────────────────────────────────────────────────────
static void threadCamera(Camera* cam) {
    try {
        dbgLog("[threadCamera] started, camera %dx%d",
               cam->frameWidth(), cam->frameHeight());
        long frameCount = 0;
        long emptyCount = 0;
        while (g_running) {
            auto rgb = cam->captureFrame();
            if (rgb.empty()) {
                ++emptyCount;
                if (emptyCount == 1 || emptyCount % 5 == 0)
                    dbgLog("[threadCamera] captureFrame() returned empty "
                           "(count=%ld)", emptyCount);
                continue;
            }

            ++frameCount;
            if (frameCount == 1 || frameCount % 100 == 0)
                dbgLog("[threadCamera] frame #%ld  size=%zu  %dx%d",
                       frameCount, rgb.size(),
                       cam->frameWidth(), cam->frameHeight());

            VideoFrame f;
            f.rgbData   = std::move(rgb);
            f.width     = cam->frameWidth();
            f.height    = cam->frameHeight();
            f.timestamp = std::chrono::steady_clock::now();
            g_queue->push(std::move(f));
        }
        dbgLog("[threadCamera] exiting after %ld frames", frameCount);
    } catch (const std::exception& e) {
        dbgLog("[threadCamera] Exception: %s", e.what());
        g_running = false;
        if (g_queue) g_queue->stop();
    } catch (...) {
        dbgLog("[threadCamera] Unknown exception");
        g_running = false;
        if (g_queue) g_queue->stop();
    }
}

// ─── threadRender ────────────────────────────────────────────────────────────
static void threadRender(DrmDisplay* drm,
                         const GlyphAtlas* atlas,
                         int camFrameWidth, int camFrameHeight,
                         int conCharWidth,  int conCharHeight,
                         int cellWidth,     int cellHeight,
                         int glyphPovMin,   int glyphPovMax)
{
    try {
        dbgLog("[threadRender] started  cam=%dx%d  grid=%dx%d  cell=%dx%d  drm=%dx%d  pov=[%d,%d)",
               camFrameWidth, camFrameHeight,
               conCharWidth, conCharHeight,
               cellWidth, cellHeight,
               drm->width(), drm->height(),
               glyphPovMin, glyphPovMax);

        const uint32_t stride = drm->stride();
        const int      drmW   = drm->width();
        const int      drmH   = drm->height();

        constexpr uint32_t FG_ARGB = 0xFF0BF445u;

        const int cellCount = conCharWidth * conCharHeight;
        std::vector<CellStat> cellStats((size_t)cellCount);

        struct CellAccum {
            double sumRGB   = 0;
            double sumDark  = 0;
            double sumLight = 0;
            int    nDark    = 0;
            int    nLight   = 0;
            int    count    = 0;
        };
        std::vector<CellAccum> accums((size_t)cellCount);

        std::vector<int> rowToCellY;
        std::vector<int> colToCellX;
        std::vector<int> cellXPixels((size_t)conCharWidth);
        std::vector<int> cellYPixels((size_t)conCharHeight);
        std::array<const GlyphRaster*, 101> frameGlyphCache{};

        rowToCellY.resize((size_t)camFrameWidth);
        colToCellX.resize((size_t)camFrameHeight);
        cellXPixels.resize((size_t)conCharWidth);
            cellYPixels.resize((size_t)conCharHeight);

        for (int py = 0; py < camFrameWidth; ++py) {
            int cy = py * conCharHeight / camFrameWidth;
            if (cy >= conCharHeight) cy = conCharHeight - 1;
            rowToCellY[(size_t)py] = cy;
        }
        for (int px = 0; px < camFrameHeight; ++px) {
            int cx = px * conCharWidth / camFrameHeight;
            if (cx >= conCharWidth) cx = conCharWidth - 1;
            colToCellX[(size_t)px] = cx;
        }
        for (int cx = 0; cx < conCharWidth; ++cx) {
            cellXPixels[(size_t)cx] = cx * cellWidth;
        }
        for (int cy = 0; cy < conCharHeight; ++cy) {
            cellYPixels[(size_t)cy] = cy * cellHeight;
        }

        constexpr int MID = 128;
        long renderCount = 0;

        while (g_running) {
            if (!g_queue->poll()) {
                dbgLog("[threadRender] poll() returned false — stopping");
                break;
            }

            auto frameOpt = g_queue->pop();
            if (!frameOpt) continue;

            const VideoFrame& frame = *frameOpt;
            const uint8_t* rgb = frame.rgbData.data();
            const int fW = frame.width;
            const int fH = frame.height;

            ++renderCount;
            if (renderCount == 1 || renderCount % 100 == 0)
                dbgLog("[threadRender] rendering frame #%ld  src=%dx%d",
                       renderCount, fW, fH);

            // ── scan-frame ────────────────────────────────────────────────
            for (auto& a : accums) { a = {}; }

            for (int py = 0; py < fH; ++py) {
                int cy = rowToCellY[(size_t)py];
                const uint8_t* row = rgb + (size_t)py * (size_t)fW * 3;
                for (int px = 0; px < fW; ++px) {
                    int cx = colToCellX[(size_t)px];
                    uint8_t r = row[px * 3 + 0];
                    uint8_t g = row[px * 3 + 1];
                    uint8_t b = row[px * 3 + 2];
                    int sum = (int)r + (int)g + (int)b;
                    CellAccum& acc = accums[(size_t)(cy * conCharWidth + cx)];
                    acc.sumRGB += sum;
                    ++acc.count;
                    if (sum / 3 < MID)       { acc.sumDark  += sum; ++acc.nDark;  }
                    else                      { acc.sumLight += sum; ++acc.nLight; }
                }
            }

            for (int i = 0; i < cellCount; ++i) {
                const CellAccum& acc = accums[(size_t)i];
                if (acc.count == 0) continue;
                cellStats[(size_t)i].avgLum =
                    (float)(acc.sumRGB / (3.0 * 255.0 * acc.count)) * 100.0f;
                cellStats[(size_t)i].avgDark  =
                    acc.nDark  > 0 ? (float)(acc.sumDark  / acc.nDark)  : 0.f;
                cellStats[(size_t)i].avgLight =
                    acc.nLight > 0 ? (float)(acc.sumLight / acc.nLight) : 0.f;
            }

            // Debug: on first frame log min/max luminosity seen
            if (renderCount == 1) {
                float minL = 1e9f, maxL = -1e9f;
                for (const auto& cs : cellStats) {
                    if (cs.avgLum < minL) minL = cs.avgLum;
                    if (cs.avgLum > maxL) maxL = cs.avgLum;
                }
                dbgLog("[threadRender] first frame cell lum range: %.1f – %.1f",
                       minL, maxL);
                // Also log a sample glyph lookup
                int sampleKey = (int)std::round((minL + maxL) * 0.5f);
                const GlyphRaster* sample = atlas->findClosest(sampleKey);
                dbgLog("[threadRender] sample lookup lum=%d → glyph=%s codepoint=U+%04X lum=%d",
                       sampleKey,
                       sample ? "found" : "NULL",
                       sample ? sample->codepoint : 0u,
                       sample ? sample->luminosity : -1);
            }

            // ── composit-console-frame ─────────────────────────────────────
            drm->clearBackBuffer();
            uint8_t* fb = drm->getBackBuffer();
            frameGlyphCache.fill(nullptr);

            int glyphsBlitted = 0;
            for (int cy = 0; cy < conCharHeight; ++cy) {
                const int destY = cellYPixels[(size_t)cy];
                for (int cx = 0; cx < conCharWidth; ++cx) {
                    const CellStat& cs = cellStats[(size_t)(cy * conCharWidth + cx)];
                    int lumKey = (int)std::lround(cs.avgLum);
                    if (lumKey < 0) lumKey = 0;
                    else if (lumKey > 100) lumKey = 100;

                    const GlyphRaster* thisRaster = frameGlyphCache[(size_t)lumKey];
                    if (!thisRaster) {
                        thisRaster = atlas->findWithPov(lumKey, glyphPovMin, glyphPovMax);
                        frameGlyphCache[(size_t)lumKey] = thisRaster;
                    }
                    if (!thisRaster) continue;

                    const int destX = cellXPixels[(size_t)cx];

                    for (int gy = 0; gy < thisRaster->height; ++gy) {
                        int sy = destY + gy;
                        if (sy >= drmH) break;
                        for (int gx = 0; gx < thisRaster->width; ++gx) {
                            uint8_t alpha = thisRaster->alpha[(size_t)(gy * thisRaster->width + gx)];
                            if (alpha == 0) continue;
                            int sx = destX + gx;
                            if (sx >= drmW) break;
                            uint32_t* pixel = reinterpret_cast<uint32_t*>(
                                fb + (size_t)sy * stride + (size_t)sx * 4);
                            if (alpha == 255) {
                                *pixel = FG_ARGB;
                            } else {
                                uint8_t fR = (uint8_t)((0x0B * alpha) / 255);
                                uint8_t fG = (uint8_t)((0xF4 * alpha) / 255);
                                uint8_t fB = (uint8_t)((0x45 * alpha) / 255);
                                *pixel = 0xFF000000u
                                       | ((uint32_t)fR << 16)
                                       | ((uint32_t)fG << 8)
                                       |  (uint32_t)fB;
                            }
                            ++glyphsBlitted;
                        }
                    }
                }
            }

            if (renderCount == 1)
                dbgLog("[threadRender] first frame: %d pixels blitted to back buffer",
                       glyphsBlitted);

            drm->flip();

            if (renderCount == 1)
                dbgLog("[threadRender] first flip complete");
        }
        dbgLog("[threadRender] exiting after %ld frames rendered", renderCount);
    } catch (const std::exception& e) {
        dbgLog("[threadRender] Exception: %s", e.what());
        g_running = false;
        if (g_queue) g_queue->stop();
    } catch (...) {
        dbgLog("[threadRender] Unknown exception");
        g_running = false;
        if (g_queue) g_queue->stop();
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
static bool parseWxH(const char* s, int& w, int& h) {
    return sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}
static bool fileExists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0;
}

static void printUsage(const char* prog) {
    fprintf(stderr,
        "Usage: %s OPTIONS\n"
        "\n"
        "  --console-dimensions WxH        Character grid dimensions (default 80x30)\n"
        "  --console-resolution WxH        Screen pixel resolution (default 1920x1080)\n"
        "  --camera PATH                   Camera device (default /dev/video0)\n"
        "  --camera-config WxHxFPS         Camera frame size and rate (default 1920x1080x30)\n"
        "  --font-file PATH                Font file (repeatable, default Noto Hentaigana)\n"
        "  --drop-frames-after N           Drop queue frames beyond this depth (default 4)\n"
        "  --drm-device PATH               DRM device (default /dev/dri/card0)\n"
        "  --debug-log PATH                Tee all log output to this file (e.g. /tmp/matx.log)\n"
        "  --glyph-pov-min N               Min frames to hold a glyph per intensity bucket (default 15)\n"
        "  --glyph-pov-max N               Max frames to hold a glyph per intensity bucket (default 60)\n"
        "  --help                          Show this message\n",
        prog);
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    try {
        // ── Defaults ──────────────────────────────────────────────────────────
        int conCharWidth  = 80;
        int conCharHeight = 30;
        int conPixelsWide = 1920;
        int conPixelsHigh = 1080;
        std::string cameraDevice    = "/dev/video0";
        int camFrameWidth  = 1920;
        int camFrameHeight = 1080;
        int frameRate      = 30;
        std::vector<std::string> fontFiles;
        int dropFramesAfter = 4;
        std::string drmDevice  = "/dev/dri/card0";
        std::string debugLog;    // path to optional log file
        int glyphPovMin = 15;
        int glyphPovMax = 60;

        static const option longOpts[] = {
            {"console-dimensions",  required_argument, nullptr, 'd'},
            {"console-resolution",  required_argument, nullptr, 'r'},
            {"camera",              required_argument, nullptr, 'c'},
            {"camera-config",       required_argument, nullptr, 'C'},
            {"font-file",           required_argument, nullptr, 'f'},
            {"drop-frames-after",   required_argument, nullptr, 'D'},
            {"drm-device",          required_argument, nullptr, 'G'},
            {"debug-log",           required_argument, nullptr, 'L'},
            {"glyph-pov-min",       required_argument, nullptr, 'p'},
            {"glyph-pov-max",       required_argument, nullptr, 'P'},
            {"help",                no_argument,       nullptr, 'h'},
            {nullptr, 0, nullptr, 0}
        };

        int opt;
        while ((opt = getopt_long(argc, argv, "", longOpts, nullptr)) != -1) {
            switch (opt) {
            case 'd':
                if (!parseWxH(optarg, conCharWidth, conCharHeight)) {
                    fprintf(stderr, "Invalid --console-dimensions: %s\n", optarg);
                    return 1;
                }
                break;
            case 'r':
                if (!parseWxH(optarg, conPixelsWide, conPixelsHigh)) {
                    fprintf(stderr, "Invalid --console-resolution: %s\n", optarg);
                    return 1;
                }
                break;
            case 'c':
                cameraDevice = optarg;
                break;
            case 'C': {
                int w, h, fps;
                if (sscanf(optarg, "%dx%dx%d", &w, &h, &fps) != 3 ||
                    w <= 0 || h <= 0 || fps <= 0) {
                    fprintf(stderr, "Invalid --camera-config: %s (expected WxHxFPS)\n", optarg);
                    return 1;
                }
                camFrameWidth  = w;
                camFrameHeight = h;
                frameRate         = fps;
                break;
            }
            case 'f':
                fontFiles.emplace_back(optarg);
                break;
            case 'D': {
                int n = atoi(optarg);
                if (n <= 0) {
                    fprintf(stderr, "Invalid --drop-frames-after: %s (must be > 0)\n", optarg);
                    return 1;
                }
                dropFramesAfter = n;
                break;
            }
            case 'G':
                drmDevice = optarg;
                break;
            case 'L':
                debugLog = optarg;
                break;
            case 'p': {
                int n = atoi(optarg);
                if (n <= 0) {
                    fprintf(stderr, "Invalid --glyph-pov-min: %s (must be > 0)\n", optarg);
                    return 1;
                }
                glyphPovMin = n;
                break;
            }
            case 'P': {
                int n = atoi(optarg);
                if (n <= 0) {
                    fprintf(stderr, "Invalid --glyph-pov-max: %s (must be > 0)\n", optarg);
                    return 1;
                }
                glyphPovMax = n;
                break;
            }
            case 'h':
            default:
                printUsage(argv[0]);
                return 0;
            }
        }

        // ── Default font if none specified ─────────────────────────────────────
        if (fontFiles.empty()) {
            fontFiles.emplace_back(
                "fonts/Noto_Serif_Hentaigana/NotoSerifHentaigana-VariableFont_wght.ttf");
        }

        // ── Open debug log file if requested ───────────────────────────────────
        if (!debugLog.empty()) {
            g_logFile = fopen(debugLog.c_str(), "w");
            if (!g_logFile)
                fprintf(stderr, "Warning: cannot open debug log '%s': %s\n",
                        debugLog.c_str(), strerror(errno));
        }

        // ── Startup banner ─────────────────────────────────────────────────────
        dbgLog("[main] matrixcam starting");
        dbgLog("[main] grid=%dx%d  resolution=%dx%d  camera=%s  cameraRes=%dx%d@%dfps",
               conCharWidth, conCharHeight,
               conPixelsWide, conPixelsHigh,
               cameraDevice.c_str(),
               camFrameWidth, camFrameHeight, frameRate);
        dbgLog("[main] drm-device=%s  drop-frames-after=%d  glyph-pov=[%d,%d)",
               drmDevice.c_str(), dropFramesAfter, glyphPovMin, glyphPovMax);
        for (size_t i = 0; i < fontFiles.size(); ++i)
            dbgLog("[main] font-file[%zu]=%s", i, fontFiles[i].c_str());

        // ── initialization1: open camera, read available resolutions ───────────
        Camera cam;
        if (!cam.open(cameraDevice)) {
            fprintf(stderr, "Error: cannot open camera device: %s\n", cameraDevice.c_str());
            return 1;
        }

        // ── Validate CLI params ────────────────────────────────────────────────
        // console-dimensions: both > 0
        if (conCharWidth <= 0 || conCharHeight <= 0) {
            fprintf(stderr, "Error: --console-dimensions must have both values > 0\n");
            printUsage(argv[0]); return 1;
        }
        // console-resolution: both > 0
        if (conPixelsWide <= 0 || conPixelsHigh <= 0) {
            fprintf(stderr, "Error: --console-resolution must have both values > 0\n");
            printUsage(argv[0]); return 1;
        }
        // camera-config: check resolution is available (warn if not, use it anyway)
        {
            bool resAvail = false;
            for (const auto& fs : cam.availableFrameSizes()) {
                if (fs.width == camFrameWidth && fs.height == camFrameHeight) {
                    resAvail = true; break;
                }
            }
            if (!resAvail && !cam.availableFrameSizes().empty()) {
                fprintf(stderr, "Warning: camera resolution %dx%d not in discrete list; "
                                "driver will negotiate.\n",
                        camFrameWidth, camFrameHeight);
            }
        }
        // font-file-checks: non-zero list and paths exist
        if (fontFiles.empty()) {
            fprintf(stderr, "Error: at least one --font-file must be specified\n");
            return 1;
        }
        for (const auto& fp : fontFiles) {
            if (!fileExists(fp)) {
                fprintf(stderr, "Error: font file not found: %s\n", fp.c_str());
                return 1;
            }
        }

        // ── initialization2 ───────────────────────────────────────────────────

        // obtain-DRM-resolutions + drm-initialization
        DrmDisplay drm;
        if (!drm.open(drmDevice.c_str())) return 1;

        auto availModes = drm.getAvailableModes();
        bool resOk = false;
        for (const auto& m : availModes) {
            if (m.width == conPixelsWide && m.height == conPixelsHigh) {
                resOk = true; break;
            }
        }
        if (!resOk) {
            fprintf(stdout, "Error: resolution %dx%d not available. Available modes:\n",
                    conPixelsWide, conPixelsHigh);
            for (const auto& m : availModes)
                fprintf(stdout, "  %dx%d @ %dHz\n", m.width, m.height, m.refreshHz);
            return 1;
        }
        if (!drm.setMode(conPixelsWide, conPixelsHigh)) {
            fprintf(stderr, "Error: failed to set DRM mode %dx%d\n",
                    conPixelsWide, conPixelsHigh);
            return 1;
        }
        if (!drm.allocateBuffers()) {
            fprintf(stderr, "Error: failed to allocate DRM framebuffers\n");
            return 1;
        }

        // configure-camera: set resolution per camera-config
        if (!cam.configure(camFrameWidth, camFrameHeight, frameRate)) {
            fprintf(stderr, "Error: failed to configure camera at %dx%d@%dfps\n",
                    camFrameWidth, camFrameHeight, frameRate);
            return 1;
        }
        if (!cam.startCapture()) {
            fprintf(stderr, "Error: failed to start camera capture\n");
            return 1;
        }

        // determine-char-cell-dims: float divide then round to int
        int cellWidth  = (int)std::round((float)conPixelsWide  / (float)conCharWidth);
        int cellHeight = (int)std::round((float)conPixelsHigh  / (float)conCharHeight);
        dbgLog("[main] cell size: %dx%d px", cellWidth, cellHeight);

        // ingest-font-files: load, iterate glyphs, rasterize, build lumGlyphRasterMap
        GlyphAtlas atlas;
        dbgLog("[main] loading glyph atlas...");
        if (!atlas.load(fontFiles, cellWidth, cellHeight)) {
            dbgLog("[main] ERROR: failed to load font files");
            return 1;
        }
        dbgLog("[main] atlas ready: %zu buckets, %zu glyphs",
               atlas.getLumMap().size(), atlas.glyphCount());

        // ── framesToRender queue ───────────────────────────────────────────────
        FrameQueue framesToRender(dropFramesAfter);
        g_queue = &framesToRender;

        // ── Install signal handlers (after setup, before threads) ──────────────
        installSignalHandlers();

        dbgLog("[main] launching threads");
        // ── Launch threads ─────────────────────────────────────────────────────
        std::thread tCamera(threadCamera, &cam);
        std::thread tRender(threadRender, &drm, &atlas,
                            camFrameWidth, camFrameHeight,
                            conCharWidth,  conCharHeight,
                            cellWidth,     cellHeight,
                            glyphPovMin,   glyphPovMax);

        // Wait for both threads to finish
        tCamera.join();
        tRender.join();

        // ── Shutdown ──────────────────────────────────────────────────────────
        dbgLog("[main] threads joined — shutting down");

        // close camera
        cam.stopCapture();

        // flush the framesToRender queue
        while (framesToRender.size() > 0) {
            framesToRender.pop();
        }

        // release all allocated objects (handled by destructors of drm, cam, atlas)
        g_queue = nullptr;

        dbgLog("[main] shutdown complete. Dropped frames: %ld",
               framesToRender.droppedFramesCount());
        if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
        return 0;

    } catch (const std::exception& e) {
        fprintf(stderr, "[main] Fatal exception: %s\n", e.what());
        return 2;
    } catch (...) {
        fprintf(stderr, "[main] Unknown fatal exception\n");
        return 2;
    }
}
