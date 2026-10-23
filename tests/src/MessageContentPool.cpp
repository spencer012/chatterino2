// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/MessageContentPool.hpp"

#include "common/Literals.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "Test.hpp"

#include <QColor>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace chatterino;
using namespace chatterino::literals;

namespace {

bool sameData(const QStringList &a, const QStringList &b)
{
    return !a.isEmpty() && a.constData() == b.constData();
}

bool sameData(const QString &a, const QString &b)
{
    return !a.isEmpty() && a.constData() == b.constData();
}

EmotePtr makeEmote(const QString &name, const QString &id)
{
    return std::make_shared<const Emote>(Emote{
        .name = {name},
        .tooltip = {name + u" tooltip"_s},
        .id = {id},
    });
}

std::unique_ptr<TextElement> makeText(
    const QString &text, MessageColor color = MessageColor::Text,
    MessageElementFlags flags = MessageElementFlag::Text)
{
    return std::make_unique<TextElement>(text, flags, color);
}

/// Builds a chat-like message: username, body text and optional emote.
MessagePtrMut makeMessage(const QString &id, const QString &login,
                          const QString &body, const EmotePtr &emote,
                          const QString &tail)
{
    auto msg = std::make_shared<Message>();
    msg->id = id;
    msg->loginName = login;
    msg->displayName = login;
    msg->serverReceivedTime =
        QDateTime::fromMSecsSinceEpoch(id.toLongLong() * 1000);
    msg->messageText = body + (emote ? u' ' + emote->name.string : QString{}) +
                       (tail.isEmpty() ? QString{} : u' ' + tail);
    msg->elements.push_back(makeText(login + u':',
                                     MessageColor(QColor(Qt::red)),
                                     MessageElementFlag::Username));
    msg->elements.push_back(makeText(body));
    if (emote)
    {
        msg->elements.push_back(
            std::make_unique<EmoteElement>(emote, MessageElementFlag::Emote));
    }
    if (!tail.isEmpty())
    {
        msg->elements.push_back(makeText(tail));
    }
    return msg;
}

/// Admits every payload on first sight, like a pool without the filter.
MessageContentPoolOptions eager(MessageContentPoolOptions options = {})
{
    options.admissionFilterSlots = 0;
    return options;
}

TextElement *textAt(const MessagePtrMut &msg, size_t i)
{
    return dynamic_cast<TextElement *>(msg->elements.at(i).get());
}

QStringList wordsOf(const std::unique_ptr<MessageElement> &element)
{
    return dynamic_cast<TextElement *>(element.get())->words();
}

}  // namespace

TEST(MessageContentPool, SharesEqualWordListsWithIndependentRendering)
{
    MessageContentPool pool(eager());

    auto a = makeText(u"hello there chat"_s, MessageColor::Text);
    auto b = makeText(u"hello there chat"_s, MessageColor::System,
                      {MessageElementFlag::Text, MessageElementFlag::Mention});
    b->setTrailingSpace(false);
    b->setLink({Link::Url, u"https://example.com"_s});
    ASSERT_FALSE(sameData(a->words(), b->words()));

    EXPECT_EQ(a->shareContent(pool), 1U);
    EXPECT_EQ(b->shareContent(pool), 1U);

    EXPECT_TRUE(sameData(a->words(), b->words()));
    EXPECT_TRUE(sameData(a->words().at(1), b->words().at(1)));
    EXPECT_TRUE(a->hasSharedWords());
    EXPECT_TRUE(b->hasSharedWords());

    // Rendering inputs stay per element.
    EXPECT_EQ(a->color().toString(),
              MessageColor(MessageColor::Text).toString());
    EXPECT_EQ(b->color().toString(),
              MessageColor(MessageColor::System).toString());
    EXPECT_TRUE(a->hasTrailingSpace());
    EXPECT_FALSE(b->hasTrailingSpace());
    EXPECT_EQ(a->getLink().type, Link::None);
    EXPECT_EQ(b->getLink().type, Link::Url);
    EXPECT_NE(a->getFlags(), b->getFlags());

    auto stats = pool.stats();
    EXPECT_EQ(stats.hits, 1U);
    EXPECT_EQ(stats.misses, 1U);
    EXPECT_EQ(stats.liveEntries, 1U);
    EXPECT_EQ(stats.liveParticipants, 2U);
    EXPECT_EQ(stats.passiveParticipants, 0U);
    EXPECT_GT(stats.retainedGrossSavedBytes, 0U);
    // Both lists were built the same way, so the released copy has the
    // same capacity as the retained one.
    EXPECT_EQ(stats.retainedGrossSavedBytes, stats.retainedPayloadBytes);
}

