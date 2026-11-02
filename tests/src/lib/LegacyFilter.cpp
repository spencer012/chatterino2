// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/LegacyFilter.hpp"

#include "controllers/filters/lang/expressions/IdentifierExpression.hpp"
#include "controllers/filters/lang/expressions/ListExpression.hpp"
#include "controllers/filters/lang/expressions/RegexExpression.hpp"
#include "controllers/filters/lang/expressions/UnaryOperation.hpp"
#include "controllers/filters/lang/expressions/ValueExpression.hpp"

#include <QRegularExpression>

namespace {

using namespace chatterino::filters;

/// Loosely compares `lhs` with `rhs`.
/// This attempts to convert both variants to a common type if they're not equal.
bool looselyCompareVariants(QVariant &lhs, QVariant &rhs)
{
    // Qt 6 and later don't convert types as much as Qt 5 did when comparing.
    //
    // Based on QVariant::cmp from Qt 5.15
    // https://github.com/qt/qtbase/blob/29400a683f96867133b28299c0d0bd6bcf40df35/src/corelib/kernel/qvariant.cpp#L4039-L4071
    if (lhs.metaType() != rhs.metaType())
    {
        if (rhs.canConvert(lhs.metaType()))
        {
            if (!rhs.convert(lhs.metaType()))
            {
                return false;
            }
        }
        else
        {
            // try the opposite conversion, it might work
            qSwap(lhs, rhs);
            if (!rhs.convert(lhs.metaType()))
            {
                return false;
            }
        }
    }

    return lhs == rhs;
}

QString explainIllType(const IllTyped &ill)
{
    return QString("%1\n\nProblem occurred here:\n%2")
        .arg(ill.message)
        .arg(ill.expr->filterString());
}

}  // namespace

