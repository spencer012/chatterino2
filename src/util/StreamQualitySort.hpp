// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QStringList>

namespace chatterino {

// Order Choose-popup labels from highest resolution to lowest. Higher frame
// rate wins a tie. `best` stays first, unrecognized names keep their relative
// order, `worst` follows those, and audio-only names stay last. A trailing
// " (...)" suffix is ignored for rank. The selectable strings are unchanged.
QStringList sortStreamQualities(QStringList options);

}  // namespace chatterino