TEST(MessageContentPool, EmoteBearingMessagesKeepOccurrenceState)
{
    MessageContentPool pool(eager());
    auto kappa = makeEmote(u"Kappa"_s, u"25"_s);
    auto otherKappa = makeEmote(u"Kappa"_s, u"7tv-kappa"_s);

    auto first =
        makeMessage(u"1"_s, u"alice"_s, u"so true"_s, kappa, u"chat moment"_s);
    auto second =
        makeMessage(u"2"_s, u"bob"_s, u"so true"_s, kappa, u"chat moment"_s);
    // Same raw text, but the emote resolves to a different provider emote.
    auto third = makeMessage(u"3"_s, u"alice"_s, u"so true"_s, otherKappa,
                             u"chat moment"_s);
    first->flags.set(MessageFlag::Highlighted);
    second->replyParent = first;

    // username + body + tail + message text
    EXPECT_EQ(pool.shareMessageContent(*first), 4U);
    EXPECT_EQ(pool.shareMessageContent(*second), 4U);
    EXPECT_EQ(pool.shareMessageContent(*third), 4U);

    // Text runs and message text are shared across all three.
    EXPECT_TRUE(
        sameData(textAt(first, 1)->words(), textAt(second, 1)->words()));
    EXPECT_TRUE(sameData(textAt(first, 1)->words(), textAt(third, 1)->words()));
    EXPECT_TRUE(sameData(textAt(first, 3)->words(), textAt(third, 3)->words()));
    EXPECT_TRUE(sameData(first->messageText, third->messageText));

    // Usernames only share between the same sender.
    EXPECT_TRUE(sameData(textAt(first, 0)->words(), textAt(third, 0)->words()));
    EXPECT_FALSE(
        sameData(textAt(first, 0)->words(), textAt(second, 0)->words()));

    // Elements and emote resolution stay per occurrence.
    EXPECT_NE(first->elements.at(2).get(), second->elements.at(2).get());
    auto emoteOf = [](const MessagePtrMut &msg) {
        return dynamic_cast<EmoteElement *>(msg->elements.at(2).get())
            ->getEmote();
    };
    EXPECT_EQ(emoteOf(first), kappa);
    EXPECT_EQ(emoteOf(second), kappa);
    EXPECT_EQ(emoteOf(third), otherKappa);

    // Per-occurrence metadata is untouched.
    EXPECT_EQ(first->id, u"1"_s);
    EXPECT_EQ(second->id, u"2"_s);
    EXPECT_EQ(second->loginName, u"bob"_s);
    EXPECT_NE(first->serverReceivedTime, second->serverReceivedTime);
    EXPECT_TRUE(first->flags.has(MessageFlag::Highlighted));
    EXPECT_FALSE(second->flags.has(MessageFlag::Highlighted));
    EXPECT_EQ(second->replyParent, first);
    EXPECT_EQ(third->replyParent, nullptr);

    // Emote elements do not take part themselves.
    EXPECT_EQ(first->elements.at(2)->shareContent(pool), 0U);
}

