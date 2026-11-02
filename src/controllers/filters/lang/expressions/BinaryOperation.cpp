// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/filters/lang/expressions/BinaryOperation.hpp"

#include "controllers/filters/lang/expressions/RegexExpression.hpp"
#include "controllers/filters/lang/expressions/ValueExpression.hpp"

#include <QRegularExpression>
#include <QStringBuilder>

#include <algorithm>

namespace {

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

/// Converts `value` to a Bool like the `&&` and `||` operators do. Values that
/// can't be converted are false.
bool isTruthy(QVariant value)
{
    return value.convert(QMetaType(QMetaType::Bool)) && value.toBool();
}

bool convertsToBool(QVariant value)
{
    return value.convert(QMetaType(QMetaType::Bool));
}

/// Checks that `pattern` matches the same strings when it's wrapped in
/// `(?:...)` and combined with other patterns using `|`.
///
/// This is deliberately conservative. Anything that refers to capture group
/// numbers, reaches past the end of the pattern (\Q, (?x) comments) or
/// controls backtracking is rejected.
bool canMergePattern(QStringView pattern)
{
    for (qsizetype i = 0; i < pattern.size(); i++)
    {
        auto c = pattern[i];
        if (c == u'\\')
        {
            if (i + 1 >= pattern.size())
            {
                return false;
            }
            auto next = pattern[i + 1];
            if (next.isDigit() || next == u'g' || next == u'k' ||
                next == u'Q' || next == u'E')
            {
                return false;
            }
            i++;
            continue;
        }

        if (c != u'(' || i + 1 >= pattern.size())
        {
            continue;
        }

        auto next = pattern[i + 1];
        if (next == u'*')
        {
            // (*VERB)
            return false;
        }
        if (next != u'?')
        {
            continue;
        }

        auto rest = pattern.sliced(i + 2);
        if (rest.startsWith(u':') || rest.startsWith(u'=') ||
            rest.startsWith(u'!') || rest.startsWith(u'>') ||
            rest.startsWith(u"<=") || rest.startsWith(u"<!"))
        {
            continue;
        }

        // Option settings like (?i) or (?s-m:...), but not (?x)
        qsizetype end = 0;
        while (end < rest.size() &&
               QStringView(u"imnsJU-^").contains(rest[end]))
        {
            end++;
        }
        if (end == 0 || end >= rest.size() ||
            (rest[end] != u')' && rest[end] != u':'))
        {
            return false;
        }
    }

    return true;
}

using namespace chatterino::filters;

/// Case folds `text` the way QString's case-insensitive search compares
/// characters: one code point at a time, so the length never changes (e.g. ß
/// stays ß). Same result as QString::toCaseFolded(), but about twice as fast
/// for mostly ASCII text.
///
/// A case-sensitive search in folded text finds the same matches as a
/// case-insensitive search in the original text.
QString foldCase(QStringView text)
{
    QString folded(text.size(), Qt::Uninitialized);
    auto *out = folded.data();
    for (qsizetype i = 0; i < text.size(); i++)
    {
        auto c = text[i].unicode();
        if (c < 0x80)
        {
            // Fast path for ASCII
            out[i] = (c >= u'A' && c <= u'Z') ? QChar(c + 0x20) : QChar(c);
            continue;
        }
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() &&
            text[i + 1].isLowSurrogate())
        {
            auto ucs4 = QChar::toCaseFolded(
                QChar::surrogateToUcs4(c, text[i + 1].unicode()));
            out[i] = QChar(QChar::highSurrogate(ucs4));
            out[i + 1] = QChar(QChar::lowSurrogate(ucs4));
            i++;
            continue;
        }
        out[i] = QChar(static_cast<char16_t>(QChar::toCaseFolded(c)));
    }
    return folded;
}

/// `subject contains "a" || subject contains "b"`, folding the subject once.
class MergedContains : public Expression
{
public:
    MergedContains(const Expression *subject, QStringList needles, int cost)
        : subject_(subject)
        , needles_(std::move(needles))
        , cost_(cost)
    {
    }

    QVariant execute(RunContext context) const override
    {
        // Only created for subjects that always return Strings
        auto folded = foldCase(this->subject_->execute(context).toString());
        return std::ranges::any_of(this->needles_, [&](const auto &needle) {
            return folded.contains(needle);
        });
    }

    PossibleType synthesizeType() const override
    {
        return TypeClass{Type::Bool};
    }

    QString debug() const override
    {
        return QString("MergedContains(%1, %2)")
            .arg(this->subject_->debug(), this->needles_.join(", "));
    }

    QString filterString() const override
    {
        QStringList parts;
        for (const auto &needle : this->needles_)
        {
            parts.append(QString("(%1 contains \"%2\")")
                             .arg(this->subject_->filterString(), needle));
        }
        return u'(' % parts.join(" || ") % u')';
    }

    int cost() const override
    {
        return this->cost_;
    }

    bool alwaysConvertsToBool() const override
    {
        return true;
    }

private:
    const Expression *subject_;
    QStringList needles_;
    int cost_;
};

