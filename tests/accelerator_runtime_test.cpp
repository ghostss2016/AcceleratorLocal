// Native regression for production logic. Compile/run only in central cs2-ci
// on .100; hook registrations and signal I/O are the substituted boundaries.
#include "../accelerator_runtime.h"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct HookBoundary {
    std::vector<int> installed;
    std::vector<int> removed;
    unsigned attempts = 0;
    unsigned destroyed = 0;
};

class BoundaryHook {
    HookBoundary& boundary_;
    int id_;
    bool reject_;
    bool active_ = false;
public:
    BoundaryHook(HookBoundary& boundary, int id, bool reject = false)
        : boundary_(boundary), id_(id), reject_(reject) {}
    bool AddInstance(int* instance) {
        ++boundary_.attempts;
        if (!instance || reject_) return false;
        active_ = true;
        boundary_.installed.push_back(id_);
        return true;
    }
    ~BoundaryHook() {
        ++boundary_.destroyed;
        if (active_) boundary_.removed.push_back(id_);
    }
};

using Hooks = accelerator::OwnedHooks<BoundaryHook, BoundaryHook>;

bool Install(Hooks& hooks, HookBoundary& boundary, int* server, int* service,
             bool rejectFrame = false, bool rejectStartup = false) {
    return hooks.Install(std::make_unique<BoundaryHook>(boundary, 1, rejectFrame),
                         std::make_unique<BoundaryHook>(boundary, 2, rejectStartup),
                         server, service);
}

void HooksAndCallbacks() {
    accelerator::CallbackActivity callbacks;
    assert(!callbacks.Busy());
    int server = 1, service = 2;
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        Hooks hooks;
        HookBoundary boundary;
        assert(!Install(hooks, boundary, &server, &service, true));
        assert(hooks.Empty() && boundary.removed.empty());
        assert(boundary.attempts == 1 && boundary.destroyed == 2);

        assert(!Install(hooks, boundary, &server, &service, false, true));
        assert(hooks.Empty());
        assert((boundary.removed == std::vector<int>{1}));
        assert(boundary.attempts == 3 && boundary.destroyed == 4);

        assert(!Install(hooks, boundary, nullptr, &service));
        assert(hooks.Empty() && boundary.attempts == 4);
        assert(!Install(hooks, boundary, &server, nullptr));
        assert(hooks.Empty() && boundary.attempts == 6);
        assert((boundary.removed == std::vector<int>{1, 1}));

        assert(Install(hooks, boundary, &server, &service));
        assert(hooks.Ready() && !hooks.Empty());
        assert(!Install(hooks, boundary, &server, &service));
        assert(hooks.Ready()); // Duplicate initialization preserves old hooks.
        assert(boundary.attempts == 8);

        const auto removals = boundary.removed.size();
        assert(!hooks.Clear(false, callbacks));
        assert(hooks.Ready() && boundary.removed.size() == removals);
        {
            accelerator::CallbackActivity::Scope outer(callbacks);
            assert(callbacks.Busy());
            assert(!hooks.Clear(true, callbacks));
            {
                accelerator::CallbackActivity::Scope inner(callbacks);
                assert(!hooks.Clear(true, callbacks));
            }
            assert(callbacks.Busy() && hooks.Ready());
            assert(boundary.removed.size() == removals);
        }
        assert(!callbacks.Busy());
        assert(hooks.Clear(true, callbacks));
        assert(hooks.Empty());
        assert((boundary.removed == std::vector<int>{1, 1, 2, 1}));
        assert(hooks.Clear(false, callbacks)); // Empty cleanup is idempotent.
        assert(boundary.removed.size() == removals + 2);

        assert(Install(hooks, boundary, &server, &service));
        hooks.RollbackInitialization(); // A later Breakpad setup failure.
        assert(hooks.Empty());
        assert((boundary.removed == std::vector<int>{1, 1, 2, 1, 2, 1}));
    }
    // Crash callback bookkeeping is also observed from a different thread.
    std::atomic<bool> started{false}, finish{false};
    std::thread worker([&] {
        accelerator::CallbackActivity::Scope scope(callbacks);
        started.store(true);
        while (!finish.load()) std::this_thread::yield();
    });
    while (!started.load()) std::this_thread::yield();
    assert(callbacks.Busy());
    Hooks empty;
    assert(!empty.Clear(true, callbacks));
    finish.store(true);
    worker.join();
    assert(!callbacks.Busy() && empty.Clear(false, callbacks));
}

void Metadata() {
    accelerator::CrashMetadata metadata;
    static_assert(sizeof(metadata.map) == 256);
    static_assert(sizeof(metadata.gamePath) == 512);
    static_assert(sizeof(metadata.commandLine) == 1024);
    static_assert(sizeof(metadata.dumpPath) == 512);
    const std::string longName(4096, 'x');
    metadata.Map(longName.c_str());
    assert(std::strlen(metadata.map) == 255);
    metadata.Map("de_dust2");
    assert(std::string(metadata.map) == "de_dust2");
    metadata.Map(nullptr);
    assert(metadata.map[0] == '\0');
    metadata.Map("");
    assert(metadata.map[0] == '\0');
    accelerator::CopyMetadata(metadata.gamePath, longName.c_str());
    accelerator::CopyMetadata(metadata.commandLine, longName.c_str());
    assert(std::strlen(metadata.gamePath) == 511);
    assert(std::strlen(metadata.commandLine) == 1023);
    char one[1]{'x'};
    accelerator::CopyMetadata(one, longName.c_str());
    assert(one[0] == '\0');
    metadata.Reset();
    assert(metadata.map[0] == '\0' && metadata.gamePath[0] == '\0' &&
           metadata.commandLine[0] == '\0' && metadata.dumpPath[0] == '\0');
}