namespace chatterino::filters::legacy {

BinaryOperation::BinaryOperation(TokenType op, ExpressionPtr left,
                                 ExpressionPtr right)
    : op_(op)
    , left_(std::move(left))
    , right_(std::move(right))
{
}

QVariant BinaryOperation::execute(RunContext context) const
{
    auto left = this->left_->execute(context);
    auto right = this->right_->execute(context);
    switch (this->op_)
    {
        case PLUS:
            if (variantIs(left, QMetaType::QString) &&
                right.canConvert<QString>())
            {
                return left.toString().append(right.toString());
            }
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() + right.toInt();
            }
            return 0;
        case MINUS:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() - right.toInt();
            }
            return 0;
        case MULTIPLY:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() * right.toInt();
            }
            return 0;
        case DIVIDE:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() / right.toInt();
            }
            return 0;
        case MOD:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() % right.toInt();
            }
            return 0;
        case OR:
            if (convertVariantTypes(left, right, QMetaType::Bool))
            {
                return left.toBool() || right.toBool();
            }
            return false;
        case AND:
            if (convertVariantTypes(left, right, QMetaType::Bool))
            {
                return left.toBool() && right.toBool();
            }
            return false;
        case EQ:
            if (variantTypesMatch(left, right, QMetaType::QString))
            {
                return left.toString().compare(right.toString(),
                                               Qt::CaseInsensitive) == 0;
            }
            return looselyCompareVariants(left, right);
        case NEQ:
            if (variantTypesMatch(left, right, QMetaType::QString))
            {
                return left.toString().compare(right.toString(),
                                               Qt::CaseInsensitive) != 0;
            }
            return !looselyCompareVariants(left, right);
        case LT:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() < right.toInt();
            }
            return false;
        case GT:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() > right.toInt();
            }
            return false;
        case LTE:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() <= right.toInt();
            }
            return false;
        case GTE:
            if (convertVariantTypes(left, right, QMetaType::Int))
            {
                return left.toInt() >= right.toInt();
            }
            return false;
        case CONTAINS:
            if (variantIs(left, QMetaType::QStringList) &&
                right.canConvert<QString>())
            {
                return left.toStringList().contains(right.toString(),
                                                    Qt::CaseInsensitive);
            }

            if (variantIs(left, QMetaType::QVariantList))
            {
                return left.toList().contains(right);
            }

            if (left.canConvert<QString>() && right.canConvert<QString>())
            {
                return left.toString().contains(right.toString(),
                                                Qt::CaseInsensitive);
            }

            return false;
        case STARTS_WITH:
            if (variantIs(left, QMetaType::QStringList) &&
                right.canConvert<QString>())
            {
                auto list = left.toStringList();
                return !list.isEmpty() &&
                       list.first().compare(right.toString(),
                                            Qt::CaseInsensitive) == 0;
            }

            if (variantIs(left, QMetaType::QVariantList))
            {
                return left.toList().startsWith(right);
            }

            if (left.canConvert<QString>() && right.canConvert<QString>())
            {
                return left.toString().startsWith(right.toString(),
                                                  Qt::CaseInsensitive);
            }

            return false;

        case ENDS_WITH:
            if (variantIs(left, QMetaType::QStringList) &&
                right.canConvert<QString>())
            {
                auto list = left.toStringList();
                return !list.isEmpty() &&
                       list.last().compare(right.toString(),
                                           Qt::CaseInsensitive) == 0;
            }

            if (variantIs(left, QMetaType::QVariantList))
            {
                return left.toList().endsWith(right);
            }

            if (left.canConvert<QString>() && right.canConvert<QString>())
            {
                return left.toString().endsWith(right.toString(),
                                                Qt::CaseInsensitive);
            }

            return false;
        case MATCH: {
            if (!left.canConvert<QString>())
            {
                return false;
            }

            auto matching = left.toString();

            switch (static_cast<QMetaType::Type>(right.typeId()))
            {
                case QMetaType::QRegularExpression: {
                    return right.toRegularExpression()
                        .match(matching)
                        .hasMatch();
                }
                case QMetaType::QVariantList: {
                    auto list = right.toList();

                    // list must be two items
                    if (list.size() != 2)
                    {
                        return false;
                    }

                    // list must be a regular expression and an int
                    if (variantIsNot(list.at(0),
                                     QMetaType::QRegularExpression) ||
                        variantIsNot(list.at(1), QMetaType::Int))
                    {
                        return false;
                    }

                    auto match =
                        list.at(0).toRegularExpression().match(matching);

                    // if matched, return nth capture group. Otherwise, return ""
                    if (match.hasMatch())
                    {
                        return match.captured(list.at(1).toInt());
                    }
                    else
                    {
                        return "";
                    }
                }
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

PossibleType BinaryOperation::synthesizeType() const
{
    auto leftSyn = this->left_->synthesizeType();
    auto rightSyn = this->right_->synthesizeType();

    // Return if either operand is ill-typed
    if (isIllTyped(leftSyn))
    {
        return leftSyn;
    }
    else if (isIllTyped(rightSyn))
    {
        return rightSyn;
    }

    auto left = std::get<TypeClass>(leftSyn);
    auto right = std::get<TypeClass>(rightSyn);

    switch (this->op_)
    {
        case PLUS:
            if (left == Type::String)
            {
                return TypeClass{Type::String};  // String concatenation
            }
            else if (left == Type::Int && right == Type::Int)
            {
                return TypeClass{Type::Int};
            }

            return IllTyped{this, "Can only add Ints or concatenate a String"};
        case MINUS:
        case MULTIPLY:
        case DIVIDE:
        case MOD:
            if (left == Type::Int && right == Type::Int)
            {
                return TypeClass{Type::Int};
            }

            return IllTyped{this, "Can only perform operation with Ints"};
        case OR:
        case AND:
            if (left == Type::Bool && right == Type::Bool)
            {
                return TypeClass{Type::Bool};
            }

            return IllTyped{this,
                            "Can only perform logical operations with Bools"};
        case EQ:
        case NEQ:
            // equals/not equals always produces a valid output
            return TypeClass{Type::Bool};
        case LT:
        case GT:
        case LTE:
        case GTE:
            if (left == Type::Int && right == Type::Int)
            {
                return TypeClass{Type::Bool};
            }

            return IllTyped{this, "Can only perform comparisons with Ints"};
        case STARTS_WITH:
        case ENDS_WITH:
            if (isList(left))
            {
                return TypeClass{Type::Bool};
            }
            if (left == Type::String && right == Type::String)
            {
                return TypeClass{Type::Bool};
            }

            return IllTyped{
                this,
                "Can only perform starts/ends with a List or two Strings"};
        case CONTAINS:
            if (isList(left))
            {
                return TypeClass{Type::Bool};
            }
            if (left == Type::String && right == Type::String)
            {
                return TypeClass{Type::Bool};
            }

            return IllTyped{
                this, "Can only perform contains with a List or two Strings"};
        case MATCH: {
            if (left != Type::String)
            {
                return IllTyped{this,
                                "Left argument of match must be a String"};
            }

            if (right == Type::RegularExpression)
            {
                return TypeClass{Type::Bool};
            }
            if (right == Type::MatchingSpecifier)
            {  // group capturing
                return TypeClass{Type::String};
            }

            return IllTyped{this, "Can only match on a RegularExpression or a "
                                  "MatchingSpecifier"};
        }
        default:
            return IllTyped{this, "Not implemented"};
    }
}

QString BinaryOperation::debug() const
{
    return QString("BinaryOp[%1](%2 : %3, %4 : %5)")
        .arg(tokenTypeToInfoString(this->op_))
        .arg(this->left_->debug())
        .arg(possibleTypeToString(this->left_->synthesizeType()))
        .arg(this->right_->debug())
        .arg(possibleTypeToString(this->right_->synthesizeType()));
}

QString BinaryOperation::filterString() const
{
    const auto opText = [&]() -> QString {
        switch (this->op_)
        {
            case AND:
                return "&&";
            case OR:
                return "||";
            case PLUS:
                return "+";
            case MINUS:
                return "-";
            case MULTIPLY:
                return "*";
            case DIVIDE:
                return "/";
            case MOD:
                return "%";
            case EQ:
                return "==";
            case NEQ:
                return "!=";
            case LT:
                return "<";
            case GT:
                return ">";
            case LTE:
                return "<=";
            case GTE:
                return ">=";
            case CONTAINS:
                return "contains";
            case STARTS_WITH:
                return "startswith";
            case ENDS_WITH:
                return "endswith";
            case MATCH:
                return "match";
            default:
                return "";
        }
    }();

    return QString("(%1 %2 %3)")
        .arg(this->left_->filterString())
        .arg(opText)
        .arg(this->right_->filterString());
}

FilterParser::FilterParser(const QString &text)
    : text_(text)
    , tokenizer_(Tokenizer(text))
    , builtExpression_(this->parseExpression(true))
{
    if (!this->valid_)
    {
        return;
    }

    // safety: returnType must not live longer than the parsed expression. See
    // comment on IllTyped::expr.
    auto returnType = this->builtExpression_->synthesizeType();
    if (isIllTyped(returnType))
    {
        this->errorLog(explainIllType(std::get<IllTyped>(returnType)));
        return;
    }

    this->returnType_ = std::get<TypeClass>(returnType).type;
}

bool FilterParser::valid() const
{
    return this->valid_;
}

Type FilterParser::returnType() const
{
    return this->returnType_;
}

ExpressionPtr FilterParser::release()
{
    ExpressionPtr ret;
    this->builtExpression_.swap(ret);
    return ret;
}

ExpressionPtr FilterParser::parseExpression(bool top)
{
    auto e = this->parseAnd();
    while (this->tokenizer_.hasNext() &&
           this->tokenizer_.nextTokenType() == TokenType::OR)
    {
        this->tokenizer_.next();
        auto nextAnd = this->parseAnd();
        e = std::make_unique<BinaryOperation>(TokenType::OR, std::move(e),
                                              std::move(nextAnd));
    }

    if (this->tokenizer_.hasNext() && top)
    {
        this->errorLog(QString("Unexpected token at end: %1")
                           .arg(this->tokenizer_.preview()));
    }

    return e;
}

ExpressionPtr FilterParser::parseAnd()
{
    auto e = this->parseUnary();
    while (this->tokenizer_.hasNext() &&
           this->tokenizer_.nextTokenType() == TokenType::AND)
    {
        this->tokenizer_.next();
        auto nextUnary = this->parseUnary();
        e = std::make_unique<BinaryOperation>(TokenType::AND, std::move(e),
                                              std::move(nextUnary));
    }
    return e;
}

ExpressionPtr FilterParser::parseUnary()
{
    if (this->tokenizer_.hasNext() && this->tokenizer_.nextTokenIsUnaryOp())
    {
        this->tokenizer_.next();
        auto type = this->tokenizer_.tokenType();
        auto nextCondition = this->parseCondition();
        return std::make_unique<UnaryOperation>(type, std::move(nextCondition));
    }
    else
    {
        return this->parseCondition();
    }
}

ExpressionPtr FilterParser::parseParentheses()
{
    // Don't call .next() before calling this method
    assert(this->tokenizer_.nextTokenType() == TokenType::LP);

    this->tokenizer_.next();
    auto e = this->parseExpression();
    if (this->tokenizer_.hasNext() &&
        this->tokenizer_.nextTokenType() == TokenType::RP)
    {
        this->tokenizer_.next();
        return e;
    }
    else
    {
        const auto message =
            this->tokenizer_.hasNext()
                ? QString("Missing closing parentheses: got %1")
                      .arg(this->tokenizer_.preview())
                : "Missing closing parentheses at end of statement";
        this->errorLog(message);

        return e;
    }
}

ExpressionPtr FilterParser::parseCondition()
{
    ExpressionPtr value;
    // parse expression wrapped in parentheses
    if (this->tokenizer_.hasNext() &&
        this->tokenizer_.nextTokenType() == TokenType::LP)
    {
        // get value inside parentheses
        value = this->parseParentheses();
    }
    else
    {
        // get current value
        value = this->parseValue();
    }

    // expecting an operator or nothing
    while (this->tokenizer_.hasNext())
    {
        if (this->tokenizer_.nextTokenIsBinaryOp())
        {
            this->tokenizer_.next();
            auto type = this->tokenizer_.tokenType();
            auto nextValue = this->parseValue();
            return std::make_unique<BinaryOperation>(type, std::move(value),
                                                     std::move(nextValue));
        }
        else if (this->tokenizer_.nextTokenIsMathOp())
        {
            this->tokenizer_.next();
            auto type = this->tokenizer_.tokenType();
            auto nextValue = this->parseValue();
            value = std::make_unique<BinaryOperation>(type, std::move(value),
                                                      std::move(nextValue));
        }
        else if (this->tokenizer_.nextTokenType() == TokenType::RP)
        {
            // RP, so move on
            break;
        }
        else if (!this->tokenizer_.nextTokenIsOp())
        {
            this->errorLog(QString("Expected an operator but got %1 %2")
                               .arg(this->tokenizer_.preview())
                               .arg(tokenTypeToInfoString(
                                   this->tokenizer_.nextTokenType())));
            break;
        }
        else
        {
            break;
        }
    }

    return value;
}

ExpressionPtr FilterParser::parseValue()
{
    // parse a literal or an expression wrapped in parenthsis
    if (this->tokenizer_.hasNext())
    {
        auto type = this->tokenizer_.nextTokenType();
        if (type == TokenType::INT)
        {
            return std::make_unique<ValueExpression>(
                this->tokenizer_.next().toInt(), type);
        }
        else if (type == TokenType::STRING)
        {
            auto before = this->tokenizer_.next();
            // remove quote marks
            auto val = before.mid(1);
            val.chop(1);
            val = val.replace("\\\"", "\"");
            return std::make_unique<ValueExpression>(val, type);
        }
        else if (type == TokenType::IDENTIFIER)
        {
            return createIdentifierExpression(this->tokenizer_.next());
        }
        else if (type == TokenType::REGULAR_EXPRESSION)
        {
            auto before = this->tokenizer_.next();
            // remove quote marks and r/ri
            bool caseInsensitive = before.startsWith("ri");
            auto val = before.mid(caseInsensitive ? 3 : 2);
            val.chop(1);
            val = val.replace("\\\"", "\"");
            return std::make_unique<RegexExpression>(val, caseInsensitive);
        }
        else if (type == TokenType::LP)
        {
            return this->parseParentheses();
        }
        else if (type == TokenType::LIST_START)
        {
            return this->parseList();
        }
        else
        {
            this->tokenizer_.next();
            this->errorLog(QString("Expected value but got %1 %2")
                               .arg(this->tokenizer_.current())
                               .arg(tokenTypeToInfoString(type)));
        }
    }
    else
    {
        this->errorLog("Unexpected end of statement");
    }

    return std::make_unique<ValueExpression>(0, TokenType::INT);
}

ExpressionPtr FilterParser::parseList()
{
    // Don't call .next() before calling this method
    assert(this->tokenizer_.nextTokenType() == TokenType::LIST_START);
    this->tokenizer_.next();

    ExpressionList list;
    bool first = true;

    while (this->tokenizer_.hasNext())
    {
        if (this->tokenizer_.nextTokenType() == TokenType::LIST_END)
        {
            this->tokenizer_.next();
            return std::make_unique<ListExpression>(std::move(list));
        }
        else if (this->tokenizer_.nextTokenType() == TokenType::COMMA && !first)
        {
            this->tokenizer_.next();
            list.push_back(this->parseValue());
            first = false;
        }
        else if (first)
        {
            list.push_back(this->parseValue());
            first = false;
        }
        else
        {
            break;
        }
    }

    const auto message =
        this->tokenizer_.hasNext()
            ? QString("Missing closing list braces: got %1")
                  .arg(this->tokenizer_.preview())
            : "Missing closing list braces at end of statement";
    this->errorLog(message);
    return std::make_unique<ListExpression>(ExpressionList());
}

void FilterParser::errorLog(const QString &text, bool expand)
{
    this->valid_ = false;
    if (expand || this->parseLog_.size() == 0)
    {
        this->parseLog_ << text;
    }
}

const QStringList &FilterParser::errors() const
{
    return this->parseLog_;
}

QString FilterParser::debugString() const
{
    return this->builtExpression_->debug();
}

}  // namespace chatterino::filters::legacy
