// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/filters/lang/expressions/Expression.hpp"
#include "controllers/filters/lang/Tokenizer.hpp"
#include "controllers/filters/lang/Types.hpp"

namespace chatterino::filters {

class BinaryOperation : public Expression
{
public:
    BinaryOperation(TokenType op, ExpressionPtr left, ExpressionPtr right);

    QVariant execute(RunContext context) const override;
    PossibleType synthesizeType() const override;
    QString debug() const override;
    QString filterString() const override;

    void optimize(const OptimizeOptions &options) override;
    int cost() const override;
    bool alwaysConvertsToBool() const override;

private:
    QVariant executeAnd(RunContext context) const;
    QVariant executeOr(RunContext context) const;

    /// Collects the operands of the chain of `op_` operations rooted here,
    /// e.g. a, b and c for `(a && b) && c`.
    void collectOperands(std::vector<Expression *> &out) const;
    void mergeMatches(std::vector<Expression *> &operands);
    void mergeContains(std::vector<Expression *> &operands);

    /// The case folded right side, if this is `contains` with a String
    /// literal on the right.
    std::optional<QString> foldedNeedle() const;

    TokenType op_;
    ExpressionPtr left_;
    ExpressionPtr right_;

    /// Whether every value of `right_` converts to a Bool. Lets `||` skip
    /// the right side once the left side is true.
    bool rightConvertsToBool_;

    /// If set, this is the root of an `&&` or `||` chain and these are its
    /// operands in evaluation order. They are owned by the tree below this
    /// node or by `merged_`.
    std::vector<const Expression *> operands_;
    /// Expressions created by optimize() to replace several operands.
    std::vector<ExpressionPtr> merged_;
};

}  // namespace chatterino::filters