/// `subject match r"a" || subject match r"b"`, evaluated with one regex.
class MergedMatch : public Expression
{
public:
    MergedMatch(const Expression *subject, QRegularExpression regex, int cost)
        : subject_(subject)
        , regex_(std::move(regex))
        , cost_(cost)
    {
    }

    QVariant execute(RunContext context) const override
    {
        auto subject = this->subject_->execute(context);
        if (!subject.canConvert<QString>())
        {
            return false;
        }
        return this->regex_.match(subject.toString()).hasMatch();
    }

    PossibleType synthesizeType() const override
    {
        return TypeClass{Type::Bool};
    }

    QString debug() const override
    {
        return QString("MergedMatch(%1, %2)")
            .arg(this->subject_->debug(), this->regex_.pattern());
    }

    QString filterString() const override
    {
        return QString("(%1 match r\"%2\")")
            .arg(this->subject_->filterString(), this->regex_.pattern());
    }

    int cost() const override
    {
        return this->cost_;
    }

    bool alwaysConvertsToBool() const override
    {
        return true;
    }

private:
    const Expression *subject_;
    QRegularExpression regex_;
    int cost_;
};

/// Approximate cost of an operator on top of evaluating its operands, in
/// nanoseconds, measured on a ~60 character message (see
/// tests/src/FilterPerformance.cpp).
int operatorCost(TokenType op)
{
    switch (op)
    {
        case AND:
        case OR:
            return 0;
        case EQ:
        case NEQ:
        case STARTS_WITH:
        case ENDS_WITH:
            return 80;
        case CONTAINS:
            // Case-insensitive search in a String. Lists are cheaper, see
            // BinaryOperation::cost().
            return 450;
        case MATCH:
            return 400;
        default:
            return 40;
    }
}

}  // namespace

