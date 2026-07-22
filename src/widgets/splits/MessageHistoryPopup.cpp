// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/MessageHistoryPopup.hpp"

#include "common/QLogging.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/listview/GenericListView.hpp"
#include "widgets/splits/MessageHistoryItem.hpp"

#include <QKeyEvent>

namespace chatterino {

MessageHistoryPopup::MessageHistoryPopup(QWidget *parent)
    : BasePopup({BasePopup::EnableCustomFrame, BasePopup::Frameless,
                 BasePopup::DontFocus, BaseWindow::DisableLayoutSave},
                parent)
    , model_(this)
{
    this->initLayout();
    this->themeChangedEvent();
}

void MessageHistoryPopup::updateHistory(const QStringList &messages,
                                        const QString &searchTerm)
{
    this->model_.clear();

    int messageCount = static_cast<int>(messages.size());
    qCDebug(chatterinoWidget) << "MessageHistoryPopup::updateHistory -"
                              << messageCount << "messages, searchTerm:"
                              << searchTerm;

    // Add items - most recent last
    for (const auto &msg : messages)
    {
        this->model_.addItem(std::make_unique<MessageHistoryItem>(
            msg, searchTerm, this->callback_));
    }

    // Resize popup: use VISIBLE_ROWS unless we have fewer messages
    // Only shrink when messages < VISIBLE_ROWS
    int visibleRows = std::min(std::max(messageCount, 1), VISIBLE_ROWS);
    
    // Calculate heights - there's window chrome overhead
    int listHeight = visibleRows * ROW_HEIGHT;
    int windowHeight = listHeight + WINDOW_CHROME_HEIGHT;
    
    // Set both list view and window height
    this->ui_.listView->setFixedHeight(listHeight);
    this->setFixedHeight(windowHeight);
    
    qCDebug(chatterinoWidget) << "  visibleRows:" << visibleRows
                              << "listHeight:" << listHeight
                              << "windowHeight:" << windowHeight;

    // Select the last (most recent) item and ensure it's visible
    if (messageCount > 0)
    {
        auto lastIndex = this->model_.index(messageCount - 1);
        this->ui_.listView->setCurrentIndex(lastIndex);
        this->ui_.listView->scrollTo(lastIndex,
                                     QAbstractItemView::PositionAtBottom);
    }
}

void MessageHistoryPopup::setInputAction(ActionCallback callback)
{
    this->callback_ = std::move(callback);
}

bool MessageHistoryPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        int key = keyEvent->key();

        // Handle Escape specially to emit cancelled signal
        if (key == Qt::Key_Escape)
        {
            this->cancelled.invoke();
            this->close();
            return true;
        }

        // Let the list view handle other navigation keys
        return this->ui_.listView->eventFilter(watched, event);
    }

    return BasePopup::eventFilter(watched, event);
}

void MessageHistoryPopup::selectNextMatch()
{
    if (this->model_.rowCount() <= 0)
    {
        return;
    }

    const QModelIndex &curIdx = this->ui_.listView->currentIndex();
    int curRow = curIdx.row();
    int count = this->model_.rowCount();

    // Go to previous item (up in the list, which is older)
    int newRow = curRow - 1;
    if (newRow < 0)
    {
        newRow = count - 1;  // Wrap around
    }

    this->ui_.listView->setCurrentIndex(curIdx.siblingAtRow(newRow));
}

QString MessageHistoryPopup::getSelectedMessage() const
{
    const QModelIndex &idx = this->ui_.listView->currentIndex();
    if (!idx.isValid())
    {
        return {};
    }

    auto *item = GenericListItem::fromVariant(idx.data());
    if (auto *historyItem = dynamic_cast<MessageHistoryItem *>(item))
    {
        return historyItem->getMessage();
    }
    return {};
}

void MessageHistoryPopup::showEvent(QShowEvent * /*event*/)
{
    // Ensure the last item is visible when showing
    if (this->model_.rowCount() > 0)
    {
        auto lastIndex = this->model_.index(this->model_.rowCount() - 1);
        this->ui_.listView->scrollTo(lastIndex,
                                     QAbstractItemView::PositionAtBottom);
    }
}

void MessageHistoryPopup::hideEvent(QHideEvent * /*event*/)
{
}

void MessageHistoryPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();
    this->ui_.listView->refreshTheme(*getTheme());
}

void MessageHistoryPopup::initLayout()
{
    LayoutCreator creator = {this};

    auto listView =
        creator.emplace<GenericListView>().assign(&this->ui_.listView);
    listView->setInvokeActionOnTab(false);
    listView->setModel(&this->model_);
    listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Set fixed width, height will be adjusted by updateHistory
    this->setFixedWidth(400);
    // Set initial list view height
    this->ui_.listView->setFixedHeight(VISIBLE_ROWS * ROW_HEIGHT);

    QObject::connect(listView.getElement(), &GenericListView::closeRequested,
                     this, [this] {
                         // Get the selected message before closing
                         QString selected = this->getSelectedMessage();
                         if (!selected.isEmpty())
                         {
                             this->messageSelected.invoke(selected);
                         }
                         this->close();
                     });
}

}  // namespace chatterino
