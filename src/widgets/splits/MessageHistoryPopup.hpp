// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BasePopup.hpp"
#include "widgets/listview/GenericListModel.hpp"

#include <pajlada/signals/signal.hpp>

#include <functional>
#include <memory>

namespace chatterino {

class GenericListView;

/// @brief Popup widget for displaying and searching chat message history
///
/// Shows a scrollable list of previously sent messages with optional
/// search filtering and match highlighting. Integrates with terminal-style
/// reverse search (Ctrl+R) in the chat input.
class MessageHistoryPopup : public BasePopup
{
    Q_OBJECT

    using ActionCallback = std::function<void(const QString &)>;

public:
    explicit MessageHistoryPopup(QWidget *parent = nullptr);

    /// @brief Updates the displayed history with optional filtering
    /// @param messages All messages for the channel
    /// @param searchTerm Current search term (empty shows all)
    void updateHistory(const QStringList &messages, const QString &searchTerm);

    /// @brief Sets the callback for when a message is selected
    void setInputAction(ActionCallback callback);

    /// @brief Handles key events for navigation
    bool eventFilter(QObject *watched, QEvent *event) override;

    /// @brief Selects the next matching item (for Ctrl+R cycling)
    void selectNextMatch();

    /// @brief Gets the currently selected message, if any
    QString getSelectedMessage() const;

    /// Signal emitted when search mode should be cancelled
    pajlada::Signals::NoArgSignal cancelled;

    /// Signal emitted when a message is selected
    pajlada::Signals::Signal<QString> messageSelected;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();

    struct {
        GenericListView *listView;
    } ui_{};

    GenericListModel model_;
    ActionCallback callback_;

    /// Number of visible rows in the popup
    static constexpr int VISIBLE_ROWS = 7;
    /// Height of each row
    static constexpr int ROW_HEIGHT = 20;
    /// Window chrome/frame overhead (measured from actual window)
    static constexpr int WINDOW_CHROME_HEIGHT = 22;
};

}  // namespace chatterino