TEST(MessageContentPool, CollisionsRequireFullEquality)
{
    MessageContentPool pool(eager({.forceHashCollisionsForTesting = true}));

    QStringList joined{u"a b"_s};
    QStringList split{u"a"_s, u"b"_s};
    QStringList concat{u"ab"_s};
    QStringList splitAgain{u"a"_s, u"b"_s};
    QStringList caseDiff{u"A"_s, u"b"_s};
    // Allocate independently: identical literals may share compiler storage
    // even when the pool correctly keeps strings and word lists separate.
    QString text = QString::fromUtf8("ab");
    MessageContentRef r1;
    MessageContentRef r2;
    MessageContentRef r3;
    MessageContentRef r4;
    MessageContentRef r5;
    MessageContentRef r6;

    EXPECT_TRUE(pool.shareWords(joined, r1));
    EXPECT_TRUE(pool.shareWords(split, r2));
    EXPECT_TRUE(pool.shareWords(concat, r3));
    EXPECT_TRUE(pool.shareWords(splitAgain, r4));
    EXPECT_TRUE(pool.shareWords(caseDiff, r5));
    // A string never matches a single-word list.
    EXPECT_TRUE(pool.shareText(text, r6));

    EXPECT_TRUE(sameData(split, splitAgain));
    EXPECT_FALSE(sameData(split, joined));
    EXPECT_FALSE(sameData(split, concat));
    EXPECT_FALSE(sameData(split, caseDiff));
    EXPECT_FALSE(sameData(text, concat.at(0)));
    EXPECT_EQ(joined, QStringList{u"a b"_s});
    EXPECT_EQ(caseDiff, (QStringList{u"A"_s, u"b"_s}));

    auto stats = pool.stats();
    EXPECT_EQ(stats.hits, 1U);
    EXPECT_EQ(stats.misses, 5U);
    EXPECT_EQ(stats.indexedEntries, 5U);
}

TEST(MessageContentPool, ContentOutlivesIndexEviction)
{
    MessageContentPool pool(eager({.maxIndexedEntries = 2}));

    auto a1 = makeText(u"first message"_s);
    auto a2 = makeText(u"first message"_s);
    a1->shareContent(pool);
    a2->shareContent(pool);
    ASSERT_TRUE(sameData(a1->words(), a2->words()));

    auto b = makeText(u"second"_s);
    auto c = makeText(u"third"_s);
    b->shareContent(pool);
    c->shareContent(pool);

    auto stats = pool.stats();
    EXPECT_EQ(stats.indexedEntries, 2U);
    EXPECT_EQ(stats.evictions, 1U);
    EXPECT_EQ(stats.liveEntries, 3U);
    EXPECT_EQ(stats.unindexedLiveEntries, 1U);
    EXPECT_EQ(stats.liveParticipants, 4U);

    // Evicted content is still valid and still shared by its participants.
    EXPECT_EQ(a1->words(), (QStringList{u"first"_s, u"message"_s}));
    EXPECT_TRUE(sameData(a1->words(), a2->words()));
    EXPECT_GT(stats.retainedGrossSavedBytes, 0U);

    // A new occurrence cannot find the evicted entry anymore.
    auto a3 = makeText(u"first message"_s);
    a3->shareContent(pool);
    EXPECT_FALSE(sameData(a1->words(), a3->words()));
    EXPECT_EQ(pool.stats().misses, 4U);

    a1.reset();
    a2.reset();
    a3.reset();
    b.reset();
    c.reset();

    stats = pool.stats();
    EXPECT_EQ(stats.liveEntries, 0U);
    EXPECT_EQ(stats.liveParticipants, 0U);
    EXPECT_EQ(stats.indexedEntries, 0U);
    EXPECT_EQ(stats.retainedPayloadBytes, 0U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);
    EXPECT_EQ(stats.estimatedOverheadBytes, 0U);
    // Only the (process-wide) tracking slots remain as a cost.
    EXPECT_EQ(stats.retainedNetSavedBytes,
              -static_cast<std::int64_t>(stats.trackingSlotBytes));
}

TEST(MessageContentPool, IndexIsBounded)
{
    MessageContentPool pool(eager({.maxIndexedEntries = 16}));
    std::vector<std::unique_ptr<TextElement>> elements;
    for (int i = 0; i < 100; i++)
    {
        elements.push_back(makeText(QString::number(i)));
        elements.back()->shareContent(pool);
    }

    auto stats = pool.stats();
    EXPECT_EQ(stats.indexedEntries, 16U);
    EXPECT_EQ(stats.evictions, 84U);
    EXPECT_EQ(stats.liveEntries, 100U);

    pool.setMaxIndexedEntries(4);
    EXPECT_EQ(pool.stats().indexedEntries, 4U);

    // The most recently used entries survive.
    auto recent = makeText(u"99"_s);
    recent->shareContent(pool);
    EXPECT_TRUE(sameData(recent->words(), elements.back()->words()));

    elements.clear();
    recent.reset();
    stats = pool.stats();
    EXPECT_EQ(stats.indexedEntries, 0U);
    EXPECT_EQ(stats.liveEntries, 0U);
}

