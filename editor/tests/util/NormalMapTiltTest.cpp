// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// util/NormalMapTilt: the measured tilt of real normal maps, and its cache never making a
// lookup wait for a decode (the Inspector and Terrain window call request() every frame on
// the UI thread): not when the file changed while its old version is still being measured,
// nor while measure() (check, check_shader) decodes another file.

#include <gtest/gtest.h>

#include "util/NormalMapTilt.h"

#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace jasperengine;
using editor::NormalMapTilt;
using editor::NormalMapTiltResult;
namespace fs = std::filesystem;

namespace {

    using Clock = std::chrono::steady_clock;

    // A lookup that must return promptly: run on its own thread, so a lookup that blocks
    // fails the test (after unblocking it with release) instead of hanging it
    template <typename Function>
    bool returnsWithin(std::chrono::milliseconds limit, Function lookup, const std::function<void()>& release) {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::thread worker([&]() {
            lookup();
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            cv.notify_all();
        });
        bool prompt;
        {
            std::unique_lock<std::mutex> lock(mutex);
            prompt = cv.wait_for(lock, limit, [&]() { return done; });
        }
        if (!prompt)
            release();
        worker.join();
        return prompt;
    }

    // A measure function for the cache tests: the file holds "<degrees>" or "slow <degrees>";
    // a slow one waits at the gate until it opens. Its state is shared with the function, so a
    // measure thread outliving the test still finds it.
    class SlowMeasure {
    public:
        SlowMeasure(): state(std::make_shared<State>()) {
            NormalMapTilt::clearCache();
            std::shared_ptr<State> shared = state;
            NormalMapTilt::setMeasureFunction([shared](const fs::path& file, float& degrees) { return shared->run(file, degrees); });
        }

        ~SlowMeasure() {
            finish();
            // the measures this test started finish before the next test
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait_for(lock, std::chrono::seconds(10), [this]() { return state->running == 0; });
            lock.unlock();
            NormalMapTilt::setMeasureFunction(nullptr);
            NormalMapTilt::clearCache();
        }

        void start(std::function<void()> work) {
            workers.emplace_back(std::move(work));
        }

        void finish() {
            open();
            for (auto& worker : workers)
                worker.join();
            workers.clear();
        }

        void open() {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->gateOpen = true;
            }
            state->cv.notify_all();
        }

        // until a slow measure is waiting at the gate
        bool waitForSlow(std::chrono::milliseconds limit) {
            std::unique_lock<std::mutex> lock(state->mutex);
            return state->cv.wait_for(lock, limit, [this]() { return state->waiting > 0; });
        }

        int calls() {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->started;
        }

    private:
        struct State {
            std::mutex mutex;
            std::condition_variable cv;
            bool gateOpen = false;
            int waiting = 0;
            int running = 0;
            int started = 0;

            bool run(const fs::path& file, float& degrees) {
                std::ifstream in(file);
                std::string word;
                in >> word;
                const bool slow = word == "slow";
                if (slow)
                    in >> word;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    started++;
                    running++;
                    if (slow) {
                        waiting++;
                        cv.notify_all();
                        cv.wait(lock, [this]() { return gateOpen; });
                        waiting--;
                    }
                }
                bool ok = true;
                try {
                    degrees = std::stof(word);
                } catch (...) {
                    ok = false;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    running--;
                }
                cv.notify_all();
                return ok;
            }
        };
        std::shared_ptr<State> state;
        std::vector<std::thread> workers;
    };

    class TempDir {
    public:
        TempDir() {
            std::random_device random;
            path = fs::temp_directory_path() / ("jasper-normalmaptilt-" + std::to_string(random()));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        fs::path path;
    };

    // writes the file, with a modification time apart from the one it had
    void writeFile(const fs::path& file, const std::string& content, int secondsLater = 0) {
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << content;
        }
        if (secondsLater) {
            fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(secondsLater));
        }
    }

    // polls request() as the Inspector does, frame by frame
    NormalMapTiltResult requestUntilReady(const fs::path& file, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
        const auto end = Clock::now() + limit;
        NormalMapTiltResult result = NormalMapTilt::request(file);
        while (!result.ready && Clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            result = NormalMapTilt::request(file);
        }
        return result;
    }

    // a normal map whose every texel is the normal (x, y, z) (unit length), as 8-bit rgb
    void writeNormalMap(const fs::path& file, int width, int height, double x, double y, double z) {
        std::vector<unsigned char> pixels((size_t)width * height * 3);
        const double n[3] = {x, y, z};
        for (size_t i = 0; i < pixels.size(); i += 3)
            for (int c = 0; c < 3; c++)
                pixels[i + c] = (unsigned char)std::lround((n[c] * 0.5 + 0.5) * 255.0);
        ASSERT_NE(stbi_write_png(file.string().c_str(), width, height, 3, pixels.data(), width * 3), 0);
    }

}

