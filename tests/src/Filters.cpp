// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/accounts/AccountController.hpp"
#include "controllers/filters/lang/expressions/BinaryOperation.hpp"
#include "controllers/filters/lang/expressions/IdentifierExpression.hpp"
#include "controllers/filters/lang/expressions/RegexExpression.hpp"
#include "controllers/filters/lang/expressions/UnaryOperation.hpp"
#include "controllers/filters/lang/expressions/ValueExpression.hpp"
#include "controllers/filters/lang/Filter.hpp"
#include "controllers/filters/lang/FilterParser.hpp"
#include "controllers/filters/lang/Tokenizer.hpp"
#include "controllers/filters/lang/Types.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "lib/LegacyFilter.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Channel.hpp"
#include "mocks/ChatterinoBadges.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/EmptyApplication.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "Test.hpp"

#include <QColor>
#include <QVariant>

#include <random>
#include <set>

using namespace Qt::Literals;
using namespace chatterino;
using namespace chatterino::filters;
using chatterino::mock::MockChannel;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : highlights(this->settings, &this->accounts)
    {
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    IUserDataController *getUserData() override
    {
        return &this->userData;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    IChatterinoBadges *getChatterinoBadges() override
    {
        return &this->chatterinoBadges;
    }

    FfzBadges *getFfzBadges() override
    {
        return &this->ffzBadges;
    }

    BttvBadges *getBttvBadges() override
    {
        return &this->bttvBadges;
    }

    SeventvBadges *getSeventvBadges() override
    {
        return &this->seventvBadges;
    }

    HighlightController *getHighlights() override
    {
        return &this->highlights;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    TwitchBadges *getTwitchBadges() override
    {
        return &this->twitchBadges;
    }

    mock::EmptyLogging logging;
    AccountController accounts;
    mock::EmoteController emotes;
    mock::UserDataController userData;
    mock::MockTwitchIrcServer twitch;
    mock::ChatterinoBadges chatterinoBadges;
    FfzBadges ffzBadges;
    BttvBadges bttvBadges;
    SeventvBadges seventvBadges;
    HighlightController highlights;
    TwitchBadges twitchBadges;
};

class FiltersF : public ::testing::Test
{
protected:
    void SetUp() override
    {
        this->mockApplication = std::make_unique<MockApplication>();
    }

    void TearDown() override
    {
        this->mockApplication.reset();
    }

    std::unique_ptr<MockApplication> mockApplication;
};

}  // namespace

namespace chatterino::filters {

std::ostream &operator<<(std::ostream &os, Type t)
{
    os << typeToString(t);
    return os;
}

}  // namespace chatterino::filters

TEST(Filters, Validity)
{
    std::vector<std::pair<QString, bool>> tests{
        {"", false},
        {R".(1 + 1).", true},
        {R".(1 + (1==1)).", false},
        {R".((1==1) + 1).", false},
        {R".(1 + (1 + (1==1))).", false},
        {R".(1 % "").", false},
        {R".("" % 1).", false},
        {R".(1 - "").", false},
        {R".("" - 1).", false},
        {R".(1 * "").", false},
        {R".("" * 1).", false},
        {R".(1 / "").", false},
        {R".("" / 1).", false},
        {R".("" || (1==1)).", false},
        {R".((1!=1) && 1).", false},
        {R".(1 < "").", false},
        {R".("" > 1).", false},
        {R".("" >= 1).", false},
        {R".(author.badges <= 1).", false},
        {R".(1 + ).", false},
        {R".(1 + 1)).", false},
        {R".((1 + 1).", false},
        {R".(author.name contains "icelys").", true},
        {R".(author.color == "#ff0000").", true},
        {R".(author.name - 5).", false},  // can't perform String - Int
        {R".(message.content match {r"(\d\d)/(\d\d)/(\d\d\d\d)", 3}).", true},
        {R".("abc" + 123 == "abc123").", true},
        {R".(123 + "abc" == "hello").", false},
        {R".(flags.reply && flags.automod).", true},
        {R".(unknown.identifier).", false},
        {R".(10 startswith 1).", false},
        {R".(10 startswith "").", false},
        {R".("10" endswith 1).", false},
        {R".(1 contains "").", false},
        {R".("10" contains 1).", false},
        {R".((1+"") contains 1).", false},
        {R".("10" match 1).", false},
        {R".(1 match r"1").", false},
        {
            R".(channel.name == "forsen" && author.badges contains "moderator").",
            true,
        },
        {R".({(1+""), 2}).", false},
        {R".("abc" match {ri"foo", "bar"}).", false},
        {R".(!{}).", false},
        {R".(!(1+"")).", false},
        {R".({).", false},
        {R".({,).", false},
        {R".({1!).", false},
        {R".((1) "").", false},
        {R".(().", false},
        {R".()").", false},
        {R".((1()").", false},
        {R".("foo).", false},
        {R".(foo").", false},
    };

    for (const auto &[input, expected] : tests)
    {
        auto filterResult = Filter::fromString(input);
        bool isValid = std::holds_alternative<Filter>(filterResult);
        EXPECT_EQ(isValid, expected)
            << "Filter::fromString( " << input << " ) should be "
            << (expected ? "valid" : "invalid");
    }
}

TEST(Filters, TypeSynthesis)
{
    using T = Type;
    struct TestCase {
        QString input;
        T type;
    };

    // clang-format off
    std::vector<TestCase> tests
    {
        {R".(1 + 1).", T::Int},
        {R".(author.color).", T::Color},
        {R".(author.name).", T::String},
        {R".(!author.subbed).", T::Bool},
        {R".(author.badges).", T::StringList},
        {R".(channel.name == "forsen" && author.badges contains "moderator").", T::Bool},
        {R".(message.content match {r"(\d\d)/(\d\d)/(\d\d\d\d)", 3}).", T::String},
    };
    // clang-format on

    for (const auto &[input, expected] : tests)
    {
        auto filterResult = Filter::fromString(input);
        bool isValid = std::holds_alternative<Filter>(filterResult);
        ASSERT_TRUE(isValid)
            << "Filter::fromString( " << input << " ) is invalid";

        auto filter = std::move(std::get<Filter>(filterResult));
        T type = filter.returnType();
        EXPECT_EQ(type, expected)
            << "Filter{ " << input << " } has type " << type << " instead of "
            << expected << ".\nDebug: " << filter.debugString();
    }
}

TEST(Filters, Evaluation)
{
    Message message;
    message.displayName = "icelys";
    message.usernameColor = QColor(0xff0000);
    message.messageText = "hey there :) 2038-01-19 123 456";
    message.channelName = "forsen";
    message.twitchBadges = {
        TwitchBadge("moderator", ""),
        TwitchBadge("staff", ""),
    };
    message.externalBadges = {"frankerfacez:bot"};
    RunContext ctx{
        .message = message,
        .channel = nullptr,
    };

    std::vector<std::pair<QString, QVariant>> tests{
        // Evaluation semantics
        {R".(1 + 1).", QVariant(2)},
        {R".(!(1 == 1)).", QVariant(false)},
        {R".(2 + 3 * 4).",
         QVariant(20)},  // math operators have the same precedence
        {R".(1 > 2 || 3 >= 3).", QVariant(true)},
        {R".(1 > 2 && 3 > 1).", QVariant(false)},
        {R".(1 > 0 && 3 > 1).", QVariant(true)},
        {R".(0 <= 0 && 3 < 1).", QVariant(false)},
        {R".("abc" + 123).", QVariant("abc123")},
        {R".("abc" + "456").", QVariant("abc456")},
        {R".(3 - 4).", QVariant(-1)},
        {R".(3 * 4).", QVariant(12)},
        {R".(8 / 3).", QVariant(2)},
        {R".(7 % 3).", QVariant(1)},
        {R".(5 == 5).", QVariant(true)},
        {R".(5 == "5").", QVariant(true)},
        {R".(5 != 7).", QVariant(true)},
        {R".(5 == "abc").", QVariant(false)},
        // String comparison is case-insensitive
        {R".("ABC123" == "abc123").", QVariant(true)},
        {R".("ABC123" != "abc123").", QVariant(false)},
        {R".("Hello world" contains "Hello").", QVariant(true)},
        {R".("Hello world" contains "LLO W").",
         QVariant(true)},  // Case-insensitive
        {R".({"abc", "def"} contains "abc").", QVariant(true)},
        {R".({"abc", "def"} contains "ABC").",
         QVariant(true)},  // Case-insensitive when list is all strings
        {R".({123, "def"} contains "DEF").",
         QVariant(false)},  // Case-sensitive if list not all strings
        {R".({"a123", "b456"} startswith "a123").", QVariant(true)},
        {R".({"a123", "b456"} startswith "A123").", QVariant(true)},
        {R".({"a123", 1} startswith "A123").", QVariant(false)},
        {R".({"a123", 1} startswith "a123").", QVariant(true)},
        {R".({} startswith "A123").", QVariant(false)},
        {R".("Hello world" startswith "Hello").", QVariant(true)},
        {R".("Hello world" startswith "world").", QVariant(false)},
        {R".({"a123", "b456"} endswith "b456").", QVariant(true)},
        {R".({"a123", "b456"} endswith "B456").", QVariant(true)},
        {R".("Hello world" endswith "world").", QVariant(true)},
        {R".("Hello world" endswith "Hello").", QVariant(false)},
        // Context map usage
        {R".(author.name).", QVariant("icelys")},
        {R".(!author.subbed).", QVariant(true)},
        {R".(author.color == "#ff0000").", QVariant(true)},
        {R".(channel.name == "forsen" && author.badges contains "moderator").",
         QVariant(true)},
        {R".(author.external_badges contains "frankerfacez:bot").",
         QVariant(true)},
        {R".(message.content match {r"(\d\d\d\d)\-(\d\d)\-(\d\d)", 3}).",
         QVariant("19")},
        {R".(message.content match {r"forsen", 3}).", QVariant("")},
        {R".(message.content match r"HEY THERE").", QVariant(false)},
        {R".(message.content match ri"HEY THERE").", QVariant(true)},
    };

    for (const auto &[input, expected] : tests)
    {
        auto filterResult = Filter::fromString(input);
        bool isValid = std::holds_alternative<Filter>(filterResult);
        ASSERT_TRUE(isValid)
            << "Filter::fromString( " << input << " ) is invalid";

        auto filter = std::move(std::get<Filter>(filterResult));
        auto result = filter.execute(ctx);

        EXPECT_EQ(result, expected)
            << "Filter{ " << input << " } evaluated to " << result.toString()
            << " instead of " << expected.toString()
            << ".\nDebug: " << filter.debugString();
    }
}

TEST(Filters, Identifier)
{
    for (const auto [identifier, _] : VALID_IDENTIFIERS_MAP.asKeyValueRange())
    {
        auto expr = createIdentifierExpression(identifier);
        ASSERT_TRUE(isWellTyped(expr->synthesizeType()))
            << "the identifier '" << identifier
            << "' must create a well typed expression";
    }
}

TEST_F(FiltersF, ExpressionDebug)
{
    struct TestCase {
        QString input;
        QString debugString;
        QString filterString;
    };

    // clang-format off
    std::vector<TestCase> tests{
        {
            .input = R".(1 + 1).",
            .debugString = "BinaryOp[Plus](Val(1) : Int, Val(1) : Int)",
            .filterString = "(1 + 1)",
        },
        {
            .input = R".(author.color == "#ff0000").",
            .debugString = "BinaryOp[Eq](Accessor(author.color) : Color, Val(#ff0000) : String)",
            .filterString = R".((author.color == "#ff0000")).",
        },
        {
            .input = R".(1).",
            .debugString = "Val(1)",
            .filterString = R".(1).",
        },
        {
            .input = R".("asd").",
            .debugString = R".(Val(asd)).",
            .filterString = R".("asd").",
        },
        {
            .input = R".(("asd")).",
            .debugString = R".(Val(asd)).",
            .filterString = R".("asd").",
        },
        {
            .input = R".(author.subbed).",
            .debugString = R".(Accessor(author.subbed)).",
            .filterString = R".(author.subbed).",
        },
        {
            .input = R".(!author.subbed).",
            .debugString = R".(UnaryOp[Not](Accessor(author.subbed) : Bool)).",
            .filterString = R".((!author.subbed)).",
        },
        {
            .input = R".({"foo", "bar"} contains "foo").",
            .debugString = R".(BinaryOp[Contains](List(Val(foo) : String, Val(bar) : String) : StringList, Val(foo) : String)).",
            .filterString = R".(({"foo", "bar"} contains "foo")).",
        },
        {
            .input = R".(!({"foo", "bar"} contains "foo")).",
            .debugString = R".(UnaryOp[Not](BinaryOp[Contains](List(Val(foo) : String, Val(bar) : String) : StringList, Val(foo) : String) : Bool)).",
            .filterString = R".((!({"foo", "bar"} contains "foo"))).",
        },
        {
            .input = R".(message.content match r"(\d\d)/(\d\d)/(\d\d\d\d)").",
            .debugString = R".(BinaryOp[Match](Accessor(message.content) : String, RegEx((\d\d)/(\d\d)/(\d\d\d\d)) : RegularExpression)).",
            .filterString = R".((message.content match r"(\d\d)/(\d\d)/(\d\d\d\d)")).",
        },
    };
    // clang-format on

    for (const auto &[input, debugString, filterString] : tests)
    {
        const auto filterResult = Filter::fromString(input);
        const auto *filter = std::get_if<Filter>(&filterResult);
        EXPECT_NE(filter, nullptr) << "Filter::fromString(" << input
                                   << ") did not build a proper filter";

        const auto actualDebugString = filter->debugString();
        EXPECT_EQ(actualDebugString, debugString)
            << "filter->debugString() on '" << input << "' should be '"
            << debugString << "', but got '" << actualDebugString << "'";

        const auto actualFilterString = filter->filterString();
        EXPECT_EQ(actualFilterString, filterString)
            << "filter->filterString() on '" << input << "' should be '"
            << filterString << "', but got '" << actualFilterString << "'";
    }
}

namespace {

/// Returns `value` and counts how often it was evaluated.
class CountingExpression : public Expression
{
public:
    CountingExpression(QVariant value, int &count, int cost = 1)
        : value_(std::move(value))
        , count_(count)
        , cost_(cost)
    {
    }

    QVariant execute(RunContext /* context */) const override
    {
        this->count_++;
        return this->value_;
    }

    PossibleType synthesizeType() const override
    {
        if (variantIs(this->value_, QMetaType::QString))
        {
            return TypeClass{Type::String};
        }
        return TypeClass{Type::Bool};
    }

    QString debug() const override
    {
        return "Counting";
    }

    QString filterString() const override
    {
        return "counting";
    }

    int cost() const override
    {
        return this->cost_;
    }

    bool alwaysConvertsToBool() const override
    {
        return QVariant(this->value_).convert(QMetaType(QMetaType::Bool));
    }

private:
    QVariant value_;
    int &count_;
    int cost_;
};

ExpressionPtr counting(QVariant value, int &count, int cost = 1)
{
    return std::make_unique<CountingExpression>(std::move(value), count, cost);
}

ExpressionPtr binary(TokenType op, ExpressionPtr left, ExpressionPtr right)
{
    return std::make_unique<BinaryOperation>(op, std::move(left),
                                             std::move(right));
}

ExpressionPtr regex(const QString &pattern)
{
    return std::make_unique<RegexExpression>(pattern, false);
}

std::vector<std::shared_ptr<Message>> differentialMessages()
{
    struct Spec {
        QString name;
        QString text;
        std::vector<std::pair<QString, QString>> badges;
        MessageFlags flags;
        QColor color;
        uint32_t bits = 0;
    };
    std::vector<Spec> specs{
        {"streamer123", "!so someone", {{"broadcaster", "1"}}, {}, Qt::red},
        {"streamer123", "Thanks for the follow, user!", {}, {}, QColor()},
        {"StreamER123", "Current song: something", {}, {}, Qt::blue},
        {"viewer", "hello streamer123 how are you", {{"subscriber", "12"}}},
        {"viewer", "", {}, MessageFlags{MessageFlag::System}},
        {"mod_person",
         "follow me on twitter",
         {{"moderator", "1"}, {"subscriber", "3"}},
         MessageFlags{MessageFlag::FirstMessage},
         QColor(0xff0000)},
        {"someone", "0", {}, MessageFlags{MessageFlag::Action}, {}, 100},
        {"someone", "false", {{"founder", "0"}}},
        {"ÄÖÜ", "ßtraße ÄÖÜ äöü ǅ ﬀ", {}, MessageFlags{MessageFlag::Whisper}},
        {"kappa",
         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab",
         {{"vip", "1"}},
         MessageFlags{MessageFlag::Highlighted, MessageFlag::Subscription}},
        {"bot", "abc\"def 2038-01-19 123 456 HEY THERE", {}, {}, {}, 1},
        {"x", "a.b (c) [d] {e} \\ | ^ $ * + ?", {}, {}, Qt::green},
        // Kelvin sign, long s, dotted I, sigmas, Deseret, capital sharp s,
        // micro sign, a lone surrogate
        {"K", "K ſ İ Σσς \U00010400\U00010428 "
              "ẞ µ ǄǅǆĲ"},
        {"lone", QString("ab") + QChar(0xD801) + "cd" + QChar(0xDC28)},
    };

    std::vector<std::shared_ptr<Message>> messages;
    for (const auto &spec : specs)
    {
        auto message = std::make_shared<Message>();
        message->displayName = spec.name;
        message->loginName = spec.name.toLower();
        message->userID = QString::number(spec.name.size() * 1234);
        message->messageText = spec.text;
        message->channelName = "forsen";
        for (const auto &[key, value] : spec.badges)
        {
            message->twitchBadges.emplace_back(key, value);
            message->twitchBadgeInfos[key] = value;
        }
        message->flags = spec.flags;
        message->usernameColor = spec.color;
        message->bits = spec.bits;
        if (spec.bits != 0)
        {
            message->externalBadges = {"frankerfacez:bot"};
        }
        messages.push_back(std::move(message));
    }
    return messages;
}

/// Generates random filters, mostly well-typed ones shaped like the ones
/// people write, and some ill-typed ones.
class FilterGenerator
{
public:
    explicit FilterGenerator(uint32_t seed)
        : rng_(seed)
    {
    }

    QString filter()
    {
        if (this->chance(0.2))
        {
            return this->anyExpr(3);
        }
        return this->boolExpr(4);
    }

private:
    bool chance(double p)
    {
        return std::bernoulli_distribution(p)(this->rng_);
    }

    const QString &pick(const std::vector<QString> &items)
    {
        return items[std::uniform_int_distribution<size_t>(
            0, items.size() - 1)(this->rng_)];
    }

    int number(int lo, int hi)
    {
        return std::uniform_int_distribution<int>(lo, hi)(this->rng_);
    }

    QString stringLiteral()
    {
        static const std::vector<QString> strings{
            R"("streamer123")",
            R"("")",
            R"("Thanks for the follow")",
            R"("moderator")",
            R"("subscriber")",
            R"("!")",
            R"("0")",
            R"("false")",
            R"("true")",
            R"("äöü")",
            R"("ÄÖÜ")",
            R"("STRASSE")",
            R"("ß")",
            R"("#ff0000")",
            R"("abc\"def")",
            R"("follow")",
            R"("forsen")",
            R"("12")",
            // Case folding edge cases
            R"("k")",
            R"("K")",
            R"("s")",
            R"("ss")",
            R"("σ")",
            R"("ς")",
            R"("Σ")",
            R"("i")",
            R"("İ")",
            R"("ı")",
            R"("ǆ")",
            R"("ǅ")",
            R"("𐐨")",
            R"("𐐀")",
            R"("ẞ")",
            R"("ff")",
            R"("µ")",
            R"("μ")",
        };
        return pick(strings);
    }

    QString regexLiteral()
    {
        static const std::vector<QString> patterns{
            R"(^(!\w+|Thanks for the follow|Current song:))",
            R"(follow)",
            R"(^!)",
            R"((\w)\1)",
            R"(\Qa.b)",
            R"((?i)hello)",
            R"((?x)a # comment)",
            R"(()",
            R"([)",
            R"()",
            R"(\d{3})",
            R"((*COMMIT)a|b)",
            R"((?<name>x))",
            R"(a|b)",
            R"(^$)",
            R"(\bkappa\b)",
            R"((?=.*a)e)",
            R"(STREAMER)",
            R"(ä)",
            R"((a+)+b)",
            R"(\\)",
            R"([(*])",
            R"((?s:.)$)",
            R"(\x{e4})",
        };
        return (this->chance(0.3) ? u"ri\""_s : u"r\""_s) % pick(patterns) %
               u'"';
    }

    QString stringOperand()
    {
        static const std::vector<QString> identifiers{
            "author.name",     "author.user_id",  "channel.name",
            "message.content", "message.content", "message.content",
            "reward.id",       "reward.title",
        };
        if (this->chance(0.2))
        {
            return this->stringLiteral();
        }
        return pick(identifiers);
    }

    QString intOperand()
    {
        static const std::vector<QString> identifiers{
            "author.sub_length",
            "bits.amount",
            "message.length",
            "reward.cost",
        };
        if (this->chance(0.4))
        {
            return QString::number(this->number(-3, 60));
        }
        if (this->chance(0.2))
        {
            // Dividing by zero crashes, so only divide by literals
            static const std::vector<QString> ops{"+", "-", "*", "/", "%"};
            const auto &op = pick(ops);
            auto rhs = (op == "/" || op == "%")
                           ? QString::number(this->number(1, 5))
                           : this->intOperand();
            return u'(' % pick(identifiers) % u' ' % op % u' ' % rhs % u')';
        }
        return pick(identifiers);
    }

    QString listLiteral()
    {
        QStringList items;
        auto n = this->number(0, 3);
        for (int i = 0; i < n; i++)
        {
            items.append(this->chance(0.8)
                             ? this->stringLiteral()
                             : QString::number(this->number(0, 9)));
        }
        return u'{' % items.join(", ") % u'}';
    }

    QString boolLeaf()
    {
        static const std::vector<QString> identifiers{
            "author.subbed",        "author.no_color",
            "channel.live",         "channel.watching",
            "flags.action",         "flags.highlighted",
            "flags.system_message", "flags.first_message",
            "flags.sub_message",    "flags.whisper",
            "flags.reply",          "flags.elevated_message",
        };
        switch (this->number(0, 7))
        {
            case 0:
                return pick(identifiers);
            case 1: {
                static const std::vector<QString> ops{"==", "!="};
                return this->stringOperand() % u' ' % pick(ops) % u' ' %
                       this->stringOperand();
            }
            case 2: {
                static const std::vector<QString> ops{
                    "<", ">", "<=", ">=", "==", "!="};
                return this->intOperand() % u' ' % pick(ops) % u' ' %
                       this->intOperand();
            }
            case 3: {
                static const std::vector<QString> ops{"contains", "startswith",
                                                      "endswith"};
                return this->stringOperand() % u' ' % pick(ops) % u' ' %
                       this->stringOperand();
            }
            case 4: {
                static const std::vector<QString> lists{
                    "author.badges", "author.external_badges"};
                QString list =
                    this->chance(0.7) ? pick(lists) : this->listLiteral();
                static const std::vector<QString> ops{"contains", "startswith",
                                                      "endswith"};
                return list % u' ' % pick(ops) % u' ' % this->stringLiteral();
            }
            case 5:
                return u"author.color == "_s % (this->chance(0.5)
                                                    ? u"\"#ff0000\""_s
                                                    : u"\"#00ff00\""_s);
            default:
                return this->stringOperand() % u" match "_s %
                       this->regexLiteral();
        }
    }

    QString boolExpr(int depth)
    {
        if (depth <= 0 || this->chance(0.3))
        {
            return this->boolLeaf();
        }
        if (this->chance(0.15))
        {
            return u"!("_s % this->boolExpr(depth - 1) % u')';
        }

        // A chain like `a && b && c`, sometimes mixing && and ||
        auto n = this->number(2, 6);
        bool mixed = this->chance(0.2);
        QString op = this->chance(0.5) ? "&&" : "||";
        QString out = this->boolExpr(depth - 1);
        for (int i = 1; i < n; i++)
        {
            if (mixed)
            {
                op = this->chance(0.5) ? "&&" : "||";
            }
            out += u' ' % op % u' ' % this->boolExpr(depth - 1);
        }
        return u'(' % out % u')';
    }

    QString anyLeaf()
    {
        static const std::vector<QString> identifiers{
            "author.badges", "author.color",   "author.name",
            "author.subbed", "message.length", "unknown.identifier",
        };
        switch (this->number(0, 5))
        {
            case 0:
                return pick(identifiers);
            case 1:
                return this->stringLiteral();
            case 2:
                return QString::number(this->number(-2, 3));
            case 3:
                return this->regexLiteral();
            case 4:
                return this->listLiteral();
            default:
                return u'{' % this->regexLiteral() % u", " %
                       QString::number(this->number(0, 2)) % u'}';
        }
    }

    QString anyExpr(int depth)
    {
        if (depth <= 0 || this->chance(0.3))
        {
            return this->chance(0.5) ? this->anyLeaf() : this->boolLeaf();
        }
        static const std::vector<QString> ops{
            "&&", "||",       "&&",         "||",       "==",
            "!=", "<",        ">",          "+",        "-",
            "*",  "contains", "startswith", "endswith", "match",
        };
        if (this->chance(0.1))
        {
            return u"!("_s % this->anyExpr(depth - 1) % u')';
        }
        return u'(' % this->anyExpr(depth - 1) % u' ' % pick(ops) % u' ' %
               this->anyExpr(depth - 1) % u')';
    }

    std::mt19937 rng_;
};

/// Evaluates `text` with the current evaluator, optimized with each of
/// `allOptions`, and the legacy one on every message, regardless of whether
/// it type checks, and reports differences.
void expectSameAsLegacy(const QString &text,
                        const std::vector<OptimizeOptions> &allOptions,
                        const std::vector<std::shared_ptr<Message>> &messages,
                        Channel *channel, int &failures)
{
    legacy::FilterParser legacyParser(text);
    auto legacyExpression = legacyParser.release();
    FilterParser parser(text);
    auto expression = parser.release();
    ASSERT_EQ(parser.valid(), legacyParser.valid()) << text;

    std::vector<QVariant> expected;
    for (const auto &message : messages)
    {
        expected.push_back(legacyExpression->execute({
            .message = *message,
            .channel = channel,
        }));
    }

    // optimize() can be called again with different options
    for (const auto &options : allOptions)
    {
        expression->optimize(options);
        for (size_t i = 0; i < messages.size(); i++)
        {
            const auto &message = messages[i];
            auto actual = expression->execute({
                .message = *message,
                .channel = channel,
            });

            bool same = expected[i].metaType() == actual.metaType() &&
                        expected[i] == actual;
            bool claimHolds =
                !expression->alwaysConvertsToBool() ||
                QVariant(actual).convert(QMetaType(QMetaType::Bool));
            if (same && claimHolds)
            {
                continue;
            }

            failures++;
            if (failures > 20)
            {
                continue;
            }
            EXPECT_TRUE(same)
                << "Filter " << text << " on message '" << message->messageText
                << "' from " << message->displayName << " evaluated to "
                << actual.toString() << " (" << actual.metaType().name()
                << ") instead of " << expected[i].toString() << " ("
                << expected[i].metaType().name()
                << ")\nDebug: " << expression->debug()
                << "\nreorder: " << options.reorder
                << ", mergeRegexes: " << options.mergeRegexes
                << ", foldContains: " << options.foldContains;
            EXPECT_TRUE(claimHolds)
                << "Filter " << text
                << " claims to always convert to a Bool, but returned "
                << actual.metaType().name();
        }
    }
}

std::vector<OptimizeOptions> allOptions()
{
    std::vector<OptimizeOptions> out;
    for (int i = 0; i < 8; i++)
    {
        out.push_back({
            .reorder = (i & 1) != 0,
            .mergeRegexes = (i & 2) != 0,
            .foldContains = (i & 4) != 0,
        });
    }
    return out;
}

const std::vector<OptimizeOptions> ALL_OPTIONS = allOptions();

}  // namespace

TEST(Filters, ShortCircuit)
{
    Message message;
    RunContext ctx{
        .message = message,
        .channel = nullptr,
    };

    struct TestCase {
        TokenType op;
        QVariant left;
        QVariant right;
        bool rightEvaluated;
    };
    std::vector<TestCase> tests{
        {AND, false, true, false},
        {AND, true, true, true},
        {AND, true, false, true},
        {AND, QStringList{}, true, false},
        {AND, "", true, false},
        {AND, "0", true, false},
        {AND, "yes", true, true},
        {OR, true, false, false},
        {OR, true, "", false},
        {OR, false, true, true},
        {OR, QStringList{}, true, false},
        // The right side has to be checked, it might not convert to a Bool
        {OR, true, QStringList{"a"}, true},
        {OR, false, QStringList{"a"}, true},
        {OR, true, QColor(Qt::red), true},
    };

    for (const auto &test : tests)
    {
        for (const auto &options : ALL_OPTIONS)
        {
            int leftCount = 0;
            int rightCount = 0;
            auto expr = binary(test.op, counting(test.left, leftCount),
                               counting(test.right, rightCount));
            expr->optimize(options);

            QVariant left = test.left;
            QVariant right = test.right;
            bool converted = convertVariantTypes(left, right, QMetaType::Bool);
            bool expected =
                converted && (test.op == AND ? left.toBool() && right.toBool()
                                             : left.toBool() || right.toBool());

            auto result = expr->execute(ctx);
            EXPECT_EQ(result, QVariant(expected))
                << tokenTypeToInfoString(test.op) << ' ' << test.left.toString()
                << ' ' << test.right.toString();
            EXPECT_EQ(leftCount, 1);
            EXPECT_EQ(rightCount, test.rightEvaluated ? 1 : 0)
                << tokenTypeToInfoString(test.op) << ' ' << test.left.toString()
                << ' ' << test.right.toString();
        }
    }
}

TEST(Filters, ReorderByCost)
{
    Message message;
    RunContext ctx{
        .message = message,
        .channel = nullptr,
    };

    for (auto op : {AND, OR})
    {
        for (bool reorder : {false, true})
        {
            int expensive = 0;
            int cheap = 0;
            int other = 0;
            // expensive && (other && cheap), with cheap deciding the result
            auto expr = binary(op, counting(op == AND, expensive, 1000),
                               binary(op, counting(op == AND, other, 50),
                                      counting(op == OR, cheap, 1)));
            expr->optimize({.reorder = reorder});

            EXPECT_EQ(expr->execute(ctx), QVariant(op == OR));
            EXPECT_EQ(cheap, 1);
            EXPECT_EQ(expensive, reorder ? 0 : 1);
        }
    }
}

TEST(Filters, MergeRegexes)
{
    Message message;
    RunContext ctx{
        .message = message,
        .channel = nullptr,
    };
    const QString subject = "Thanks for the follow";

    struct TestCase {
        QString first;
        QString second;
        bool merged;
    };
    std::vector<TestCase> tests{
        {"^!\\w+", "follow", true},
        {"^!\\w+", "^Current song:", true},
        {"a|^Thanks", "x", true},
        {"(?i)THANKS", "x", true},
        {"(?i:THANKS)", "x", true},
        {"(?=.*f)T", "x", true},
        {"(T)\\1", "x", false},
        {"\\Qfollow", "x", false},
        {"(?x)follow # comment", "x", false},
        {"(*COMMIT)T", "x", false},
        {"(?<name>T)", "x", false},
        {"(", "follow", false},
    };

    for (const auto &test : tests)
    {
        bool firstMatches =
            QRegularExpression(test.first).match(subject).hasMatch();
        bool secondMatches =
            QRegularExpression(test.second).match(subject).hasMatch();

        for (bool merge : {false, true})
        {
            int count = 0;
            auto expr = binary(
                OR, binary(MATCH, counting(subject, count), regex(test.first)),
                binary(MATCH, counting(subject, count), regex(test.second)));
            expr->optimize({.reorder = false, .mergeRegexes = merge});

            EXPECT_EQ(expr->execute(ctx),
                      QVariant(firstMatches || secondMatches))
                << test.first << " || " << test.second;

            // A merged pattern evaluates the subject once. Otherwise, a
            // matching first pattern skips the second one.
            int expectedCount = (merge && test.merged) || firstMatches ? 1 : 2;
            EXPECT_EQ(count, expectedCount)
                << test.first << " || " << test.second << " merge: " << merge;
        }
    }
}

TEST_F(FiltersF, MatchesLegacyEvaluation)
{
    auto messages = differentialMessages();
    auto channel = std::make_shared<MockChannel>("forsen");

    std::vector<QString> filters{
        R".(author.name == "streamer123" && !(message.content match r"^(!\w+|Thanks for the follow|Current song:)")).",
        R".(author.name == "streamer123" && !(message.content match r"^!" || message.content match r"follow" || message.content contains "Current song")).",
        R".(message.content match r"a" || message.content match ri"FOLLOW" || message.content match r"(\w)\1").",
        R".(author.badges || flags.action).",
        R".(flags.action || author.badges).",
        R".(flags.action || author.color).",
        R".(flags.action && author.badges).",
        R".(author.badges && flags.action).",
        R".("abc" && 1).",
        R".("" || 0).",
        R".(1 || "false").",
        R".(r"a" || flags.action).",
        R".(flags.action || r"a").",
        R".({} || flags.action).",
        R".(unknown.identifier || flags.action).",
        R".(flags.action || flags.whisper || author.badges || flags.system_message).",
        R".((author.badges || flags.action) || flags.system_message).",
        R".(flags.system_message || (author.badges || flags.action)).",
        R".(message.content match {r"(\d+)", 1} || flags.action).",
        R".(message.content || flags.action).",
        R".(message.length && message.content).",
        R".(message.content match r"(" || message.content match r"follow").",
        R".(message.content match r"follow" || author.name match r"follow").",
        R".(message.content match r"follow" || message.content match ri"FOLLOW").",
        R".(!(flags.action || author.badges)).",
        R".(author.name == "streamer123" && ).",
        R".(&& flags.action).",
        R".(flags.action ||).",
        R".(message.content contains "a" || message.content contains "ẞ" || message.content contains "").",
        R".(message.content contains "k" || author.name contains "k" || message.content contains "σ").",
        R".(author.badges contains "moderator" || author.badges contains "vip").",
        R".(message.content contains "𐐨" || message.content contains "ss" || flags.action).",
    };

    FilterGenerator generator(20261102);
    for (int i = 0; i < 10000; i++)
    {
        filters.push_back(generator.filter());
    }

    int failures = 0;
    for (const auto &filter : filters)
    {
        expectSameAsLegacy(filter, ALL_OPTIONS, messages, channel.get(),
                           failures);
    }
    EXPECT_EQ(failures, 0);
}

TEST(Filters, GeneratedFiltersAreMostlyValid)
{
    // The differential test is only meaningful if most generated filters
    // would be accepted by the filter editor
    FilterGenerator generator(42);
    int valid = 0;
    const int total = 1000;
    for (int i = 0; i < total; i++)
    {
        auto result = Filter::fromString(generator.filter());
        if (std::holds_alternative<Filter>(result))
        {
            valid++;
        }
    }
    EXPECT_GT(valid, total / 2);
}

TEST(Filters, FoldedContainsMatchesLegacy)
{
    // Searching a case folded subject must find exactly what a
    // case-insensitive search finds. Check every cased code point against the
    // characters it's related to.
    Message message;
    RunContext ctx{
        .message = message,
        .channel = nullptr,
    };

    auto value = [](const QString &s) {
        return std::make_unique<ValueExpression>(s, TokenType::STRING);
    };
    auto evaluate = [&](const QString &haystack, const QString &needle,
                        bool optimized) {
        if (optimized)
        {
            // Only `||` chains of several literals search a folded subject;
            // the second literal never matches
            BinaryOperation op(
                OR,
                std::make_unique<BinaryOperation>(CONTAINS, value(haystack),
                                                  value(needle)),
                std::make_unique<BinaryOperation>(CONTAINS, value(haystack),
                                                  value(u"\x01"_s)));
            op.optimize({});
            return op.execute(ctx);
        }
        legacy::BinaryOperation op(CONTAINS, value(haystack), value(needle));
        return op.execute(ctx);
    };

    int checked = 0;
    int failures = 0;
    for (char32_t c = 0; c <= 0x10FFFF; c++)
    {
        if (QChar::isSurrogate(c))
        {
            continue;
        }
        std::set<char32_t> related{
            QChar::toLower(c),
            QChar::toUpper(c),
            QChar::toTitleCase(c),
            QChar::toCaseFolded(c),
            QChar::toCaseFolded(QChar::toUpper(c)),
        };
        related.erase(c);
        if (related.empty())
        {
            continue;
        }
        related.insert(c);

        auto text = QString::fromUcs4(&c, 1);
        for (auto r : related)
        {
            auto other = QString::fromUcs4(&r, 1);
            using Case = std::pair<QString, QString>;
            for (const auto &[haystack, needle] : {
                     Case{u"a"_s % text % u"b", other},
                     Case{u"a"_s % other % u"b", text},
                     Case{text, other % u'x'},
                 })
            {
                checked++;
                auto expected = evaluate(haystack, needle, false);
                auto actual = evaluate(haystack, needle, true);
                if (expected != actual && ++failures <= 20)
                {
                    ADD_FAILURE()
                        << "U+" << std::hex << static_cast<uint32_t>(c)
                        << " vs U+" << static_cast<uint32_t>(r) << std::dec
                        << ": '" << haystack << "' contains '" << needle
                        << "' is " << expected.toBool() << " but was "
                        << actual.toBool();
                }
            }
        }
    }
    EXPECT_GT(checked, 5000);
    EXPECT_EQ(failures, 0);

    // Characters with a multi-character full case folding are compared as a
    // single character
    using Case = std::tuple<QString, QString, bool>;
    for (const auto &[haystack, needle, contains] : {
             Case{u"straße"_s, u"ss"_s, false},
             Case{u"STRASSE"_s, u"ß"_s, false},
             Case{u"straße"_s, u"ẞ"_s, true},
             Case{u"ǆ"_s, u"dž"_s, false},
             Case{u"ﬀ"_s, u"ff"_s, false},
             Case{u"İ"_s, u"i"_s, false},
         })
    {
        EXPECT_EQ(evaluate(haystack, needle, false).toBool(), contains)
            << haystack << " contains " << needle;
        EXPECT_EQ(evaluate(haystack, needle, true).toBool(), contains)
            << haystack << " contains " << needle;
    }
}
