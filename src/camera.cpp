#include "camera.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <turbojpeg.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static void fccStr(uint32_t fcc, char out[5]) {
    out[0] = (char)(fcc & 0xFF);         out[1] = (char)((fcc >> 8)  & 0xFF);
    out[2] = (char)((fcc >> 16) & 0xFF); out[3] = (char)((fcc >> 24) & 0xFF);
    out[4] = '\0';
}

Camera::~Camera() {
    try {
        if (streaming_) stopCapture();
        for (auto& b : buffers_)
            if (b.start && b.start != MAP_FAILED)
                munmap(b.start, b.length);
        if (fd_ >= 0) close(fd_);
    } catch (...) {}
}

bool Camera::open(const std::string& device) {
    fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { perror(device.c_str()); return false; }

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); return false; }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s: not a video capture device\n", device.c_str()); return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s: does not support streaming\n", device.c_str()); return false;
    }

    // Enumerate frame sizes — prefer MJPEG list, fall back to YUYV
    const uint32_t fmtsToEnum[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    for (uint32_t pixFmt : fmtsToEnum) {
        v4l2_frmsizeenum fsize{};
        fsize.pixel_format = pixFmt;
        for (fsize.index = 0; xioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0; ++fsize.index) {
            if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                frameSizes_.push_back({(int)fsize.discrete.width, (int)fsize.discrete.height});
            else {
                frameSizes_.push_back({(int)fsize.stepwise.max_width, (int)fsize.stepwise.max_height});
                break;
            }
        }
        if (!frameSizes_.empty()) break;
    }
    return true;
}

bool Camera::configure(int width, int height, int fps) {
    // Try MJPEG first — compressed; supports higher resolutions over USB.
    // Fall back to YUYV for cameras that don't support MJPEG.
    const uint32_t fmtsToTry[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    bool formatSet = false;

    for (uint32_t tryFmt : fmtsToTry) {
        v4l2_format fmt{};
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = (uint32_t)width;
        fmt.fmt.pix.height      = (uint32_t)height;
        fmt.fmt.pix.pixelformat = tryFmt;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;

        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) continue;

        char got[5]; fccStr(fmt.fmt.pix.pixelformat, got);
        char tried[5]; fccStr(tryFmt, tried);

        if (fmt.fmt.pix.pixelformat == tryFmt) {
            width_       = (int)fmt.fmt.pix.width;
            height_      = (int)fmt.fmt.pix.height;
            pixelFormat_ = fmt.fmt.pix.pixelformat;
            fprintf(stderr, "[Camera::configure] accepted: %dx%d  fmt=%.4s  "
                            "bytesperline=%u  sizeimage=%u\n",
                    width_, height_, tried,
                    fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
            formatSet = true;
            break;
        }
        fprintf(stderr, "[Camera::configure] requested %.4s but driver returned "
                        "%.4s — trying next\n", tried, got);
    }

    if (!formatSet) {
        fprintf(stderr, "[Camera::configure] ERROR: could not negotiate "
                        "MJPEG or YUYV at %dx%d\n", width, height);
        return false;
    }

    // Best-effort frame rate
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = (uint32_t)fps;
    xioctl(fd_, VIDIOC_S_PARM, &parm);

    // Read back actual frame rate
    parm = {}; parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_PARM, &parm) == 0) {
        auto& tf = parm.parm.capture.timeperframe;
        if (tf.numerator > 0)
            fprintf(stderr, "[Camera::configure] actual frame rate: %.2f fps\n",
                    (float)tf.denominator / (float)tf.numerator);
    }

    // Request MMAP buffers
    v4l2_requestbuffers req{};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); return false; }
    if (req.count < 2) { fprintf(stderr, "Insufficient V4L2 buffers\n"); return false; }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); return false; }
        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) { perror("mmap camera buffer"); return false; }
    }
    return true;
}

bool Camera::startCapture() {
    for (uint32_t i = 0; i < (uint32_t)buffers_.size(); ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); return false; }
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); return false; }
    streaming_ = true;
    char fcc[5]; fccStr(pixelFormat_, fcc);
    fprintf(stderr, "[Camera::startCapture] STREAMON OK — %zu buffers, %dx%d %.4s\n",
            buffers_.size(), width_, height_, fcc);
    return true;
}

void Camera::stopCapture() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

std::vector<uint8_t> Camera::captureFrame() {
    fd_set fds; FD_ZERO(&fds); FD_SET(fd_, &fds);
    timeval tv{2, 0};
    int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret == 0) {
        fprintf(stderr, "[Camera::captureFrame] select() timed out (2s) — "
                        "camera not sending frames\n");
        return {};
    }
    if (ret < 0) {
        if (errno != EINTR) perror("[Camera::captureFrame] select");
        return {};
    }

    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return {};
        perror("VIDIOC_DQBUF"); return {};
    }

    std::vector<uint8_t> rgb((size_t)(width_ * height_ * 3));
    const auto* src = static_cast<const uint8_t*>(buffers_[buf.index].start);
    bool ok = false;

    if (pixelFormat_ == V4L2_PIX_FMT_MJPEG) {
        ok = decodeMjpeg(src, buf.bytesused, rgb.data(), width_, height_);
    } else {
        // YUYV or any other raw format we treat as YUYV
        yuyvToRgb(src, rgb.data(), width_, height_);
        ok = true;
    }

    xioctl(fd_, VIDIOC_QBUF, &buf);  // re-queue regardless of decode result
    if (!ok) return {};
    return rgb;
}

// ─── MJPEG → RGB24 via libturbojpeg ──────────────────────────────────────────
bool Camera::decodeMjpeg(const uint8_t* jpegData, size_t jpegSize,
                          uint8_t* rgb, int width, int height)
{
    tjhandle tj = tjInitDecompress();
    if (!tj) {
        fprintf(stderr, "[Camera::decodeMjpeg] tjInitDecompress failed\n");
        return false;
    }
    int ret = tjDecompress2(tj,
                            const_cast<unsigned char*>(jpegData),
                            (unsigned long)jpegSize,
                            rgb,
                            width,
                            width * 3,   // pitch (bytes per row)
                            height,
                            TJPF_RGB,
                            TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE);
    if (ret != 0)
        fprintf(stderr, "[Camera::decodeMjpeg] error: %s\n", tjGetErrorStr2(tj));
    tjDestroy(tj);
    return ret == 0;
}

// ─── ITU-R BT.601 YUYV → RGB24 ───────────────────────────────────────────────
void Camera::yuyvToRgb(const uint8_t* yuyv, uint8_t* rgb, int width, int height) {
    for (int i = 0; i < width * height / 2; ++i) {
        int y0 = yuyv[4*i+0], cb = yuyv[4*i+1];
        int y1 = yuyv[4*i+2], cr = yuyv[4*i+3];
        int c0 = y0 - 16, c1 = y1 - 16;
        int d  = cb - 128, e = cr - 128;
        rgb[6*i+0] = clampByte((298*c0 + 409*e         + 128) >> 8);
        rgb[6*i+1] = clampByte((298*c0 - 100*d - 208*e + 128) >> 8);
        rgb[6*i+2] = clampByte((298*c0 + 516*d         + 128) >> 8);
        rgb[6*i+3] = clampByte((298*c1 + 409*e         + 128) >> 8);
        rgb[6*i+4] = clampByte((298*c1 - 100*d - 208*e + 128) >> 8);
        rgb[6*i+5] = clampByte((298*c1 + 516*d         + 128) >> 8);
    }
}