// ---- the measure itself

TEST(NormalMapTilt, MeasuresRealImages) {
    TempDir dir;
    const fs::path flat = dir.path / "flat.png";
    const fs::path tilted = dir.path / "tilted.png";
    const fs::path large = dir.path / "large.png";
    const fs::path text = dir.path / "not_an_image.png";
    writeNormalMap(flat, 16, 16, 0.0, 0.0, 1.0);
    const double angle = 9.0 * 3.14159265358979323846 / 180.0;
    writeNormalMap(tilted, 32, 8, std::sin(angle), 0.0, std::cos(angle));
    // sampled on a grid of at most 512 x 512
    writeNormalMap(large, 1500, 700, 0.0, std::sin(angle * 2.0), std::cos(angle * 2.0));
    writeFile(text, "this is not a png");

    float degrees = -1.0f;
    ASSERT_TRUE(NormalMapTilt::measureFile(flat, degrees));
    EXPECT_NEAR(degrees, 0.0f, 0.5f);
    ASSERT_TRUE(NormalMapTilt::measureFile(tilted, degrees));
    EXPECT_NEAR(degrees, 9.0f, 0.5f);
    std::string error;
    EXPECT_FALSE(NormalMapTilt::measureFile(text, degrees, &error));
    EXPECT_NE(error.find("not an image"), std::string::npos) << error;
    EXPECT_FALSE(NormalMapTilt::measureFile(dir.path / "missing.png", degrees, &error));

    // through the cache, both ways in
    NormalMapTilt::clearCache();
    NormalMapTiltResult result = NormalMapTilt::measure(tilted);
    EXPECT_TRUE(result.ready && result.valid);
    EXPECT_NEAR(result.degrees, 9.0f, 0.5f);
    EXPECT_GE(result.degrees, NormalMapTilt::WARNING_DEGREES);
    result = requestUntilReady(large);
    EXPECT_TRUE(result.ready && result.valid);
    EXPECT_NEAR(result.degrees, 18.0f, 0.5f);
    result = requestUntilReady(flat);
    EXPECT_TRUE(result.ready && result.valid);
    EXPECT_LT(result.degrees, NormalMapTilt::WARNING_DEGREES);
    result = NormalMapTilt::measure(text);
    EXPECT_TRUE(result.ready);
    EXPECT_FALSE(result.valid);
    result = NormalMapTilt::request(dir.path / "missing.png");
    EXPECT_TRUE(result.ready);
    EXPECT_FALSE(result.valid);

    // a file replaced by another image is measured again (same path, new contents)
    writeNormalMap(tilted, 32, 9, 0.0, 0.0, 1.0);
    fs::last_write_time(tilted, fs::last_write_time(tilted) + std::chrono::seconds(3));
    result = NormalMapTilt::measure(tilted);
    EXPECT_TRUE(result.valid);
    EXPECT_NEAR(result.degrees, 0.0f, 0.5f);
    NormalMapTilt::clearCache();
}

// ---- the cache

TEST(NormalMapTiltCache, MeasuredOncePerVersion) {
    TempDir dir;
    SlowMeasure slow;
    const fs::path file = dir.path / "a_normal.png";
    writeFile(file, "7");
    EXPECT_NEAR(NormalMapTilt::measure(file).degrees, 7.0f, 1e-4f);
    EXPECT_NEAR(requestUntilReady(file).degrees, 7.0f, 1e-4f);
    EXPECT_NEAR(NormalMapTilt::measure(file).degrees, 7.0f, 1e-4f);
    EXPECT_EQ(slow.calls(), 1);

    // the same modification time but another size is another version
    const fs::file_time_type modified = fs::last_write_time(file);
    writeFile(file, "11.5");
    fs::last_write_time(file, modified);
    EXPECT_NEAR(NormalMapTilt::measure(file).degrees, 11.5f, 1e-4f);
    EXPECT_EQ(slow.calls(), 2);

    // an unreadable value: ready and not valid
    writeFile(file, "x", 2);
    const NormalMapTiltResult bad = requestUntilReady(file);
    EXPECT_TRUE(bad.ready);
    EXPECT_FALSE(bad.valid);
}