namespace chatterino::filters {

BinaryOperation::BinaryOperation(TokenType op, ExpressionPtr left,
                                 ExpressionPtr right)
    : op_(op)
    , left_(std::move(left))
    , right_(std::move(right))
    , rightConvertsToBool_(this->right_->alwaysConvertsToBool())
{
}

QVariant BinaryOperation::execute(RunContext context) const
{
    // Short-circuit && and || so the right side is only evaluated when it can
    // change the result.
    if (this->op_ == AND)
    {
        return this->executeAnd(context);
    }
    if (this->op_ == OR)
    {
        return this->executeOr(context);
    }

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

// Both operators used to evaluate both sides and then return false if either
// side didn't convert to a Bool. These keep that result.
QVariant BinaryOperation::executeAnd(RunContext context) const
{
    if (!this->operands_.empty())
    {
        for (const auto *operand : this->operands_)
        {
            if (!isTruthy(operand->execute(context)))
            {
                return false;
            }
        }
        return true;
    }

    // An unconvertible or false side makes the result false either way
    if (!isTruthy(this->left_->execute(context)))
    {
        return false;
    }
    return isTruthy(this->right_->execute(context));
}

QVariant BinaryOperation::executeOr(RunContext context) const
{
    if (!this->operands_.empty())
    {
        // Only built when every operand converts to a Bool
        for (const auto *operand : this->operands_)
        {
            if (isTruthy(operand->execute(context)))
            {
                return true;
            }
        }
        return false;
    }

    auto left = this->left_->execute(context);
    if (!left.convert(QMetaType(QMetaType::Bool)))
    {
        return false;
    }
    if (left.toBool())
    {
        // The result is still false if the right side doesn't convert
        return this->rightConvertsToBool_ ||
               convertsToBool(this->right_->execute(context));
    }
    return isTruthy(this->right_->execute(context));
}

void BinaryOperation::collectOperands(std::vector<Expression *> &out) const
{
    for (const auto *side : {&this->left_, &this->right_})
    {
        auto *binary = dynamic_cast<BinaryOperation *>(side->get());
        if (binary && binary->op_ == this->op_)
        {
            binary->collectOperands(out);
        }
        else
        {
            out.push_back(side->get());
        }
    }
}

void BinaryOperation::optimize(const OptimizeOptions &options)
{
    this->operands_.clear();
    this->merged_.clear();

    if (this->op_ != AND && this->op_ != OR)
    {
        this->left_->optimize(options);
        this->right_->optimize(options);
        return;
    }

    std::vector<Expression *> operands;
    this->collectOperands(operands);
    for (auto *operand : operands)
    {
        operand->optimize(options);
    }

    // a && b is only true if every operand converts to a Bool and is true, so
    // the operands can be evaluated in any order. For ||, an operand that
    // doesn't convert makes the result false even if another operand is
    // true. Only take those apart if that can't happen.
    if (this->op_ == OR &&
        !std::ranges::all_of(operands, [](const auto *operand) {
            return operand->alwaysConvertsToBool();
        }))
    {
        return;
    }

    if (this->op_ == OR && options.mergeRegexes)
    {
        this->mergeMatches(operands);
    }
    if (this->op_ == OR && options.foldContains)
    {
        this->mergeContains(operands);
    }

    if (options.reorder)
    {
        // Stable, so equally expensive operands keep the order they were
        // written in.
        std::ranges::stable_sort(operands, {}, [](const auto *operand) {
            return operand->cost();
        });
    }

    this->operands_.assign(operands.begin(), operands.end());
}

void BinaryOperation::mergeMatches(std::vector<Expression *> &operands)
{
    struct Group {
        const Expression *subject;
        bool caseInsensitive;
        QStringList patterns;
        std::vector<size_t> indices;
        int cost = 0;
    };
    std::vector<Group> groups;

    for (size_t i = 0; i < operands.size(); i++)
    {
        auto *match = dynamic_cast<BinaryOperation *>(operands[i]);
        if (!match || match->op_ != MATCH)
        {
            continue;
        }
        const auto *regex =
            dynamic_cast<const RegexExpression *>(match->right_.get());
        // An invalid pattern never matches, it must not make the merged
        // pattern invalid.
        if (!regex || !regex->regex().isValid() ||
            !canMergePattern(regex->pattern()))
        {
            continue;
        }

        auto subject = match->left_->filterString();
        auto it = std::ranges::find_if(groups, [&](const Group &group) {
            return group.caseInsensitive == regex->caseInsensitive() &&
                   group.subject->filterString() == subject;
        });
        if (it == groups.end())
        {
            groups.push_back({
                .subject = match->left_.get(),
                .caseInsensitive = regex->caseInsensitive(),
            });
            it = std::prev(groups.end());
        }
        it->patterns.append(u"(?:" % regex->pattern() % u')');
        it->indices.push_back(i);
        it->cost = std::max(it->cost, match->cost());
    }

    std::vector<bool> removed(operands.size(), false);
    for (auto &group : groups)
    {
        if (group.indices.size() < 2)
        {
            continue;
        }

        QRegularExpression combined(
            group.patterns.join(u'|'),
            group.caseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                  : QRegularExpression::NoPatternOption);
        if (!combined.isValid())
        {
            continue;
        }

        // One pass over the subject, slightly slower than a single regex
        auto cost = group.cost + 20 * static_cast<int>(group.indices.size());
        auto merged = std::make_unique<MergedMatch>(group.subject,
                                                    std::move(combined), cost);
        operands[group.indices.front()] = merged.get();
        for (size_t j = 1; j < group.indices.size(); j++)
        {
            removed[group.indices[j]] = true;
        }
        this->merged_.push_back(std::move(merged));
    }

    size_t i = 0;
    std::erase_if(operands, [&](const auto *) {
        return removed[i++];
    });
}

std::optional<QString> BinaryOperation::foldedNeedle() const
{
    if (this->op_ != CONTAINS)
    {
        return std::nullopt;
    }
    const auto *value =
        dynamic_cast<const ValueExpression *>(this->right_.get());
    if (!value || !variantIs(value->value(), QMetaType::QString))
    {
        return std::nullopt;
    }
    return foldCase(get<QString>(value->value()));
}

void BinaryOperation::mergeContains(std::vector<Expression *> &operands)
{
    struct Group {
        const Expression *subject;
        QStringList needles;
        std::vector<size_t> indices;
    };
    std::vector<Group> groups;

    for (size_t i = 0; i < operands.size(); i++)
    {
        auto *contains = dynamic_cast<BinaryOperation *>(operands[i]);
        if (!contains || !contains->left_->alwaysReturnsString())
        {
            continue;
        }
        auto needle = contains->foldedNeedle();
        if (!needle)
        {
            continue;
        }

        auto subject = contains->left_->filterString();
        auto it = std::ranges::find_if(groups, [&](const Group &group) {
            return group.subject->filterString() == subject;
        });
        if (it == groups.end())
        {
            groups.push_back({.subject = contains->left_.get()});
            it = std::prev(groups.end());
        }
        it->needles.append(*needle);
        it->indices.push_back(i);
    }

    std::vector<bool> removed(operands.size(), false);
    for (auto &group : groups)
    {
        if (group.indices.size() < 2)
        {
            continue;
        }

        // Folding the subject costs about as much as one case-insensitive
        // search, each needle then only a case-sensitive search
        auto cost = group.subject->cost() + operatorCost(CONTAINS) / 2 +
                    90 * static_cast<int>(group.needles.size());
        auto merged = std::make_unique<MergedContains>(
            group.subject, std::move(group.needles), cost);
        operands[group.indices.front()] = merged.get();
        for (size_t j = 1; j < group.indices.size(); j++)
        {
            removed[group.indices[j]] = true;
        }
        this->merged_.push_back(std::move(merged));
    }

    size_t i = 0;
    std::erase_if(operands, [&](const auto *) {
        return removed[i++];
    });
}

int BinaryOperation::cost() const
{
    if (!this->operands_.empty())
    {
        int total = 0;
        for (const auto *operand : this->operands_)
        {
            total += operand->cost();
        }
        return total;
    }

    auto own = operatorCost(this->op_);
    if (this->op_ == CONTAINS)
    {
        if (isList(this->left_->synthesizeType()))
        {
            own = 70;
        }
    }
    return own + this->left_->cost() + this->right_->cost();
}

bool BinaryOperation::alwaysConvertsToBool() const
{
    // execute() only returns Bools, Ints and Strings
    return true;
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

}  // namespace chatterino::filters
