// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/listview/GenericListItem.hpp"

#include <functional>

namespace chatterino {

/// @brief A list item representing a message in the chat history popup
///
/// Displays the message text with optional search term highlighting.
/// Long messages are truncated with an ellipsis.
class MessageHistoryItem : public GenericListItem
{
    using ActionCallback = std::function<void(const QString &)>;

public:
    /// @brief Constructs a message history item
    /// @param message The full message text
    /// @param searchTerm The current search term for highlighting (can be empty)
    /// @param action Callback invoked when the item is selected
    MessageHistoryItem(const QString &message, const QString &searchTerm,
                       ActionCallback action);

    // GenericListItem interface
    void action() override;
    void paint(QPainter *painter, const QRect &rect) const override;
    QSize sizeHint(const QRect &rect) const override;

    /// @brief Updates the search term for highlighting
    void setSearchTerm(const QString &searchTerm);

    /// @brief Gets the full message text
    const QString &getMessage() const;

private:
    QString message_;
    QString searchTerm_;
    ActionCallback action_;

    static constexpr int ITEM_HEIGHT = 20;
    static constexpr int MARGIN = 4;
};

}  // namespace chatterino