TEST(MessageContentPool, StatisticsFollowParticipantLifetime)
{
    MessageContentPool pool(eager());

    std::vector<std::unique_ptr<TextElement>> elements;
    for (int i = 0; i < 3; i++)
    {
        elements.push_back(makeText(u"same words here"_s));
        elements.back()->shareContent(pool);
    }

    auto stats = pool.stats();
    const auto payload = stats.retainedPayloadBytes;
    ASSERT_GT(payload, 0);
    EXPECT_EQ(stats.liveParticipants, 3U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 2 * payload);
    EXPECT_EQ(stats.cumulativeDeduplicatedBytes, 2 * payload);
    EXPECT_GT(stats.estimatedOverheadBytes, 0U);
    EXPECT_EQ(stats.fixedOverheadBytes, 0U);
    EXPECT_GE(stats.trackingSlots, 3U);
    EXPECT_EQ(stats.trackingSlotBytes,
              stats.trackingSlots * sizeof(MessageContentRef));
    EXPECT_EQ(stats.retainedNetSavedBytes,
              static_cast<std::int64_t>(stats.retainedGrossSavedBytes) -
                  static_cast<std::int64_t>(stats.estimatedOverheadBytes) -
                  static_cast<std::int64_t>(stats.trackingSlotBytes));

    elements.pop_back();
    stats = pool.stats();
    EXPECT_EQ(stats.liveParticipants, 2U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, payload);
    // Cumulative values never decrease.
    EXPECT_EQ(stats.cumulativeDeduplicatedBytes, 2 * payload);

    elements.clear();
    stats = pool.stats();
    EXPECT_EQ(stats.liveParticipants, 0U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);
    EXPECT_EQ(stats.indexedEntries, 0U);
    EXPECT_EQ(stats.expired, 1U);
    EXPECT_EQ(stats.hits, 2U);

    // A single occurrence costs overhead without saving anything.
    auto single = makeText(u"unique"_s);
    single->shareContent(pool);
    EXPECT_LT(pool.stats().retainedNetSavedBytes, 0);
}

TEST(MessageContentPool, ToggleKeepsSharedContent)
{
    MessageContentPool pool(eager());

    auto a = makeText(u"toggle me"_s);
    auto b = makeText(u"toggle me"_s);
    a->shareContent(pool);
    b->shareContent(pool);

    pool.setEnabled(false);
    EXPECT_FALSE(pool.isEnabled());

    auto stats = pool.stats();
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(stats.indexedEntries, 0U);
    EXPECT_EQ(stats.liveParticipants, 2U);
    EXPECT_TRUE(sameData(a->words(), b->words()));

    auto c = makeText(u"toggle me"_s);
    EXPECT_EQ(c->shareContent(pool), 0U);
    EXPECT_FALSE(c->hasSharedWords());
    EXPECT_FALSE(sameData(a->words(), c->words()));
    EXPECT_EQ(pool.stats().skipped, 1U);

    a.reset();
    EXPECT_EQ(pool.stats().liveParticipants, 1U);
    EXPECT_EQ(b->words(), (QStringList{u"toggle"_s, u"me"_s}));

    pool.setEnabled(true);
    auto d = makeText(u"toggle me"_s);
    auto e = makeText(u"toggle me"_s);
    d->shareContent(pool);
    e->shareContent(pool);
    EXPECT_TRUE(sameData(d->words(), e->words()));
    // `b` was unindexed while disabled, so it is not found again.
    EXPECT_FALSE(sameData(b->words(), d->words()));
}

