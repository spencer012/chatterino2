// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QTimer>

#include <cstdint>
#include <memory>

namespace chatterino {

constexpr long unsigned GIF_FRAME_LENGTH = 20;

class GIFTimer
{
public:
    void initialize();

    pajlada::Signals::NoArgSignal signal;
    std::uint64_t position() const
    {
        return *this->position_;
    }

    /// Images read this shared clock only when their current frame is needed.
    /// Keeping the clock alive also makes frames safe after the timer dies.
    std::shared_ptr<const std::uint64_t> clock() const
    {
        return this->position_;
    }

    void advance();

    void registerOpenOverlayWindow()
    {
        this->openOverlayWindows_++;
    }

    void unregisterOpenOverlayWindow()
    {
        assert(this->openOverlayWindows_ >= 1);
        this->openOverlayWindows_--;
    }

private:
    QTimer timer;
    std::shared_ptr<std::uint64_t> position_ =
        std::make_shared<std::uint64_t>(0);
    size_t openOverlayWindows_ = 0;
};

}  // namespace chatterino
