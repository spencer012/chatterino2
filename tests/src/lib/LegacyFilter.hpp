// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/filters/lang/expressions/Expression.hpp"
#include "controllers/filters/lang/Tokenizer.hpp"
#include "controllers/filters/lang/Types.hpp"

/// The filter parser and BinaryOperation as they were before && and || were
/// short-circuited and reordered (copied from commit c13330521). Tests compare
/// the current evaluation against this one.
///
/// The other expression types are shared with the current code. Their
/// execute() functions did not change.
namespace chatterino::filters::legacy {

class BinaryOperation : public Expression
{
public:
    BinaryOperation(TokenType op, ExpressionPtr left, ExpressionPtr right);

    QVariant execute(RunContext context) const override;
    PossibleType synthesizeType() const override;
    QString debug() const override;
    QString filterString() const override;

private:
    TokenType op_;
    ExpressionPtr left_;
    ExpressionPtr right_;
};

class FilterParser
{
public:
    /**
     * Take input text & attempt to parse it into a filter
     **/
    FilterParser(const QString &text);

    bool valid() const;
    Type returnType() const;
    ExpressionPtr release();

    const QStringList &errors() const;
    QString debugString() const;

private:
    ExpressionPtr parseExpression(bool top = false);
    ExpressionPtr parseAnd();
    ExpressionPtr parseUnary();
    ExpressionPtr parseParentheses();
    ExpressionPtr parseCondition();
    ExpressionPtr parseValue();
    ExpressionPtr parseList();

    void errorLog(const QString &text, bool expand = false);

    QStringList parseLog_;
    bool valid_ = true;

    QString text_;
    Tokenizer tokenizer_;
    ExpressionPtr builtExpression_;
    Type returnType_ = Type::Bool;
};

}  // namespace chatterino::filters::legacy