TEST(MessageContentPool, MutationAndCloneKeepAccounting)
{
    MessageContentPool pool(eager());

    auto a = makeText(u"base text"_s);
    auto b = makeText(u"base text"_s);
    a->shareContent(pool);
    b->shareContent(pool);
    const auto gross = pool.stats().retainedGrossSavedBytes;
    ASSERT_GT(gross, 0U);

    auto clone = a->clone();
    auto *clonedText = dynamic_cast<TextElement *>(clone.get());
    ASSERT_NE(clonedText, nullptr);
    EXPECT_TRUE(clonedText->hasSharedWords());
    EXPECT_TRUE(sameData(clonedText->words(), a->words()));
    auto stats = pool.stats();
    EXPECT_EQ(stats.liveParticipants, 3U);
    EXPECT_EQ(stats.passiveParticipants, 1U);
    // A clone shares its Qt payload without any pool: no extra savings.
    EXPECT_EQ(stats.retainedGrossSavedBytes, gross);

    // Mutating one occurrence detaches it without touching the others.
    b->appendText(u"extra"_s);
    EXPECT_FALSE(b->hasSharedWords());
    EXPECT_EQ(b->words(), (QStringList{u"base"_s, u"text"_s, u"extra"_s}));
    EXPECT_EQ(a->words(), (QStringList{u"base"_s, u"text"_s}));
    stats = pool.stats();
    EXPECT_EQ(stats.liveParticipants, 2U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);

    auto msg = makeMessage(u"1"_s, u"carol"_s, u"base text"_s, nullptr, {});
    pool.shareMessageContent(*msg);
    auto msgClone = msg->clone();
    EXPECT_TRUE(sameData(msgClone->messageText, msg->messageText));
    EXPECT_TRUE(msgClone->messageTextContent.isShared());
    EXPECT_TRUE(msgClone->messageTextContent.isPassive());
    EXPECT_FALSE(msg->messageTextContent.isPassive());
    EXPECT_EQ(msgClone->id, msg->id);
}

TEST(MessageContentPool, ClonesNeverReportSavingsAndKeepTracking)
{
    MessageContentPool pool(eager());

    auto original = makeText(u"clone source words"_s);
    original->shareContent(pool);

    std::vector<std::unique_ptr<MessageElement>> clones;
    for (int i = 0; i < 5; i++)
    {
        clones.push_back(original->clone());
    }
    // A clone of a clone is passive as well.
    clones.push_back(clones.back()->clone());

    auto stats = pool.stats();
    EXPECT_EQ(stats.liveEntries, 1U);
    EXPECT_EQ(stats.liveParticipants, 7U);
    EXPECT_EQ(stats.passiveParticipants, 6U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);
    EXPECT_EQ(stats.cumulativeDeduplicatedBytes, 0U);

    // Re-sharing a clone's (already shared) words is a hit that saves
    // nothing.
    MessageContentRef ref;
    QStringList alias = wordsOf(clones.front());
    EXPECT_TRUE(pool.shareWords(alias, ref));
    EXPECT_TRUE(ref.isPassive());
    stats = pool.stats();
    EXPECT_EQ(stats.hits, 1U);
    EXPECT_EQ(stats.hitsWithoutSaving, 1U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);
    ref.reset();

    // The original may die first; clones keep the entry alive and tracked.
    original.reset();
    stats = pool.stats();
    EXPECT_EQ(stats.liveEntries, 1U);
    EXPECT_EQ(stats.liveParticipants, 6U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);

    // A real duplicate joining clone-only content releases its copy, but
    // there is only one counted copy, so nothing is claimed as retained.
    auto dup = makeText(u"clone source words"_s);
    dup->shareContent(pool);
    EXPECT_TRUE(sameData(dup->words(), wordsOf(clones.front())));
    stats = pool.stats();
    EXPECT_GT(stats.cumulativeDeduplicatedBytes, 0U);
    EXPECT_EQ(stats.retainedGrossSavedBytes, 0U);

    clones.clear();
    dup.reset();
    stats = pool.stats();
    EXPECT_EQ(stats.liveEntries, 0U);
    EXPECT_EQ(stats.liveParticipants, 0U);
    EXPECT_EQ(stats.passiveParticipants, 0U);
}

