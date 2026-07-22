// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/MessageHistoryItem.hpp"

#include <QFontMetrics>
#include <QPainter>

namespace chatterino {

MessageHistoryItem::MessageHistoryItem(const QString &message,
                                       const QString &searchTerm,
                                       ActionCallback action)
    : message_(message)
    , searchTerm_(searchTerm)
    , action_(std::move(action))
{
}

void MessageHistoryItem::action()
{
    if (this->action_)
    {
        this->action_(this->message_);
    }
}

void MessageHistoryItem::paint(QPainter *painter, const QRect &rect) const
{
    painter->save();

    QRect textRect = rect.adjusted(MARGIN, 0, -MARGIN, 0);
    QFontMetrics fm(painter->font());

    // Elide text if too long
    QString displayText =
        fm.elidedText(this->message_, Qt::ElideRight, textRect.width());

    if (this->searchTerm_.isEmpty())
    {
        // No search term - just draw the text
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          displayText);
    }
    else
    {
        // Draw text with search term highlighted
        int y = textRect.center().y() + fm.ascent() / 2 - fm.descent() / 2;
        int x = textRect.left();

        QString lowerDisplay = displayText.toLower();
        QString lowerSearch = this->searchTerm_.toLower();

        int pos = 0;
        while (pos < displayText.length())
        {
            int matchPos = lowerDisplay.indexOf(lowerSearch, pos);

            if (matchPos == -1)
            {
                // No more matches, draw the rest
                QString remaining = displayText.mid(pos);
                painter->drawText(x, y, remaining);
                break;
            }

            // Draw text before match
            if (matchPos > pos)
            {
                QString before = displayText.mid(pos, matchPos - pos);
                painter->drawText(x, y, before);
                x += fm.horizontalAdvance(before);
            }

            // Draw matched text with highlight
            QString matched =
                displayText.mid(matchPos, this->searchTerm_.length());

            // Draw semi-transparent highlight background
            QColor highlightColor = painter->pen().color();
            highlightColor.setAlpha(60);
            QRect highlightRect(x, textRect.top() + 2,
                                fm.horizontalAdvance(matched),
                                textRect.height() - 4);
            painter->fillRect(highlightRect, highlightColor);

            // Draw matched text (no bolding to avoid width issues)
            painter->drawText(x, y, matched);

            x += fm.horizontalAdvance(matched);
            pos = matchPos + this->searchTerm_.length();
        }
    }

    painter->restore();
}

QSize MessageHistoryItem::sizeHint(const QRect &rect) const
{
    return QSize(rect.width(), ITEM_HEIGHT);
}

void MessageHistoryItem::setSearchTerm(const QString &searchTerm)
{
    this->searchTerm_ = searchTerm;
}

const QString &MessageHistoryItem::getMessage() const
{
    return this->message_;
}

}  // namespace chatterino
