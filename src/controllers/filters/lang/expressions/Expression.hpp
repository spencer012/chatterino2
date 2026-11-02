// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/filters/lang/Types.hpp"

#include <QString>
#include <QVariant>

#include <memory>
#include <vector>

namespace chatterino {
struct Message;
class Channel;
}  // namespace chatterino

namespace chatterino::filters {

struct RunContext {
    const Message &message;
    Channel *channel;
};

/// Controls how a parsed filter is prepared for evaluation. None of these
/// change what a filter evaluates to, only how much work that takes.
struct OptimizeOptions {
    /// Evaluate the operands of `&&` and `||` chains cheapest first.
    bool reorder = true;
    /// Combine `x match r"a" || x match r"b"` into one regular expression.
    bool mergeRegexes = true;
    /// Combine `x contains "a" || x contains "b"` into one search for each
    /// literal in a case folded copy of `x`.
    bool foldContains = true;
};

class Expression
{
public:
    virtual ~Expression() = default;

    virtual QVariant execute(RunContext context) const = 0;
    virtual PossibleType synthesizeType() const = 0;
    virtual QString debug() const = 0;
    virtual QString filterString() const = 0;

    /// Prepares this expression and its children for evaluation. This is
    /// called once on the root of a parsed filter.
    virtual void optimize(const OptimizeOptions & /* options */)
    {
    }

    /// A rough estimate of how expensive execute() is, in nanoseconds.
    virtual int cost() const
    {
        return 1;
    }

    /// True if every value execute() can return converts to a Bool.
    ///
    /// This must hold for the values execute() actually returns, even if the
    /// expression is ill-typed.
    virtual bool alwaysConvertsToBool() const
    {
        return false;
    }

    /// True if every value execute() can return is a QString.
    virtual bool alwaysReturnsString() const
    {
        return false;
    }
};

using ExpressionPtr = std::unique_ptr<Expression>;
using ExpressionList = std::vector<std::unique_ptr<Expression>>;

}  // namespace chatterino::filters