TEST(MessageContentPool, AlreadySharedStorageIsNotCounted)
{
    MessageContentPool pool(eager());

    // Words backed by static literal data have no allocation of their own.
    QStringList literalA{u"static"_s, u"words"_s};
    QStringList literalB{u"static"_s, u"words"_s};
    // Heap-allocated words.
    QStringList heapA{QString::number(123456789), QString::number(987654321)};
    QStringList heapB{QString::number(123456789), QString::number(987654321)};
    // Equal words, where one string is also held elsewhere.
    QString outside = QString::number(555555555);
    QStringList partlyA{outside, QString::number(444444444)};
    QStringList partlyB{outside, QString::number(444444444)};

    std::array<MessageContentRef, 6> refs;
    pool.shareWords(literalA, refs[0]);
    pool.shareWords(literalB, refs[1]);
    const auto literalGross = pool.stats().retainedGrossSavedBytes;
    pool.shareWords(heapA, refs[2]);
    pool.shareWords(heapB, refs[3]);
    const auto heapGross = pool.stats().retainedGrossSavedBytes - literalGross;
    pool.shareWords(partlyA, refs[4]);
    pool.shareWords(partlyB, refs[5]);
    const auto partlyGross =
        pool.stats().retainedGrossSavedBytes - literalGross - heapGross;

    // Only the list array was released for the literal list.
    EXPECT_GT(literalGross, 0U);
    EXPECT_GT(heapGross, literalGross);
    // The externally held word was not released.
    EXPECT_GT(partlyGross, literalGross);
    EXPECT_LT(partlyGross, heapGross);
    EXPECT_EQ(pool.stats().cumulativeDeduplicatedBytes,
              literalGross + heapGross + partlyGross);
    EXPECT_TRUE(sameData(partlyA, partlyB));
    EXPECT_EQ(pool.stats().passiveParticipants, 0U);
}

TEST(MessageContentPool, SavingsUseCapacityConservatively)
{
    MessageContentPool pool(eager());

    QStringList base{QString::number(11111), QString::number(22222)};
    QStringList big{QString::number(11111), QString::number(22222)};
    big.reserve(64);
    QStringList small{QString::number(11111), QString::number(22222)};
    QStringList large{QString::number(11111), QString::number(22222)};
    large.reserve(256);

    std::array<MessageContentRef, 4> refs;
    pool.shareWords(base, refs[0]);
    pool.shareWords(big, refs[1]);
    const auto afterBig = pool.stats();
    pool.shareWords(small, refs[2]);
    pool.shareWords(large, refs[3]);
    const auto stats = pool.stats();

    // Released copies are sized by capacity, so the reserved one counts
    // more than the retained (unreserved) copy.
    EXPECT_GT(afterBig.cumulativeDeduplicatedBytes,
              afterBig.retainedPayloadBytes);
    EXPECT_EQ(afterBig.retainedGrossSavedBytes,
              afterBig.cumulativeDeduplicatedBytes);

    // Retained savings use the smallest released copy for every duplicate,
    // so they stay below what was actually released.
    EXPECT_LT(stats.retainedGrossSavedBytes, stats.cumulativeDeduplicatedBytes);
    EXPECT_EQ(stats.retainedGrossSavedBytes % 3, 0U);
    EXPECT_LE(stats.retainedGrossSavedBytes / 3, afterBig.retainedPayloadBytes);
}

TEST(MessageContentPool, AdmissionFilterKeepsUniqueContentFree)
{
    MessageContentPool pool({.admissionFilterSlots = 1024});

    std::vector<std::unique_ptr<TextElement>> elements;
    for (int i = 0; i < 500; i++)
    {
        elements.push_back(makeText(u"unique "_s + QString::number(i)));
        EXPECT_EQ(elements.back()->shareContent(pool), 0U);
        EXPECT_FALSE(elements.back()->hasSharedWords());
    }

    auto stats = pool.stats();
    EXPECT_EQ(stats.deferred, 500U);
    EXPECT_EQ(stats.misses, 0U);
    EXPECT_EQ(stats.liveEntries, 0U);
    EXPECT_EQ(stats.indexedEntries, 0U);
    // Only the fixed filter remains as pool overhead.
    EXPECT_GT(stats.fixedOverheadBytes, 0U);
    EXPECT_EQ(stats.estimatedOverheadBytes, stats.fixedOverheadBytes);

    // The second sighting creates the entry, later ones share it. Use the
    // most recent payload so no later one can have taken its filter slot.
    auto again = makeText(u"unique 499"_s);
    EXPECT_EQ(again->shareContent(pool), 1U);
    auto third = makeText(u"unique 499"_s);
    EXPECT_EQ(third->shareContent(pool), 1U);
    EXPECT_TRUE(sameData(again->words(), third->words()));
    // The first occurrence was never pooled and keeps its own copy.
    EXPECT_FALSE(sameData(elements.at(499)->words(), third->words()));

    stats = pool.stats();
    EXPECT_EQ(stats.misses, 1U);
    EXPECT_EQ(stats.hits, 1U);
    EXPECT_EQ(stats.liveEntries, 1U);
    EXPECT_GT(stats.retainedGrossSavedBytes, 0U);

    // Disabling clears the filter: re-enabling starts over.
    pool.setEnabled(false);
    pool.setEnabled(true);
    auto fresh = makeText(u"unique 498"_s);
    EXPECT_EQ(fresh->shareContent(pool), 0U);
}

