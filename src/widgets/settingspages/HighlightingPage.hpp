// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QTableView;

namespace chatterino {

class HighlightingPage : public SettingsPage
{
public:
    HighlightingPage();

    /// Also hides highlight rows whose name doesn't match the query, so a
    /// long list of user highlights can be searched from the settings dialog.
    bool filterElements(const QString &query) override;

private:
    QTableView *highlightsView_{};
};

}  // namespace chatterino
