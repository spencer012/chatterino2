// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>

class QPlainTextEdit;

namespace chatterino {

/// A scrollable snapshot. It never enters a nested modal event loop.
class CacheDiagnosticsDialog : public QDialog
{
public:
    explicit CacheDiagnosticsDialog(const QString &report,
                                    QWidget *parent = nullptr);
    void setReport(const QString &report);

private:
    QPlainTextEdit *text_;
};

}  // namespace chatterino
