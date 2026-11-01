// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/HighlightingPage.hpp"

#include "controllers/highlights/HighlightBlacklistModel.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/Model.hpp"
#include "controllers/highlights/types/AnyHighlight.hpp"  // IWYU pragma: keep
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/helper/EditableModelView.hpp"
#include "widgets/settingspages/HighlightingWidget.hpp"

#include <QFileDialog>
#include <QHeaderView>
#include <QPushButton>
#include <QTableView>
#include <QTabWidget>

namespace chatterino {

HighlightingPage::HighlightingPage()
{
    LayoutCreator<HighlightingPage> layoutCreator(this);

    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();
    {
        // TABS
        auto tabs = layout.emplace<QTabWidget>();
        {
            // HIGHLIGHTS
            auto *highlightsWidget = new HighlightingWidget;
            this->highlightsView_ = highlightsWidget->findChild<QTableView *>();
            tabs->addTab(highlightsWidget, "Highlights");

            auto disabledUsers =
                tabs.appendTab(new QVBoxLayout, "Blacklisted Users");
            {
                disabledUsers.emplace<QLabel>(
                    "Disable notification sounds and highlights from certain "
                    "users (e.g. bots).");
                EditableModelView *view =
                    disabledUsers
                        .emplace<EditableModelView>(
                            (new HighlightBlacklistModel(nullptr))
                                ->initialized(&getSettings()->blacklistedUsers))
                        .getElement();

                view->addRegexHelpLink();
                view->setTitles({"Username", "Enable\nregex"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);

                // fourtf: make class extrend BaseWidget and add this to
                // dpiChanged
                QTimer::singleShot(1, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                });

                // We can safely ignore this signal connection since we own the view
                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->blacklistedUsers.append(
                        HighlightBlacklistUser{"blacklisted user", false});
                });
            }
        }

        // MISC
        layout.append(createCheckBox(
            "Play highlight sound even when Chatterino is focused",
            getSettings()->highlightAlwaysPlaySound));
        layout.append(createCheckBox(
            "Flash taskbar only stops highlighting when Chatterino is focused",
            getSettings()->longAlerts));
    }
}

bool HighlightingPage::filterElements(const QString &query)
{
    const auto pageMatch = SettingsPage::filterElements(query);

    if (this->highlightsView_ == nullptr)
    {
        return pageMatch;
    }

    auto *model = this->highlightsView_->model();
    bool rowMatch = false;
    for (int row = 0; row < model->rowCount(); row++)
    {
        const auto name =
            model->index(row, highlights::Model::Column::Name).data().toString();
        const auto match = name.contains(query, Qt::CaseInsensitive);
        rowMatch |= match;
        this->highlightsView_->setRowHidden(row, !match);
    }

    return pageMatch || rowMatch;
}

}  // namespace chatterino
