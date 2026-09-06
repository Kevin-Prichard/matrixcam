#pragma once
#include <string>
#include <vector>
#include <cstdint>

class Camera {
public:
    Camera()  = default;
    ~Camera();

    // Open the device and verify it is a V4L2 video capture device.
    // Also enumerates available frame sizes (stored internally).
    bool open(const std::string& device);

    // Negotiate format at the requested resolution/fps.
    // Tries MJPEG first (best USB bandwidth), falls back to YUYV.
    // Actual negotiated width/height stored in width_/height_.
    bool configure(int width, int height, int fps);

    // Enqueue all MMAP buffers and start streaming.
    bool startCapture();

    // Stop streaming (STREAMOFF). Safe to call if not started.
    void stopCapture();

    bool isOpen()      const { return fd_ >= 0; }
    bool isStreaming() const { return streaming_; }
    int  frameWidth()  const { return width_;  }
    int  frameHeight() const { return height_; }

    struct FrameSize { int width; int height; };
    const std::vector<FrameSize>& availableFrameSizes() const { return frameSizes_; }

    // Block (up to 2 s) until a frame arrives; returns RGB24 bytes (width*height*3).
    // Returns empty vector on timeout or error.
    std::vector<uint8_t> captureFrame();

private:
    int      fd_          = -1;
    int      width_       = 0;
    int      height_      = 0;
    uint32_t pixelFormat_ = 0;   // V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_YUYV
    bool     streaming_   = false;

    struct MmapBuffer { void* start = nullptr; size_t length = 0; };
    std::vector<MmapBuffer>  buffers_;
    std::vector<FrameSize>   frameSizes_;

    // Decode one MJPEG frame (V4L2 buffer) → RGB24
    static bool   decodeMjpeg(const uint8_t* jpegData, size_t jpegSize,
                               uint8_t* rgb, int width, int height);
    // ITU-R BT.601 YUYV → RGB24 conversion
    static void   yuyvToRgb(const uint8_t* yuyv, uint8_t* rgb, int width, int height);
    static uint8_t clampByte(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
};