void ProcessStateOutput() {
    const int state = 41;
    int resolver = 7;
    unsigned calls = 0;
    for (bool contents : {false, true}) {
        for (bool requestingOnly : {false, true}) {
            accelerator::PrintOriginalProcessState(state, contents, requestingOnly, &resolver,
                [&](const int& actualState, bool stackContents, bool dumpStackPointers,
                    bool only, int threadIndex, int* actualResolver) {
                    ++calls;
                    assert(&actualState == &state && actualResolver == &resolver);
                    assert(stackContents == contents && only == requestingOnly);
                    assert(!dumpStackPointers && threadIndex == -1);
                });
        }
    }
    assert(calls == 4);
}

void OwnedSignal(int, siginfo_t*, void*) {}
void ForeignSignal(int, siginfo_t*, void*) {}

struct SignalBoundary {
    const std::array<int, 5> signals{{SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}};
    std::array<struct sigaction, 5> current{};
    unsigned reads = 0, writes = 0;
    int failRead = -1, failWrite = -1;

    SignalBoundary() {
        for (auto& action : current) {
            action.sa_sigaction = OwnedSignal;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&action.sa_mask);
            for (int signal : signals) sigaddset(&action.sa_mask, signal);
        }
    }
    std::size_t Index(int signal) const {
        for (std::size_t i = 0; i < signals.size(); ++i)
            if (signals[i] == signal) return i;
        assert(false);
        return 0;
    }
    bool Read(int signal, struct sigaction& action) {
        ++reads;
        if (signal == failRead) return false;
        action = current[Index(signal)];
        return true;
    }
    bool Write(int signal, const struct sigaction& action) {
        ++writes;
        if (signal == failWrite) return false;
        current[Index(signal)] = action;
        return true;
    }
};

void Signals() {
    accelerator::SignalMonitor monitor;
    SignalBoundary boundary;
    const auto read = [&](int signal, struct sigaction& action) { return boundary.Read(signal, action); };
    const auto write = [&](int signal, const struct sigaction& action) { return boundary.Write(signal, action); };
    assert(!monitor.Ready());
    assert(!monitor.Repair(read, write));
    assert(boundary.reads == 0 && boundary.writes == 0);

    assert(!monitor.Capture(read) && !monitor.Ready());
    boundary.failRead = SIGFPE;
    assert(!monitor.Prepare(read) && !monitor.Ready());
    boundary.failRead = -1;
    for (auto& action : boundary.current) action.sa_sigaction = ForeignSignal;
    assert(monitor.Prepare(read));
    assert(!monitor.Capture(read) && !monitor.Ready()); // No actual installation.
    for (auto& action : boundary.current) action.sa_sigaction = OwnedSignal;
    boundary.failRead = SIGFPE;
    assert(!monitor.Capture(read) && !monitor.Ready());
    boundary.failRead = -1;
    boundary.current[4].sa_sigaction = ForeignSignal;
    assert(!monitor.Capture(read) && !monitor.Ready());
    boundary.current[4].sa_sigaction = OwnedSignal;
    boundary.current[0].sa_flags &= ~SA_SIGINFO;
    assert(!monitor.Capture(read) && !monitor.Ready());
    boundary.current[0].sa_flags |= SA_SIGINFO;
    assert(monitor.Capture(read) && monitor.Ready());

    boundary.reads = 0;
    for (unsigned frame = 0; frame < 10000; ++frame) assert(monitor.Repair(read, write));
    assert(boundary.reads == 50000 && boundary.writes == 0);
    boundary.current[1].sa_sigaction = ForeignSignal;
    assert(monitor.Repair(read, write));
    assert(boundary.writes == 5);
    for (const auto& action : boundary.current) {
        assert(action.sa_sigaction == OwnedSignal);
        assert((action.sa_flags & (SA_SIGINFO | SA_ONSTACK)) == (SA_SIGINFO | SA_ONSTACK));
        for (int signal : boundary.signals) assert(sigismember(&action.sa_mask, signal) == 1);
    }
    boundary.current[2].sa_flags &= ~SA_SIGINFO;
    assert(monitor.Repair(read, write));
    assert(boundary.writes == 10); // Same pointer with wrong flags is repaired.

    boundary.current[0].sa_sigaction = ForeignSignal;
    boundary.failRead = SIGBUS;
    assert(!monitor.Repair(read, write));
    assert(boundary.writes == 10); // Failed reads never use uninitialized actions.
    boundary.failRead = -1;
    boundary.failWrite = SIGILL;
    boundary.current[3].sa_sigaction = ForeignSignal;
    assert(!monitor.Repair(read, write));
    assert(boundary.writes == 15);
    assert(boundary.current[3].sa_sigaction == ForeignSignal);
    boundary.failWrite = -1;
    assert(monitor.Repair(read, write));
    assert(boundary.writes == 20 && boundary.current[3].sa_sigaction == OwnedSignal);

    // An unsuccessful recapture retains the last complete installed snapshot.
    boundary.current[4].sa_sigaction = ForeignSignal;
    assert(!monitor.Capture(read) && monitor.Ready());
    assert(monitor.Repair(read, write));
    assert(boundary.current[4].sa_sigaction == OwnedSignal);
    monitor.Reset();
    const auto writes = boundary.writes;
    assert(!monitor.Ready() && !monitor.Repair(read, write));
    assert(boundary.writes == writes);
}

} // namespace

int main() {
    HooksAndCallbacks();
    Metadata();
    ProcessStateOutput();
    Signals();
    std::cout << "accelerator_runtime_test: all checks passed\n";
}
