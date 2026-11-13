// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ScrollbarHighlight.hpp"

namespace chatterino {

// Views keep a highlight for every message, and most have none, so this
// doesn't allocate a color
ScrollbarHighlight::ScrollbarHighlight()
    : style_(Style::None)
{
}

ScrollbarHighlight::ScrollbarHighlight(const std::shared_ptr<QColor> &color,
                                       Style style)
    : color_(color)
    , style_(style)
{
}

QColor ScrollbarHighlight::getColor() const
{
    if (!this->color_)
    {
        return {};
    }
    return *this->color_;
}

ScrollbarHighlight::Style ScrollbarHighlight::getStyle() const
{
    return this->style_;
}

bool ScrollbarHighlight::isNull() const
{
    return this->style_ == Style::None || !this->color_ ||
           !this->color_->isValid();
}

}  // namespace chatterino
