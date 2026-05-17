//
// Generic CLOCK / second-chance cache with a background "aging" thread.
// Public API matches the instructor's skeleton: getKey / putKey.
//
//   ClockSweep<T>(maxCacheSize, sweepInterval)
//     T   getKey(const T& key)   -> returns cached key on hit (sets ref bit),
//                                   default-constructed T on miss.
//     void putKey(const T& key)  -> inserts/refreshes; evicts via clock sweep
//                                   when the cache is full.
//
// Designed as a building block for a database storage-buffer manager: once a
// real Page type exists, instantiate ClockSweep<Page> and the same eviction
// logic carries over.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

template <typename T>
class ClockSweep {
public:
    explicit ClockSweep(std::size_t maxCacheSize,
                        std::chrono::milliseconds sweepInterval =
                            std::chrono::milliseconds(500))
        : maxCacheSize_(maxCacheSize),
          sweepInterval_(sweepInterval),
          slots_(maxCacheSize),
          hand_(0),
          stop_(false) {
        if (maxCacheSize == 0) {
            throw std::invalid_argument("ClockSweep capacity must be > 0");
        }
        bgClockThread_ = std::thread(&ClockSweep::sweepLoop, this);
    }

    ~ClockSweep() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (bgClockThread_.joinable()) {
            bgClockThread_.join();
        }
    }

    ClockSweep(const ClockSweep&) = delete;
    ClockSweep& operator=(const ClockSweep&) = delete;

    T getKey(const T& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = keyIndex_.find(key);
        if (it == keyIndex_.end()) {
            return T{};
        }
        slots_[it->second].refBit = true;
        return slots_[it->second].key;
    }

    void putKey(const T& key) {
        std::lock_guard<std::mutex> lk(mu_);

        auto it = keyIndex_.find(key);
        if (it != keyIndex_.end()) {
            slots_[it->second].refBit = true;
            return;
        }

        for (std::size_t scanned = 0; scanned < slots_.size(); ++scanned) {
            if (!slots_[hand_].occupied) {
                slots_[hand_] = Slot{key, true, true};
                keyIndex_[key] = hand_;
                hand_ = (hand_ + 1) % slots_.size();
                return;
            }
            hand_ = (hand_ + 1) % slots_.size();
        }

        std::size_t victim = findVictimLocked();
        keyIndex_.erase(slots_[victim].key);
        slots_[victim] = Slot{key, true, true};
        keyIndex_[key] = victim;
        hand_ = (victim + 1) % slots_.size();
    }

    bool contains(const T& key) {
        std::lock_guard<std::mutex> lk(mu_);
        return keyIndex_.find(key) != keyIndex_.end();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lk(mu_);
        return keyIndex_.size();
    }

    std::size_t capacity() const { return maxCacheSize_; }

    void debugPrint(const std::string& label) {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << label << "] hand=" << hand_ << " | ";
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            std::cout << "slot" << i << "=";
            if (slots_[i].occupied) {
                std::cout << slots_[i].key
                          << "(ref=" << (slots_[i].refBit ? '1' : '0') << ")";
            } else {
                std::cout << "_";
            }
            if (i + 1 < slots_.size()) std::cout << "  ";
        }
        std::cout << "\n";
    }

private:
    struct Slot {
        T key{};
        bool refBit{false};
        bool occupied{false};
    };

    std::size_t maxCacheSize_;
    std::chrono::milliseconds sweepInterval_;
    std::vector<Slot> slots_;
    std::unordered_map<T, std::size_t> keyIndex_;
    std::size_t hand_;

    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_;
    std::thread bgClockThread_;

    std::size_t findVictimLocked() {
        for (std::size_t passes = 0; passes < 2 * slots_.size(); ++passes) {
            Slot& s = slots_[hand_];
            if (s.occupied && !s.refBit) {
                return hand_;
            }
            if (s.occupied && s.refBit) {
                s.refBit = false;
            }
            hand_ = (hand_ + 1) % slots_.size();
        }
        return hand_;
    }

    void sweepLoop() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!stop_) {
            if (cv_.wait_for(lk, sweepInterval_, [this] { return stop_; })) {
                break;
            }
            for (auto& s : slots_) {
                if (s.occupied && s.refBit) {
                    s.refBit = false;
                }
            }
        }
    }
};

namespace {

void demoIntCache() {
    std::cout << "=== Demo 1: ClockSweep<int>, capacity=4, sweep=300ms ===\n";
    ClockSweep<int> cache(4, std::chrono::milliseconds(300));

    cache.putKey(1); cache.debugPrint("put 1");
    cache.putKey(2); cache.debugPrint("put 2");
    cache.putKey(3); cache.debugPrint("put 3");
    cache.putKey(4); cache.debugPrint("put 4 (cache full)");

    std::cout << "\n-- sleep 400ms so the background sweep ages all ref bits to 0 --\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    cache.debugPrint("after bg sweep");

    cache.getKey(2); cache.debugPrint("get 2 (hit, refBit -> 1)");
    cache.getKey(4); cache.debugPrint("get 4 (hit, refBit -> 1)");

    std::cout << "\n-- inserting 5: clock should evict the first slot with refBit=0 --\n";
    cache.putKey(5); cache.debugPrint("put 5");

    std::cout << "\n-- inserting 6: another eviction, victim chosen by clock sweep --\n";
    cache.putKey(6); cache.debugPrint("put 6");

    std::cout << "\ncontains(2)=" << std::boolalpha << cache.contains(2)
              << "  contains(1)=" << cache.contains(1)
              << "  contains(5)=" << cache.contains(5) << "\n";
    std::cout << "size=" << cache.size() << " / capacity=" << cache.capacity() << "\n\n";
}

void demoStringCache() {
    std::cout << "=== Demo 2: ClockSweep<std::string>, capacity=3 ===\n";
    ClockSweep<std::string> sc(3, std::chrono::milliseconds(500));

    sc.putKey("alice");
    sc.putKey("bob");
    sc.putKey("carol");
    sc.debugPrint("filled");

    sc.getKey("alice");
    sc.debugPrint("get alice (refBit -> 1)");

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    sc.debugPrint("after bg sweep");

    sc.getKey("alice");
    sc.debugPrint("get alice again (only alice has ref=1)");

    sc.putKey("dave");
    sc.debugPrint("put dave (evicts bob — first refBit=0 slot)");

    sc.putKey("erin");
    sc.debugPrint("put erin");

    std::cout << "contains(alice)=" << sc.contains("alice")
              << "  contains(bob)=" << sc.contains("bob")
              << "  contains(carol)=" << sc.contains("carol") << "\n\n";
}

void demoUpdateRefreshesRefBit() {
    std::cout << "=== Demo 3: putKey on an existing key just refreshes refBit ===\n";
    ClockSweep<int> c(3, std::chrono::milliseconds(1000));
    c.putKey(10); c.putKey(20); c.putKey(30);
    c.debugPrint("filled");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.debugPrint("after bg sweep (all aged)");
    c.putKey(20);
    c.debugPrint("put 20 again (existing key, refBit -> 1)");
    std::cout << "size=" << c.size() << " (still 3, no growth)\n\n";
}

}  // namespace

int main() {
    demoIntCache();
    demoStringCache();
    demoUpdateRefreshesRefBit();
    std::cout << "All demos finished.\n";
    return 0;
}