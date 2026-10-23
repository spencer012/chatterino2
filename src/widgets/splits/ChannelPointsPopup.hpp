// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/channelpoints/ChannelPointsTypes.hpp"
#include "widgets/BasePopup.hpp"

#include <QHash>
#include <QPointer>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace chatterino {

class Split;
class ChannelPointsConfirmDialog;
class ChannelPointsRewardRow;

/// Lists a channel's rewards. Rows are kept and updated in place, so the list
/// doesn't jump around or lose hover/focus while it refreshes.
class ChannelPointsPopup : public BasePopup
{
    Q_OBJECT

public:
    ChannelPointsPopup(Split *split, QWidget *parent = nullptr);
    ~ChannelPointsPopup() override;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void themeChangedEvent() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildHeader(QVBoxLayout *root);
    QWidget *buildConnectPage();
    QWidget *buildMessagePage();
    QWidget *buildListPage();
    void buildFooter(QVBoxLayout *root);

    void refresh();
    void refreshHeader(const ChannelPointsViewState &state);
    void refreshConnectPage(const ChannelPointsViewState &state);
    void refreshList(const ChannelPointsViewState &state);
    void refreshFooter(const ChannelPointsViewState &state);

    void handleRowAction(const ChannelPointRewardView &view);
    void openConfirmDialog(const ChannelPointRewardView &view);
    void toggleFavorite(const QString &rewardId);
    [[nodiscard]] bool isFavorite(const QString &rewardId) const;

    void attachToController();
    void detachFromController();

    Split *split_{};
    QString channelLogin_;
    bool attached_ = false;
    std::optional<int> balance_;

    QLabel *channelLabel_{};
    QLabel *balanceLabel_{};
    QToolButton *menuButton_{};
    QLineEdit *searchEdit_{};
    QLabel *banner_{};

    QStackedWidget *stack_{};
    QWidget *connectPage_{};
    QWidget *messagePage_{};
    QWidget *listPage_{};

    // Connect page
    QLabel *connectTitle_{};
    QLabel *connectText_{};
    QLabel *codeLabel_{};
    QLabel *codeHint_{};
    QPushButton *connectButton_{};
    QPushButton *copyCodeButton_{};
    QPushButton *openActivateButton_{};
    QPushButton *cancelLoginButton_{};
    QString verificationUri_;

    // Message page (loading/empty/error)
    QLabel *messageLabel_{};
    QPushButton *retryButton_{};

    // List page
    QScrollArea *scrollArea_{};
    QVBoxLayout *listLayout_{};
    QLabel *favoritesHeader_{};
    QLabel *availableHeader_{};
    QLabel *unavailableHeader_{};
    QLabel *noMatchesLabel_{};
    QHash<QString, ChannelPointsRewardRow *> rows_;
    QStringList currentOrder_;

    // Footer
    QLabel *liveLabel_{};
    QLabel *updatedLabel_{};
    QPushButton *stopAllButton_{};

    QPointer<ChannelPointsConfirmDialog> confirmDialog_;
    QTimer refreshTimer_;
};

}  // namespace chatterino
