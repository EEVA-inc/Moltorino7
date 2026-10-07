#include "util/MemoryReclaimer.hpp"

#include <QCoreApplication>
#include <QThreadPool>
#include <QTimer>

#if defined(Q_OS_LINUX)
#    include <features.h>
#endif

#if defined(Q_OS_WIN) && defined(_MSC_VER)
#    include <malloc.h>
#elif defined(Q_OS_LINUX) && defined(__GLIBC__)
#    include <malloc.h>
#elif defined(Q_OS_MACOS)
#    include <malloc/malloc.h>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

namespace chatterino {

namespace detail {

MemoryReliefCoordinator::MemoryReliefCoordinator(
    Duration coalesceDelay, Duration minimumInterval, Now now,
    ScheduleTimer scheduleTimer, Execute execute, Relief relief)
    : coalesceDelay_(coalesceDelay)
    , minimumInterval_(minimumInterval)
    , now_(std::move(now))
    , scheduleTimer_(std::move(scheduleTimer))
    , execute_(std::move(execute))
    , relief_(std::move(relief))
{
}

void MemoryReliefCoordinator::request()
{
    std::optional<Duration> delay;
    {
        std::lock_guard lock(this->mutex_);
        if (this->running_)
        {
            this->pending_ = true;
            return;
        }
        if (this->scheduled_)
        {
            return;
        }

        const auto now = this->now_();
        auto runAt = now + this->coalesceDelay_;
        if (this->lastCompleted_)
        {
            runAt = std::max(runAt,
                             *this->lastCompleted_ + this->minimumInterval_);
        }
        delay = std::chrono::duration_cast<Duration>(runAt - now);
        this->scheduled_ = true;
    }

    this->scheduleAfter(*delay);
}

void MemoryReliefCoordinator::timerFired()
{
    {
        std::lock_guard lock(this->mutex_);
        if (!this->scheduled_)
        {
            return;
        }
        this->scheduled_ = false;
        if (this->running_)
        {
            this->pending_ = true;
            return;
        }
        this->running_ = true;
    }

    this->execute_([this] {
        this->relief_();
        this->workerFinished();
    });
}

void MemoryReliefCoordinator::workerFinished()
{
    bool schedulePending = false;
    {
        std::lock_guard lock(this->mutex_);
        if (!this->running_)
        {
            return;
        }

        this->running_ = false;
        this->lastCompleted_ = this->now_();
        if (this->pending_)
        {
            this->pending_ = false;
            this->scheduled_ = true;
            schedulePending = true;
        }
    }

    if (schedulePending)
    {
        this->scheduleAfter(this->minimumInterval_);
    }
}

void MemoryReliefCoordinator::scheduleAfter(Duration delay)
{
    this->scheduleTimer_(std::max(Duration::zero(), delay), [this] {
        this->timerFired();
    });
}

}

namespace {

#if (defined(Q_OS_WIN) && defined(_MSC_VER)) || \
    (defined(Q_OS_LINUX) && defined(__GLIBC__)) || defined(Q_OS_MACOS)

constexpr auto COALESCE_DELAY = std::chrono::seconds{5};
constexpr auto MINIMUM_INTERVAL = std::chrono::seconds{30};

void releaseFreeAllocatorPages()
{
#    if defined(Q_OS_WIN) && defined(_MSC_VER)
    _heapmin();
#    elif defined(Q_OS_LINUX) && defined(__GLIBC__)
    malloc_trim(0);
#    elif defined(Q_OS_MACOS)
    malloc_zone_pressure_relief(nullptr, 0);
#    endif
}

detail::MemoryReliefCoordinator &memoryReliefCoordinator()
{
    static detail::MemoryReliefCoordinator coordinator{
        std::chrono::duration_cast<detail::MemoryReliefCoordinator::Duration>(
            COALESCE_DELAY),
        std::chrono::duration_cast<detail::MemoryReliefCoordinator::Duration>(
            MINIMUM_INTERVAL),
        [] {
            return std::chrono::steady_clock::now();
        },
        [](auto delay, auto task) {
            auto *application = QCoreApplication::instance();
            if (application == nullptr || QCoreApplication::closingDown())
            {
                return;
            }
            QTimer::singleShot(delay, application, std::move(task));
        },
        [](auto task) {
            QThreadPool::globalInstance()->start(std::move(task));
        },
        [] {
            releaseFreeAllocatorPages();
        }};
    return coordinator;
}

#endif

}

void requestMemoryPressureRelief()
{
#if (defined(Q_OS_LINUX) && defined(__GLIBC__)) || defined(Q_OS_MACOS)
    auto *application = QCoreApplication::instance();
    if (application == nullptr || QCoreApplication::closingDown())
    {
        return;
    }
    memoryReliefCoordinator().request();
#endif
}

void requestTransientMemoryPressureRelief()
{
#if (defined(Q_OS_WIN) && defined(_MSC_VER)) || \
    (defined(Q_OS_LINUX) && defined(__GLIBC__)) || defined(Q_OS_MACOS)
    auto *application = QCoreApplication::instance();
    if (application == nullptr || QCoreApplication::closingDown())
    {
        return;
    }
    memoryReliefCoordinator().request();
#endif
}

}
