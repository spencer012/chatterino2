// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QHash>
#include <QSet>
#include <QString>

class QTimer;

namespace chatterino {

class Split;
class TwitchChannel;

namespace channeleventsbanner {
class EventBar;
class ShareRow;
}  // namespace channeleventsbanner

/**
 * Bars for the channel's hype train, poll and prediction, stacked under the
 * raid banner. Each one shows and hides on its own, so they don't replace
 * each other when several run at the same time.
 */
class ChannelEventsBanner final : public BaseWidget
{
    Q_OBJECT

public:
    explicit ChannelEventsBanner(Split *split);

    // Pass nullptr to detach from any channel.
    void setChannel(TwitchChannel *channel);

protected:
    void scaleChangedEvent(float newScale) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    using EventBar = channeleventsbanner::EventBar;
    using ShareRow = channeleventsbanner::ShareRow;

    void refresh();
    void refreshHypeTrain();
    void refreshPoll();
    void refreshPrediction();
    void tick();
    void updateTimer();

    [[nodiscard]] bool isExpanded(const QString &eventId,
                                  bool defaultExpanded) const;
    void toggleExpanded(const QString &eventId, bool defaultExpanded);

    /// Makes sure we can act for the user. Offers to open the channel points
    /// popup (where the sign-in lives) if not.
    bool ensureSignedIn(const QString &action);
    void vote(const QString &choiceId);
    void predict(const QString &outcomeId);

    Split *split_ = nullptr;
    TwitchChannel *channel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;

    EventBar *hypeTrainBar_ = nullptr;
    EventBar *pollBar_ = nullptr;
    EventBar *predictionBar_ = nullptr;
    std::vector<ShareRow *> pollRows_;
    std::vector<ShareRow *> predictionRows_;

    QTimer *tickTimer_ = nullptr;

    /// Event IDs the user closed.
    QSet<QString> dismissed_;
    /// Event ID -> expanded, for events the user expanded or collapsed.
    QHash<QString, bool> expanded_;
    /// The poll we're voting in right now.
    QString votePending_;
};

}  // namespace chatterino
