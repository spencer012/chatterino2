// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/CacheDiagnosticsDialog.hpp"

#include "util/Clipboard.hpp"

#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace chatterino {

CacheDiagnosticsDialog::CacheDiagnosticsDialog(const QString &report,
                                               QWidget *parent)
    : QDialog(parent)
    , text_(new QPlainTextEdit(this))
{
    this->setWindowTitle("Message cache diagnostics");
    this->setWindowModality(Qt::NonModal);
    this->text_->setReadOnly(true);
    this->text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    this->setReport(report);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(this->text_, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *copy =
        buttons->addButton("Copy report", QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, this, [this] {
        crossPlatformCopy(this->text_->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    QSize size(780, 640);
    if (auto *screen =
            parent ? parent->screen() : QGuiApplication::primaryScreen())
    {
        size =
            size.boundedTo(screen->availableGeometry().size() - QSize(40, 80));
    }
    this->resize(size);
}

void CacheDiagnosticsDialog::setReport(const QString &report)
{
    this->text_->setPlainText(report);
}

}  // namespace chatterino
