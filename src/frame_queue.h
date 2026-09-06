#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <cstdint>
#include <vector>
#include <chrono>
#include <cstdio>

// ─── Frame data passed from threadCamera → threadRender ───────────────────
struct VideoFrame {
    std::vector<uint8_t> rgbData;  // packed RGB24, row-major, width*height*3 bytes
    int width  = 0;
    int height = 0;
    std::chrono::steady_clock::time_point timestamp;
};

// ─── Thread-safe queue with blocking mutex protection ─────────────────────
// When queue depth exceeds dropFramesAfter, oldest frames are dropped and
// droppedFramesCount is incremented. Logged at count==1 and every 10th drop.
class FrameQueue {
public:
    explicit FrameQueue(int dropFramesAfter = 4)
        : dropFramesAfter_(dropFramesAfter) {}

    // Push a frame. If queue depth >= dropFramesAfter, drop oldest first.
    void push(VideoFrame frame) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_) return;
            while (!queue_.empty() && (int)queue_.size() >= dropFramesAfter_) {
                queue_.pop();  // drop oldest
                ++droppedFramesCount_;
                long cnt = droppedFramesCount_;
                if (cnt == 1 || cnt % 10 == 0) {
                    fprintf(stderr, "[FrameQueue] Dropped frames total: %ld\n", cnt);
                }
            }
            queue_.push(std::move(frame));
        }
        cv_.notify_one();
    }

    // Blocking wait until a frame is available or queue is stopped.
    // Returns true if a frame is available, false if stopped and empty.
    bool poll() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        return !queue_.empty();
    }

    // Non-blocking dequeue. Returns nullopt if queue is empty.
    std::optional<VideoFrame> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        VideoFrame f = std::move(queue_.front());
        queue_.pop();
        return f;
    }

    // Signal all waiting threads to wake and exit.
    void stop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

    long droppedFramesCount() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return droppedFramesCount_;
    }

    bool isStopped() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return stopped_;
    }

private:
    std::queue<VideoFrame>          queue_;
    mutable std::mutex              mutex_;
    std::condition_variable         cv_;
    int                             dropFramesAfter_;
    long                            droppedFramesCount_ = 0;
    bool                            stopped_ = false;
};
