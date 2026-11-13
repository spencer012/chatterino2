// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/helper/ChannelView.hpp"

#include <QTimer>

#include <atomic>
#include <memory>
#include <vector>

class QLineEdit;

namespace chatterino {

class Split;
class MessagePredicate;

class SearchPopup : public BasePopup
{
public:
    SearchPopup(QWidget *parent, Split *split = nullptr);
    ~SearchPopup() override;

    virtual void addChannel(ChannelView &channel);
    void goToMessage(const MessagePtr &message);
    /**
     * This method should only be used for searches that
     * don't include a mentions channel,
     * since it will only search in the opened channels (not globally).
     * @param messageId
     */
    void goToMessageId(const QString &messageId);

protected:
    virtual void updateWindowTitle();
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();
    void search();
    void showResults(std::vector<MessagePtr> messages,
                     ChannelView::PreparedMessages prepared);
    void addShortcuts() override;

    /// The messages to search, shared with the search threads
    struct Snapshot;
    std::shared_ptr<Snapshot> buildSnapshot();

    /**
     * @brief Only retains those messages from a list of messages that satisfy
     *        a search query. The work is split over the global thread pool.
     *
     * @param text          the search query -- will be parsed for MessagePredicates
     * @param snapshot      list of messages to filter
     * @param cancelled     stops the search early once set
     *
     * @return the messages from "snapshot" matching "text", in order
     */
    static std::vector<MessagePtr> filter(
        const QString &text, const std::vector<MessagePtr> &snapshot,
        const std::atomic_bool &cancelled);

    /**
     * @brief Checks the input for tags and registers their corresponding
     *        predicates.
     *
     * @param input the string to check for tags
     * @return a vector of MessagePredicates requested in the input
     */
    static std::vector<std::unique_ptr<MessagePredicate>> parsePredicates(
        const QString &input);

    std::shared_ptr<Snapshot> snapshot_;
    QTimer searchTimer_;
    /// Set to stop the search that is currently running
    std::shared_ptr<std::atomic_bool> cancelSearch_;
    uint64_t searchGeneration_ = 0;
    QLineEdit *searchInput_{};
    ChannelView *channelView_{};
    QString channelName_{};
    Split *split_ = nullptr;
    QList<std::reference_wrapper<ChannelView>> searchChannels_;
};

}  // namespace chatterino
