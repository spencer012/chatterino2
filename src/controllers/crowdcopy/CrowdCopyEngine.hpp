// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <QDateTime>
#include <QString>

#include <vector>

namespace chatterino {

struct CrowdCopyResult {
    QString text;
    int userCount{0};
};

class CrowdCopyEngine
{
public:
    static CrowdCopyResult evaluate(const std::vector<MessagePtr> &messages,
                                    const QDateTime &now);
    static QString normalizeText(const QString &text);
};

}  // namespace chatterino