TEST(NormalMapTiltCache, ChangedFileDoesNotWaitForTheOldMeasure) {
    TempDir dir;
    SlowMeasure slow;
    const fs::path file = dir.path / "rock_normal.png";
    writeFile(file, "slow 20");

    // the old version is being measured (held at the gate), on a thread of its own: on the
    // Background lane it could hold the lane's only slot (a machine of few cores), and the new
    // version's measure would wait for it
    slow.start([file]() { NormalMapTilt::measure(file); });
    ASSERT_TRUE(slow.waitForSlow(std::chrono::seconds(5)));
    EXPECT_FALSE(NormalMapTilt::request(file).ready);

    // the file changes: the next lookups return at once, and measure the new version
    writeFile(file, "4.25", 2);
    NormalMapTiltResult first;
    const auto start = Clock::now();
    EXPECT_TRUE(returnsWithin(std::chrono::milliseconds(2000), [&]() { first = NormalMapTilt::request(file); }, [&]() { slow.open(); }))
        << "request() of a changed file waited for the measure of its old version";
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(2000));

    const NormalMapTiltResult result = requestUntilReady(file);
    ASSERT_TRUE(result.ready && result.valid);
    EXPECT_NEAR(result.degrees, 4.25f, 1e-4f);

    // the old measure finishing later does not overwrite it
    slow.open();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NEAR(NormalMapTilt::request(file).degrees, 4.25f, 1e-4f);
    EXPECT_NEAR(NormalMapTilt::measure(file).degrees, 4.25f, 1e-4f);
}

TEST(NormalMapTiltCache, MeasureDoesNotHoldTheCacheWhileItDecodes) {
    TempDir dir;
    NormalMapTiltResult measured, second;
    SlowMeasure slow;
    const fs::path big = dir.path / "big_normal.png";
    const fs::path other = dir.path / "other_normal.png";
    writeFile(big, "slow 30");
    writeFile(other, "5");

    // check's measure() decoding one file on its own thread...
    slow.start([&]() { measured = NormalMapTilt::measure(big); });
    ASSERT_TRUE(slow.waitForSlow(std::chrono::seconds(5)));

    // ...while the UI asks for others, and for the same one
    EXPECT_TRUE(returnsWithin(std::chrono::milliseconds(2000), [&]() {
        NormalMapTilt::request(other);
        EXPECT_FALSE(NormalMapTilt::request(big).ready);
    }, [&]() { slow.open(); })) << "request() waited for measure() of another file";
    const NormalMapTiltResult result = requestUntilReady(other);
    EXPECT_TRUE(result.ready && result.valid);
    EXPECT_NEAR(result.degrees, 5.0f, 1e-4f);

    // a second measure of the decoding file waits for that one's result, not another decode
    slow.start([&]() { second = NormalMapTilt::measure(big); });
    slow.finish();
    EXPECT_TRUE(measured.ready && measured.valid);
    EXPECT_NEAR(measured.degrees, 30.0f, 1e-4f);
    EXPECT_NEAR(second.degrees, 30.0f, 1e-4f);
    EXPECT_EQ(slow.calls(), 2);
}

TEST(NormalMapTiltCache, ConcurrentLookupsAgree) {
    TempDir dir;
    SlowMeasure slow;
    slow.open();
    std::vector<fs::path> files;
    for (int i = 0; i < 8; i++) {
        files.push_back(dir.path / ("n" + std::to_string(i) + ".png"));
        writeFile(files.back(), std::to_string(i * 2));
    }
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; t++) {
        threads.emplace_back([&, t]() {
            for (int round = 0; round < 20; round++) {
                for (size_t i = 0; i < files.size(); i++) {
                    const NormalMapTiltResult r = (t + round) % 2 ? NormalMapTilt::measure(files[i]) : requestUntilReady(files[i]);
                    if (!r.ready || !r.valid || std::fabs(r.degrees - (float)(i * 2)) > 1e-4f)
                        wrong++;
                }
            }
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    EXPECT_EQ(wrong.load(), 0);
    // each file measured once
    EXPECT_EQ(slow.calls(), (int)files.size());
}
