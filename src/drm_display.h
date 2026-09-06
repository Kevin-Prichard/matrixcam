#pragma once
#include <cstdint>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct DrmModeInfo {
    int width, height, refreshHz;
};

// ─── DRM/KMS display output using dumb buffers and page-flip events ───────
class DrmDisplay {
public:
    DrmDisplay() = default;
    ~DrmDisplay();

    // Open the DRM device, find a connected connector, and discover the CRTC.
    bool open(const char* device = "/dev/dri/card0");

    // Return all display modes advertised by the active connector.
    std::vector<DrmModeInfo> getAvailableModes() const;

    // Store the mode matching width x height (does not apply it yet).
    bool setMode(int width, int height);

    // Allocate two dumb framebuffers (DRM_FORMAT_ARGB8888) via drmModeAddFB2,
    // then apply the stored mode via drmModeSetCrtc.
    bool allocateBuffers();

    // Fill the back buffer with ARGB 0xFF000000 (opaque black).
    void clearBackBuffer();

    // Direct pointer to the back buffer pixels (mmap'd).
    uint8_t* getBackBuffer();

    // Present back buffer using drmModePageFlip + drmHandleEvent (vsync'd).
    // Then toggles front/back index with nextFrameBuf ^= 1.
    void flip();

    int      width()  const { return width_;  }
    int      height() const { return height_; }
    uint32_t stride() const { return stride_; }

private:
    int      fd_          = -1;
    uint32_t connectorId_ = 0;
    uint32_t crtcId_      = 0;

    struct Buffer {
        uint32_t handle = 0;
        uint32_t fbId   = 0;
        uint64_t size   = 0;
        uint8_t* map    = nullptr;
    };

    // frameBuffers[0] and frameBuffers[1] — spec calls this static array "frameBuffers"
    Buffer   frameBuffers_[2];
    int      nextFrameBuf_ = 0;   // index of back buffer (toggles with ^= 1)

    int      width_   = 0;
    int      height_  = 0;
    uint32_t stride_  = 0;

    drmModeModeInfo activeMode_{};
    drmModeCrtc*    savedCrtc_ = nullptr;

    // Page-flip completion flag written by the event handler
    bool flipPending_ = false;
    static void pageFlipHandler(int fd, unsigned int seq,
                                unsigned int tv_sec, unsigned int tv_usec,
                                void* userData);
};
