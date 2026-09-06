#include "drm_display.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
// drm_mode.h is already included transitively via xf86drm.h / xf86drmMode.h.
// drm_fourcc.h lives in the libdrm include dir (no "drm/" prefix on Debian/Ubuntu).
#include <drm_fourcc.h>

DrmDisplay::~DrmDisplay() {
    try {
        // Restore the original CRTC
        if (savedCrtc_ && fd_ >= 0) {
            drmModeSetCrtc(fd_, savedCrtc_->crtc_id,
                           savedCrtc_->buffer_id,
                           savedCrtc_->x, savedCrtc_->y,
                           &connectorId_, 1, &savedCrtc_->mode);
            drmModeFreeCrtc(savedCrtc_);
            savedCrtc_ = nullptr;
        }
        for (auto& buf : frameBuffers_) {
            if (buf.map && buf.map != MAP_FAILED)
                munmap(buf.map, (size_t)buf.size);
            if (buf.fbId)
                drmModeRmFB(fd_, buf.fbId);
            if (buf.handle) {
                drm_mode_destroy_dumb dd{};
                dd.handle = buf.handle;
                drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            }
        }
        if (fd_ >= 0) close(fd_);
    } catch (...) {}
}

bool DrmDisplay::open(const char* device) {
    fd_ = ::open(device, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) { perror(device); return false; }

    uint64_t hasDumb = 0;
    if (drmGetCap(fd_, DRM_CAP_DUMB_BUFFER, &hasDumb) < 0 || !hasDumb) {
        fprintf(stderr, "DRM driver does not support dumb buffers\n");
        return false;
    }

    drmModeRes* res = drmModeGetResources(fd_);
    if (!res) { fprintf(stderr, "drmModeGetResources failed\n"); return false; }

    // Find first connected connector with modes
    drmModeConnector* conn = nullptr;
    for (int i = 0; i < res->count_connectors; ++i) {
        auto* c = drmModeGetConnector(fd_, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn         = c;
            connectorId_ = c->connector_id;
            break;
        }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) {
        fprintf(stderr, "No connected display found\n");
        drmModeFreeResources(res);
        return false;
    }

    // Resolve encoder → CRTC
    if (conn->encoder_id) {
        drmModeEncoder* enc = drmModeGetEncoder(fd_, conn->encoder_id);
        if (enc) { crtcId_ = enc->crtc_id; drmModeFreeEncoder(enc); }
    }
    if (!crtcId_) {
        for (int i = 0; i < conn->count_encoders && !crtcId_; ++i) {
            drmModeEncoder* enc = drmModeGetEncoder(fd_, conn->encoders[i]);
            if (!enc) continue;
            for (int j = 0; j < res->count_crtcs; ++j) {
                if (enc->possible_crtcs & (1u << j)) {
                    crtcId_ = res->crtcs[j]; break;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }

    savedCrtc_ = drmModeGetCrtc(fd_, crtcId_);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return crtcId_ != 0;
}

std::vector<DrmModeInfo> DrmDisplay::getAvailableModes() const {
    std::vector<DrmModeInfo> modes;
    if (fd_ < 0) return modes;
    drmModeConnector* conn = drmModeGetConnector(fd_, connectorId_);
    if (!conn) return modes;
    for (int i = 0; i < conn->count_modes; ++i) {
        DrmModeInfo m;
        m.width     = conn->modes[i].hdisplay;
        m.height    = conn->modes[i].vdisplay;
        m.refreshHz = (int)conn->modes[i].vrefresh;
        modes.push_back(m);
    }
    drmModeFreeConnector(conn);
    return modes;
}

bool DrmDisplay::setMode(int width, int height) {
    drmModeConnector* conn = drmModeGetConnector(fd_, connectorId_);
    if (!conn) return false;
    bool found = false;
    for (int i = 0; i < conn->count_modes; ++i) {
        if (conn->modes[i].hdisplay == (uint16_t)width &&
            conn->modes[i].vdisplay == (uint16_t)height) {
            activeMode_ = conn->modes[i];
            found = true;
            break;
        }
    }
    drmModeFreeConnector(conn);
    if (!found) return false;
    width_  = width;
    height_ = height;
    return true;
}

bool DrmDisplay::allocateBuffers() {
    for (int i = 0; i < 2; ++i) {
        // Create dumb buffer
        drm_mode_create_dumb create{};
        create.width  = (uint32_t)width_;
        create.height = (uint32_t)height_;
        create.bpp    = 32;
        if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
            perror("DRM_IOCTL_MODE_CREATE_DUMB"); return false;
        }
        frameBuffers_[i].handle = create.handle;
        frameBuffers_[i].size   = create.size;
        stride_                  = create.pitch;

        // Register as DRM_FORMAT_ARGB8888 framebuffer using drmModeAddFB2
        uint32_t handles[4] = { create.handle, 0, 0, 0 };
        uint32_t pitches[4] = { create.pitch,  0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        if (drmModeAddFB2(fd_, (uint32_t)width_, (uint32_t)height_,
                          DRM_FORMAT_ARGB8888,
                          handles, pitches, offsets,
                          &frameBuffers_[i].fbId, 0) < 0) {
            perror("drmModeAddFB2"); return false;
        }

        // Map the dumb buffer into process address space
        drm_mode_map_dumb mapDumb{};
        mapDumb.handle = create.handle;
        if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &mapDumb) < 0) {
            perror("DRM_IOCTL_MODE_MAP_DUMB"); return false;
        }
        frameBuffers_[i].map = static_cast<uint8_t*>(
            mmap(nullptr, (size_t)create.size, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, (off_t)mapDumb.offset));
        if (frameBuffers_[i].map == MAP_FAILED) {
            perror("mmap DRM buffer"); frameBuffers_[i].map = nullptr; return false;
        }

        // Initialize to opaque black: ARGB 0xFF000000
        uint32_t* p32 = reinterpret_cast<uint32_t*>(frameBuffers_[i].map);
        size_t pixCnt = (size_t)(stride_ / 4) * (size_t)height_;
        std::fill(p32, p32 + pixCnt, 0xFF000000u);
    }

    // Point display at buffer[0] (front) with the requested mode
    nextFrameBuf_ = 1;  // back buffer starts at index 1
    if (drmModeSetCrtc(fd_, crtcId_, frameBuffers_[0].fbId,
                       0, 0, &connectorId_, 1, &activeMode_) < 0) {
        perror("drmModeSetCrtc"); return false;
    }
    return true;
}

void DrmDisplay::clearBackBuffer() {
    auto& buf = frameBuffers_[nextFrameBuf_];
    if (!buf.map) return;
    uint32_t* p32 = reinterpret_cast<uint32_t*>(buf.map);
    size_t pixCnt = (size_t)(stride_ / 4) * (size_t)height_;
    std::fill(p32, p32 + pixCnt, 0xFF000000u);
}

uint8_t* DrmDisplay::getBackBuffer() {
    return frameBuffers_[nextFrameBuf_].map;
}

// Static page-flip event handler: sets the flipPending_ flag via userData pointer
void DrmDisplay::pageFlipHandler(int /*fd*/, unsigned int /*seq*/,
                                 unsigned int /*tv_sec*/, unsigned int /*tv_usec*/,
                                 void* userData) {
    bool* pending = static_cast<bool*>(userData);
    *pending = false;
}

void DrmDisplay::flip() {
    flipPending_ = true;
    int backFb = frameBuffers_[nextFrameBuf_].fbId;
    if (drmModePageFlip(fd_, crtcId_, backFb,
                        DRM_MODE_PAGE_FLIP_EVENT, &flipPending_) < 0) {
        perror("drmModePageFlip");
        // Fall back to immediate mode-set on failure
        drmModeSetCrtc(fd_, crtcId_, backFb, 0, 0, &connectorId_, 1, &activeMode_);
        nextFrameBuf_ ^= 1;
        return;
    }

    // Wait for the flip event (vsync)
    drmEventContext evCtx{};
    evCtx.version          = DRM_EVENT_CONTEXT_VERSION;
    evCtx.page_flip_handler = pageFlipHandler;

    while (flipPending_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv{1, 0};
        int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) { perror("select (DRM event)"); break; }
        if (ret == 0) break;  // timeout — unlikely on real hardware
        drmHandleEvent(fd_, &evCtx);
    }

    // Toggle to the other buffer for the next loop iteration
    nextFrameBuf_ ^= 1;
}
