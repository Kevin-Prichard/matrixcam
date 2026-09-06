#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#include "../frame_queue.h"
#include "test_helpers.h"

using namespace std::chrono_literals;

// ─── Tier 2: FrameQueue pure unit tests (no hardware) ────────────────────────

TEST(FrameQueue, PushAndPopSingleThread) {
    FrameQueue q(10);
    VideoFrame f;
    f.width = 4; f.height = 4;
    f.rgbData.assign(4 * 4 * 3, 0xAB);

    q.push(f);
    EXPECT_EQ(q.size(), 1u);

    auto result = q.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->width, 4);
    EXPECT_EQ(result->height, 4);
    EXPECT_EQ(result->rgbData[0], 0xAB);
    EXPECT_EQ(q.size(), 0u);
}

TEST(FrameQueue, PopOnEmptyReturnsNullopt) {
    FrameQueue q(10);
    auto result = q.pop();
    EXPECT_FALSE(result.has_value());
}

TEST(FrameQueue, BackpressureDropFiresAtThreshold) {
    // dropFramesAfter=2: push 5 frames → frames beyond 2 should be dropped
    FrameQueue q(2);

    for (int i = 0; i < 5; ++i) {
        VideoFrame f;
        f.width = i; f.height = 1;
        q.push(std::move(f));
    }

    // Queue should be at most 2 deep (drops were triggered)
    EXPECT_LE(q.size(), 2u);
    EXPECT_GT(q.droppedFramesCount(), 0L);
}

TEST(FrameQueue, DroppedFramesCountAccumulates) {
    FrameQueue q(1);

    for (int i = 0; i < 10; ++i) {
        VideoFrame f;
        f.width = i;
        q.push(std::move(f));
    }
    // We pushed 10 frames into a queue of depth 1: all but the last should be replaced
    EXPECT_GE(q.droppedFramesCount(), 9L);
}

TEST(FrameQueue, PollBlocksUntilFrameAvailable) {
    FrameQueue q(10);

    std::atomic<bool> pollReturned{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(50ms);
        VideoFrame f;
        f.width = 1; f.height = 1;
        q.push(std::move(f));
    });

    // poll() should block until the producer pushes
    bool available = q.poll();
    pollReturned = true;

    EXPECT_TRUE(available);
    EXPECT_TRUE(pollReturned);
    producer.join();

    // Pop the frame to clean up
    q.pop();
}

TEST(FrameQueue, PollReturnsFalseAfterStop) {
    FrameQueue q(10);

    std::thread stopper([&] {
        std::this_thread::sleep_for(30ms);
        q.stop();
    });

    bool available = q.poll();
    EXPECT_FALSE(available);
    stopper.join();
}

TEST(FrameQueue, ConcurrentPushPop) {
    FrameQueue q(32);
    constexpr int kFrames = 200;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < kFrames; ++i) {
            VideoFrame f;
            f.width = i;
            q.push(std::move(f));
            std::this_thread::sleep_for(1ms);
        }
        q.stop();
    });

    std::thread consumer([&] {
        while (true) {
            if (!q.poll()) break;
            auto f = q.pop();
            if (f) ++consumed;
        }
    });

    producer.join();
    consumer.join();

    // At least some frames were consumed (exact count depends on timing/drops)
    EXPECT_GT(consumed.load(), 0);
}

TEST(FrameQueue, StopUnblocksConsumerThread) {
    FrameQueue q(10);
    std::atomic<bool> threadDone{false};

    std::thread t([&] {
        q.poll();   // blocks
        threadDone = true;
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(threadDone.load());

    q.stop();
    t.join();

    EXPECT_TRUE(threadDone.load());
}

TEST(FrameQueue, IsStopped) {
    FrameQueue q(4);
    EXPECT_FALSE(q.isStopped());
    q.stop();
    EXPECT_TRUE(q.isStopped());
}
