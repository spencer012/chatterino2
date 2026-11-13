// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/GradualRelease.hpp"

#include "debug/AssertInGuiThread.hpp"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

#include <deque>

namespace {

using namespace chatterino;

// Freeing one message layout takes a few hundred nanoseconds when its message
// isn't cached, so this keeps each turn to a few milliseconds.
constexpr size_t BUDGET_PER_TURN = 8192;

/// Owned by the application. Once it is about to quit, everything is released
/// right away, while images and pixmaps can still be destroyed.
class GradualReleaser : public QObject
{
public:
    explicit GradualReleaser(QCoreApplication *app)
        : QObject(app)
    {
        this->timer_.setSingleShot(true);
        this->timer_.setInterval(0);
        QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
            this->releaseSome();
        });
        QObject::connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            this->quitting_ = true;
            this->timer_.stop();
            this->releaseAll();
        });
    }

    ~GradualReleaser() override
    {
        this->releaseAll();
    }

    void add(std::function<bool(size_t)> release)
    {
        if (this->quitting_)
        {
            releaseNow(release);
            return;
        }
        this->pending_.push_back(std::move(release));
        if (!this->timer_.isActive())
        {
            this->timer_.start();
        }
    }

    static void releaseNow(const std::function<bool(size_t)> &release)
    {
        while (!release(BUDGET_PER_TURN))
        {
        }
    }

private:
    void releaseSome()
    {
        if (!this->pending_.empty() && this->pending_.front()(BUDGET_PER_TURN))
        {
            this->pending_.pop_front();
        }
        if (!this->pending_.empty())
        {
            this->timer_.start();
        }
    }

    void releaseAll()
    {
        while (!this->pending_.empty())
        {
            releaseNow(this->pending_.front());
            this->pending_.pop_front();
        }
    }

    std::deque<std::function<bool(size_t)>> pending_;
    QTimer timer_;
    bool quitting_ = false;
};

}  // namespace

namespace chatterino::detail {

void scheduleGradualRelease(std::function<bool(size_t budget)> releaseSome)
{
    assertInGuiThread();

    auto *app = QCoreApplication::instance();
    if (app == nullptr || QCoreApplication::closingDown())
    {
        // Nothing would run the release later, so do it now
        GradualReleaser::releaseNow(releaseSome);
        return;
    }

    static QPointer<GradualReleaser> releaser;
    if (!releaser)
    {
        releaser = new GradualReleaser(app);
    }
    releaser->add(std::move(releaseSome));
}

}  // namespace chatterino::detail
