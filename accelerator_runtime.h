/**
 * AcceleratorLocal runtime ownership and crash metadata.
 * Changes for the SVAROG fork; the original plugin remains by Phoenix and
 * Asher Baker. Distributed under the original GNU GPL version 3.0.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <signal.h>
#include <utility>

namespace accelerator {

template<std::size_t Size>
void CopyMetadata(char (&destination)[Size], const char* source) {
    static_assert(Size > 0, "metadata needs a terminator");
    if (!source) {
        destination[0] = '\0';
        return;
    }
    const auto length = strnlen(source, Size - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

struct CrashMetadata {
    char map[256]{};
    char gamePath[512]{};
    char commandLine[1024]{};
    char dumpPath[512]{};

    void Reset() { *this = {}; }
    void Map(const char* name) { CopyMetadata(map, name); }
};

// The pinned Breakpad adds optional pointer-dump and single-thread arguments.
// Keep the original full process-state output and invoke the printer once.
template<class State, class Resolver, class Print>
void PrintOriginalProcessState(const State& state, bool stackContents,
                              bool requestingThreadOnly, Resolver* resolver,
                              Print print) {
    print(state, stackContents, false, requestingThreadOnly, -1, resolver);
}

// A signal callback must never fall back to a locking libatomic operation.
static_assert(std::atomic<unsigned>::is_always_lock_free,
              "crash callback accounting requires lock-free atomics");

class CallbackActivity {
    std::atomic<unsigned> active_{0};
public:
    class Scope {
        CallbackActivity& owner_;
    public:
        explicit Scope(CallbackActivity& owner) : owner_(owner) {
            owner_.active_.fetch_add(1);
        }
        ~Scope() { owner_.active_.fetch_sub(1); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
    bool Busy() const { return active_.load() != 0; }
};

// KHook owns the synchronous detour removal. This pair owns our registrations
// and rolls the first one back if the second is rejected. The template permits
// the native regression to replace only the hook registration/I/O boundary.
template<class FrameHook, class StartupHook>
class OwnedHooks {
    std::unique_ptr<FrameHook> frame_;
    std::unique_ptr<StartupHook> startup_;
public:
    bool Empty() const { return !frame_ && !startup_; }
    bool Ready() const { return frame_ && startup_; }

    template<class Server, class Service>
    bool Install(std::unique_ptr<FrameHook> frame,
                 std::unique_ptr<StartupHook> startup,
                 Server* server, Service* service) {
        if (!Empty()) return false;
        frame_ = std::move(frame);
        if (!frame_ || !frame_->AddInstance(server)) {
            frame_.reset();
            return false;
        }
        startup_ = std::move(startup);
        if (!startup_ || !startup_->AddInstance(service)) {
            startup_.reset();
            frame_.reset();
            return false;
        }
        return true;
    }

    bool Clear(bool providerAvailable, const CallbackActivity& callbacks) {
        // Reject before changing any resource: a reentrant unload must leave
        // the active callback's context and the complete pair alive.
        if (callbacks.Busy() || (!Empty() && !providerAvailable)) return false;
        RollbackInitialization();
        return true;
    }

    // Only the non-reentrant Load transaction uses this route. Removal is
    // synchronous in KHook, including a hook accepted before a later failure.
    void RollbackInitialization() {
        startup_.reset();
        frame_.reset();
    }
};

// Preserve the original five-signal monitoring contract. Breakpad owns all
// signal installation/restoration (including any other signals in its pin).
// Save full installed actions rather than reconstructing masks/flags every tick.
class SignalMonitor {
    static constexpr std::array<int, 5> signals_{{SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}};
    std::array<struct sigaction, signals_.size()> before_{};
    std::array<struct sigaction, signals_.size()> installed_{};
    bool prepared_ = false;
    bool ready_ = false;
public:
    bool Ready() const { return ready_; }
    void Reset() { prepared_ = false; ready_ = false; before_ = {}; installed_ = {}; }

    template<class Read>
    bool Prepare(Read read) {
        std::array<struct sigaction, signals_.size()> candidate{};
        for (std::size_t i = 0; i < signals_.size(); ++i)
            if (!read(signals_[i], candidate[i])) return false;
        before_ = candidate;
        prepared_ = true;
        return true;
    }

    template<class Read>
    bool Capture(Read read) {
        if (!prepared_) return false;
        std::array<struct sigaction, signals_.size()> candidate{};
        for (std::size_t i = 0; i < signals_.size(); ++i) {
            if (!read(signals_[i], candidate[i]) ||
                !(candidate[i].sa_flags & SA_SIGINFO) ||
                !candidate[i].sa_sigaction ||
                candidate[i].sa_sigaction == before_[i].sa_sigaction ||
                candidate[i].sa_sigaction != candidate[0].sa_sigaction) {
                return false;
            }
        }
        installed_ = candidate;
        ready_ = true;
        return true;
    }

    template<class Read, class Write>
    bool Repair(Read read, Write write) const {
        if (!ready_) return false;
        bool changed = false;
        for (std::size_t i = 0; i < signals_.size(); ++i) {
            struct sigaction current{};
            if (!read(signals_[i], current)) return false;
            if (current.sa_sigaction != installed_[i].sa_sigaction ||
                (current.sa_flags & (SA_SIGINFO | SA_ONSTACK)) !=
                    (installed_[i].sa_flags & (SA_SIGINFO | SA_ONSTACK))) {
                changed = true;
            }
        }
        if (!changed) return true;
        bool success = true;
        for (std::size_t i = 0; i < signals_.size(); ++i) {
            if (!write(signals_[i], installed_[i])) success = false;
        }
        return success;
    }
};

} // namespace accelerator