TEST(MessageContentPool, HighUniquenessBoundsBookkeeping)
{
    MessageContentPool pool(
        {.maxIndexedEntries = 64, .admissionFilterSlots = 256});

    // Mostly unique content with one repeated phrase.
    std::vector<std::unique_ptr<TextElement>> elements;
    for (int i = 0; i < 5000; i++)
    {
        QString text = (i % 50 == 0)
                           ? u"repeated phrase"_s
                           : QString(u"unique message "_s + QString::number(i));
        elements.push_back(makeText(text));
        elements.back()->shareContent(pool);
    }

    auto stats = pool.stats();
    // Only the repeated phrase gets an entry; unique payloads get none.
    EXPECT_EQ(stats.liveEntries, 1U);
    EXPECT_LE(stats.indexedEntries, 64U);
    EXPECT_GE(stats.deferred, 4850U);
    EXPECT_GT(stats.retainedGrossSavedBytes, 0U);

    elements.clear();
    stats = pool.stats();
    EXPECT_EQ(stats.liveEntries, 0U);
    EXPECT_EQ(stats.liveParticipants, 0U);
}

TEST(MessageContentPool, TrackingSlotsAreCounted)
{
    MessageContentPool pool({.enabled = false});

    const auto before = pool.stats().trackingSlots;
    {
        MessageContentRef a;
        MessageContentRef b = a;
        MessageContentRef c = std::move(a);
        EXPECT_EQ(pool.stats().trackingSlots, before + 3);

        // Every message and text element carries one, pooled or not.
        auto msg = makeMessage(u"1"_s, u"dave"_s, u"hi"_s, nullptr, {});
        EXPECT_EQ(pool.stats().trackingSlots, before + 3 + 3);
        EXPECT_EQ(pool.stats().trackingSlotBytes,
                  (before + 6) * sizeof(MessageContentRef));
    }
    EXPECT_EQ(sizeof(MessageContentRef), sizeof(void *));
    EXPECT_EQ(pool.stats().trackingSlots, before);
}

TEST(MessageContentPool, ParticipantsMayOutliveThePool)
{
    std::unique_ptr<TextElement> a;
    std::unique_ptr<TextElement> b;
    std::unique_ptr<MessageElement> c;
    {
        MessageContentPool pool(eager());
        a = makeText(u"outlive"_s);
        b = makeText(u"outlive"_s);
        a->shareContent(pool);
        b->shareContent(pool);
    }
    EXPECT_TRUE(sameData(a->words(), b->words()));
    c = a->clone();
    a.reset();
    b.reset();
    EXPECT_EQ(wordsOf(c), QStringList{u"outlive"_s});
    c.reset();
}

TEST(MessageContentPool, ConcurrentShareAndRelease)
{
    MessageContentPool pool({.maxIndexedEntries = 64});
    std::atomic<bool> failed = false;

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
    {
        threads.emplace_back([&pool, &failed, t] {
            std::vector<QStringList> lists;
            std::vector<MessageContentRef> refs;
            for (int i = 0; i < 2000; i++)
            {
                QStringList words{u"word"_s, QString::number((i + t) % 100)};
                MessageContentRef ref;
                pool.shareWords(words, ref);
                if (words.at(1) != QString::number((i + t) % 100))
                {
                    failed = true;
                }
                lists.push_back(std::move(words));
                refs.push_back(std::move(ref));
                if (refs.size() > 50)
                {
                    refs.erase(refs.begin());
                    lists.erase(lists.begin());
                }
            }
        });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_FALSE(failed);
    auto stats = pool.stats();
    EXPECT_EQ(stats.liveParticipants, 0U);
    EXPECT_EQ(stats.passiveParticipants, 0U);
    EXPECT_EQ(stats.liveEntries, 0U);
    EXPECT_EQ(stats.indexedEntries, 0U);
    EXPECT_EQ(stats.hits + stats.misses + stats.deferred, 8000U);
    EXPECT_GT(stats.hits, 0U);
}
