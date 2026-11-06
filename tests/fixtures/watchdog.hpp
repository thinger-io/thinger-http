#ifndef THINGER_HTTP_TEST_WATCHDOG_HPP
#define THINGER_HTTP_TEST_WATCHDOG_HPP

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace thinger::http::test {

    // Ends the test program if the test is still running after `limit`: a deadlock fails the test
    // instead of hanging it
    class watchdog {
    public:
        explicit watchdog(std::chrono::seconds limit) {
            thread_ = std::thread([this, limit] {
                std::unique_lock lock(mutex_);
                if (!done_.wait_for(lock, limit, [this] { return disarmed_; })) {
                    std::fprintf(stderr, "watchdog: the test is still running after %llds, aborting\n",
                                 static_cast<long long>(limit.count()));
                    std::abort();
                }
            });
        }

        ~watchdog() {
            {
                std::lock_guard lock(mutex_);
                disarmed_ = true;
            }
            done_.notify_all();
            thread_.join();
        }

        watchdog(const watchdog&) = delete;
        watchdog& operator=(const watchdog&) = delete;

    private:
        std::mutex mutex_;
        std::condition_variable done_;
        bool disarmed_ = false;
        std::thread thread_;
    };

    // How long `action` took
    template<typename Action>
    std::chrono::milliseconds timed(Action&& action) {
        auto begin = std::chrono::steady_clock::now();
        action();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    }

}

#endif // THINGER_HTTP_TEST_WATCHDOG_HPP
