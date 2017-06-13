#pragma once

#include <mbgl/actor/actor.hpp>
#include <mbgl/actor/mailbox.hpp>
#include <mbgl/actor/scheduler.hpp>
#include <mbgl/util/platform.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/util.hpp>

#include <cassert>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

namespace mbgl {
namespace util {

template<class Object>
class ThreadedObject : public Scheduler {
public:
    template <class... Args>
    ThreadedObject(const std::string& name, Args&&... args) {
        std::promise<void> running;

        thread = std::thread([&] {
            platform::setCurrentThreadName(name);
            platform::makeThreadLowPriority();

            util::RunLoop loop_(util::RunLoop::Type::New);
            loop = &loop_;

            object = std::make_unique<Actor<Object>>(*this, std::forward<Args>(args)...);
            running.set_value();

            loop->run();
            loop = nullptr;
        });

        running.get_future().get();
    }

    ~ThreadedObject() override {
        MBGL_VERIFY_THREAD(tid);

        if (paused) {
            resume();
        }

        std::promise<void> joinable;

        // Kill the actor, so we don't get more
        // messages posted on this scheduler after
        // we delete the RunLoop.
        loop->invoke([&] {
            object.reset();
            joinable.set_value();
        });

        joinable.get_future().get();

        loop->stop();
        thread.join();
    }

    ActorRef<std::decay_t<Object>> actor() const {
        return object->self();
    }

    void pause() {
        MBGL_VERIFY_THREAD(tid);

        assert(!paused);

        paused = std::make_unique<std::promise<void>>();
        resumed = std::make_unique<std::promise<void>>();

        auto pausing = paused->get_future();

        loop->invoke([this] {
            auto resuming = resumed->get_future();
            paused->set_value();
            resuming.get();
        });

        pausing.get();
    }

    void resume() {
        MBGL_VERIFY_THREAD(tid);

        assert(paused);

        resumed->set_value();

        resumed.reset();
        paused.reset();
    }

private:
    MBGL_STORE_THREAD(tid);

    void schedule(std::weak_ptr<Mailbox> mailbox) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(mailbox);
        }

        loop->invoke([this] { receive(); });
    }

    void receive() {
        std::unique_lock<std::mutex> lock(mutex);

        auto mailbox = queue.front();
        queue.pop();
        lock.unlock();

        Mailbox::maybeReceive(mailbox);
    }

    std::mutex mutex;
    std::queue<std::weak_ptr<Mailbox>> queue;
    std::thread thread;
    std::unique_ptr<Actor<Object>> object;

    std::unique_ptr<std::promise<void>> paused;
    std::unique_ptr<std::promise<void>> resumed;

    util::RunLoop* loop = nullptr;
};

} // namespace util
} // namespace mbgl
