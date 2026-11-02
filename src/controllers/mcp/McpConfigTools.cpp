// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "common/SignalVector.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/filters/lang/Filter.hpp"
#include "controllers/filters/lang/Tokenizer.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/types/AnyHighlight.hpp"
#include "controllers/highlights/types/Common.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "controllers/mcp/McpConfig.hpp"
#include "controllers/mcp/McpJson.hpp"
#include "controllers/mcp/McpTool.hpp"
#include "controllers/nicknames/Nickname.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <set>

namespace chatterino::mcp {

using namespace Qt::StringLiterals;

namespace {

const auto &LOG = chatterinoMcp;

/// How many backups to keep. Older ones are deleted.
constexpr int MAX_BACKUPS = 200;

void scheduleSave()
{
    // Signal vectors write their setting 100ms after a change
    QTimer::singleShot(500, [] {
        getSettings()->requestSave();
        getApp()->getCommands()->save();
    });
}

/// Describes how one settings list is read, validated and written
template <typename T>
struct SectionTraits;

template <>
struct SectionTraits<highlights::AnyHighlight> {
    using T = highlights::AnyHighlight;

    static SignalVector<T> &vector()
    {
        return getSettings()->sharedHighlights;
    }

    static QString key(const T &item)
    {
        return highlights::getID(item).toString();
    }

    static QJsonObject info(const T &item)
    {
        QJsonObject o{
            {"id", key(item)},
            {"type", highlights::getHighlightTypeName(item).toString()},
            {"name", highlights::getName(item)},
            {"enabled", highlights::isEnabled(item)},
            {"builtIn", !highlights::isUserDefined(item)},
        };
        auto error = highlights::getError(item);
        if (!error.isEmpty())
        {
            o.insert("error", error);
        }
        return o;
    }

    /// `previous` is set when an existing item is replaced
    static std::variant<T, QString> parse(QJsonObject object,
                                          const T *previous)
    {
        if (previous && !object.contains("id"))
        {
            object.insert("id", key(*previous));
        }
        if (!previous && !object.contains("id"))
        {
            object.insert("id", highlights::generateID());
        }
        if (!object.contains("type") && !previous)
        {
            return u"Missing \"type\" (one of \"filter\", \"message\", "
                   "\"user\", \"badge\")"_s;
        }

        auto item = fromJson<T>(object);
        if (!item || std::holds_alternative<highlights::InvalidHighlight>(*item))
        {
            return u"Could not read this highlight. Compare it with the "
                   "items from list_config."_s;
        }

        if (previous)
        {
            if (highlights::isUserDefined(*previous) !=
                    highlights::isUserDefined(*item) ||
                (!highlights::isUserDefined(*previous) &&
                 key(*previous) != key(*item)))
            {
                return u"A built-in highlight can only be replaced by itself "
                       "(same id)"_s;
            }
        }
        else if (!highlights::isUserDefined(*item))
        {
            return u"Built-in highlights always exist. Change them with "
                   "update_config_items."_s;
        }

        auto error = highlights::getError(*item);
        if (!error.isEmpty())
        {
            return error;
        }
        return *item;
    }

    static QString canRemove(const T &item)
    {
        if (!highlights::isUserDefined(item))
        {
            return u"Built-in highlights can't be removed. Set \"enabled\" to "
                   "false instead."_s;
        }
        return {};
    }
};

template <>
struct SectionTraits<IgnorePhrase> {
    using T = IgnorePhrase;

    static SignalVector<T> &vector()
    {
        return getSettings()->ignoredMessages;
    }

    static QString key(const T & /*item*/)
    {
        return {};
    }

    static QJsonObject info(const T &item)
    {
        QJsonObject o;
        if (item.isRegex() && !item.isRegexValid())
        {
            o.insert("error", "Invalid regex");
        }
        return o;
    }

    static std::variant<T, QString> parse(const QJsonObject &object,
                                          const T * /*previous*/)
    {
        auto item = fromJson<T>(object);
        if (!item)
        {
            return u"Could not read this ignore phrase"_s;
        }
        if (item->getPattern().isEmpty())
        {
            return u"\"pattern\" is empty"_s;
        }
        if (item->isRegex() && !item->isRegexValid())
        {
            return u"\"pattern\" is not a valid regex"_s;
        }
        return *item;
    }

    static QString canRemove(const T & /*item*/)
    {
        return {};
    }
};

template <>
struct SectionTraits<FilterRecordPtr> {
    using T = FilterRecordPtr;

    static SignalVector<T> &vector()
    {
        return getSettings()->filterRecords;
    }

    static QString key(const T &item)
    {
        return item->getId().toString(QUuid::WithoutBraces);
    }

    static QJsonObject info(const T &item)
    {
        QJsonObject o{{"id", key(item)}};
        auto res = filters::Filter::fromString(item->getFilter());
        if (auto *error = std::get_if<filters::FilterError>(&res))
        {
            o.insert("error", error->message);
        }
        return o;
    }

    static std::variant<T, QString> parse(QJsonObject object,
                                          const T *previous)
    {
        if (!object.contains("id"))
        {
            object.insert("id", previous ? key(*previous)
                                         : QUuid::createUuid().toString(
                                               QUuid::WithoutBraces));
        }
        auto item = fromJson<T>(object);
        if (!item)
        {
            return u"Could not read this filter"_s;
        }
        if ((*item)->getName().isEmpty())
        {
            return u"\"name\" is empty"_s;
        }
        auto res = filters::Filter::fromString((*item)->getFilter());
        if (auto *error = std::get_if<filters::FilterError>(&res))
        {
            return u"Invalid filter: %1"_s.arg(error->message);
        }
        return *item;
    }

    static QString canRemove(const T & /*item*/)
    {
        return {};
    }
};

template <>
struct SectionTraits<Command> {
    using T = Command;

    static SignalVector<T> &vector()
    {
        return getApp()->getCommands()->items;
    }

    static QString key(const T &item)
    {
        return item.name;
    }

    static QJsonObject info(const T & /*item*/)
    {
        return {};
    }

    static std::variant<T, QString> parse(QJsonObject object,
                                          const T * /*previous*/)
    {
        if (!object.contains("showInMsgContextMenu"))
        {
            object.insert("showInMsgContextMenu", false);
        }
        auto item = fromJson<T>(object);
        if (!item)
        {
            return u"Could not read this command. It needs \"name\" and "
                   "\"func\"."_s;
        }
        if (item->name.isEmpty())
        {
            return u"\"name\" is empty"_s;
        }
        return *item;
    }

    static QString canRemove(const T & /*item*/)
    {
        return {};
    }
};

template <>
struct SectionTraits<HighlightBlacklistUser> {
    using T = HighlightBlacklistUser;

    static SignalVector<T> &vector()
    {
        return getSettings()->blacklistedUsers;
    }

    static QString key(const T & /*item*/)
    {
        return {};
    }

    static QJsonObject info(const T & /*item*/)
    {
        return {};
    }

    static std::variant<T, QString> parse(const QJsonObject &object,
                                          const T * /*previous*/)
    {
        auto item = fromJson<T>(object);
        if (!item)
        {
            return u"Could not read this user"_s;
        }
        if (item->getPattern().isEmpty())
        {
            return u"\"pattern\" is empty"_s;
        }
        return *item;
    }

    static QString canRemove(const T & /*item*/)
    {
        return {};
    }
};

template <>
struct SectionTraits<Nickname> {
    using T = Nickname;

    static SignalVector<T> &vector()
    {
        return getSettings()->nicknames;
    }

    static QString key(const T & /*item*/)
    {
        return {};
    }

    static QJsonObject info(const T & /*item*/)
    {
        return {};
    }

    static std::variant<T, QString> parse(const QJsonObject &object,
                                          const T * /*previous*/)
    {
        auto item = fromJson<T>(object);
        if (!item)
        {
            return u"Could not read this nickname"_s;
        }
        if (item->name().isEmpty())
        {
            return u"\"name\" is empty"_s;
        }
        return *item;
    }

    static QString canRemove(const T & /*item*/)
    {
        return {};
    }
};

/// Type-erased access to one settings list
struct Section {
    QString name;
    QString description;
    /// What `id` refers to for this section, empty if items only have indexes
    QString idMeaning;

    std::function<QJsonArray()> list;
    std::function<QJsonArray()> rawValues;
    std::function<QString(const QJsonArray &values)> validateAll;
    std::function<QString(const QJsonArray &values, int position)> add;
    /// Each update is {index | id, value}
    std::function<QString(const QJsonArray &updates)> update;
    std::function<QString(std::vector<int> indexes, const QStringList &ids)>
        remove;
    std::function<QString(int from, int to)> move;
    std::function<QString(const QJsonArray &values)> replaceAll;
};

template <typename T>
int findIndex(const std::vector<T> &items, const QJsonObject &target,
              QString &error)
{
    using Traits = SectionTraits<T>;
    if (target.contains("index"))
    {
        auto index = target.value("index").toInt(-1);
        if (index < 0 || index >= static_cast<int>(items.size()))
        {
            error = u"Index %1 is out of range (0-%2)"_s.arg(index).arg(
                static_cast<int>(items.size()) - 1);
            return -1;
        }
        return index;
    }
    if (target.contains("id"))
    {
        auto id = target.value("id").toString();
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (!id.isEmpty() && Traits::key(items[i]) == id)
            {
                return static_cast<int>(i);
            }
        }
        error = u"No item with id \"%1\""_s.arg(id);
        return -1;
    }
    error = u"Give \"index\" or \"id\""_s;
    return -1;
}

template <typename T>
Section makeSection(QString name, QString description, QString idMeaning)
{
    using Traits = SectionTraits<T>;

    Section s{
        .name = std::move(name),
        .description = std::move(description),
        .idMeaning = std::move(idMeaning),
    };

    s.list = [] {
        QJsonArray out;
        const auto &items = Traits::vector().raw();
        for (size_t i = 0; i < items.size(); ++i)
        {
            auto entry = Traits::info(items[i]);
            entry.insert("index", static_cast<int>(i));
            entry.insert("value", toJson(items[i]));
            out.append(entry);
        }
        return out;
    };

    s.rawValues = [] {
        QJsonArray out;
        for (const auto &item : Traits::vector().raw())
        {
            out.append(toJson(item));
        }
        return out;
    };

    auto parseAll = [](const QJsonArray &values, std::vector<T> &out) {
        for (qsizetype i = 0; i < values.size(); ++i)
        {
            auto parsed = Traits::parse(values[i].toObject(), nullptr);
            if (auto *error = std::get_if<QString>(&parsed))
            {
                return u"Item %1: %2"_s.arg(i).arg(*error);
            }
            out.push_back(std::get<T>(std::move(parsed)));
        }
        return QString();
    };

    s.validateAll = [parseAll](const QJsonArray &values) {
        std::vector<T> parsed;
        return parseAll(values, parsed);
    };

    s.add = [parseAll](const QJsonArray &values, int position) {
        std::vector<T> parsed;
        if (auto error = parseAll(values, parsed); !error.isEmpty())
        {
            return error;
        }

        auto &vec = Traits::vector();
        if (!vec.isSorted())
        {
            for (size_t i = 0; i < parsed.size(); ++i)
            {
                auto key = Traits::key(parsed[i]);
                if (key.isEmpty())
                {
                    continue;
                }
                for (const auto &existing : vec.raw())
                {
                    if (Traits::key(existing) == key)
                    {
                        return u"Item %1: id \"%2\" is already used"_s.arg(i)
                            .arg(key);
                    }
                }
            }
        }

        auto size = static_cast<int>(vec.raw().size());
        if (position < 0 || position > size)
        {
            position = size;
        }
        for (auto &item : parsed)
        {
            vec.insert(item, position++);
        }
        return QString();
    };

    s.update = [](const QJsonArray &updates) {
        auto &vec = Traits::vector();
        std::vector<std::pair<int, T>> parsed;
        std::set<int> seen;
        for (qsizetype i = 0; i < updates.size(); ++i)
        {
            auto update = updates[i].toObject();
            QString error;
            auto index = findIndex(vec.raw(), update, error);
            if (index < 0)
            {
                return u"Update %1: %2"_s.arg(i).arg(error);
            }
            if (!seen.insert(index).second)
            {
                return u"Update %1: item %2 is updated twice"_s.arg(i).arg(
                    index);
            }
            auto result =
                Traits::parse(update.value("value").toObject(), &vec.raw()[index]);
            if (auto *e = std::get_if<QString>(&result))
            {
                return u"Update %1: %2"_s.arg(i).arg(*e);
            }
            parsed.emplace_back(index, std::get<T>(std::move(result)));
        }
        for (auto &[index, item] : parsed)
        {
            vec.removeAt(index);
            vec.insert(item, index);
        }
        return QString();
    };

    s.remove = [](std::vector<int> indexes, const QStringList &ids) {
        auto &vec = Traits::vector();
        const auto &items = vec.raw();
        for (const auto &id : ids)
        {
            QString error;
            auto index = findIndex(items, {{"id", id}}, error);
            if (index < 0)
            {
                return error;
            }
            indexes.push_back(index);
        }
        std::ranges::sort(indexes);
        auto [first, last] = std::ranges::unique(indexes);
        indexes.erase(first, last);
        for (auto index : indexes)
        {
            if (index < 0 || index >= static_cast<int>(items.size()))
            {
                return u"Index %1 is out of range"_s.arg(index);
            }
            if (auto error = Traits::canRemove(items[index]); !error.isEmpty())
            {
                return u"Item %1: %2"_s.arg(index).arg(error);
            }
        }
        for (auto it = indexes.rbegin(); it != indexes.rend(); ++it)
        {
            vec.removeAt(*it);
        }
        return QString();
    };

    s.move = [](int from, int to) {
        auto &vec = Traits::vector();
        auto size = static_cast<int>(vec.raw().size());
        if (vec.isSorted())
        {
            return u"This list is sorted and can't be reordered"_s;
        }
        if (from < 0 || from >= size || to < 0 || to >= size)
        {
            return u"Index out of range (0-%1)"_s.arg(size - 1);
        }
        T item = vec.raw()[from];
        vec.removeAt(from);
        vec.insert(item, to);
        return QString();
    };

    s.replaceAll = [parseAll](const QJsonArray &values) {
        std::vector<T> parsed;
        for (qsizetype i = 0; i < values.size(); ++i)
        {
            // Restoring has to bring built-in highlights back, so this
            // deserializes directly instead of going through `parse`
            auto item = fromJson<T>(values[i].toObject());
            if (!item)
            {
                return u"Item %1 could not be read"_s.arg(i);
            }
            parsed.push_back(std::move(*item));
        }
        auto &vec = Traits::vector();
        vec.clear();
        for (const auto &item : parsed)
        {
            vec.append(item);
        }
        return QString();
    };

    return s;
}

const std::vector<Section> &sections()
{
    static std::vector<Section> s{
        makeSection<highlights::AnyHighlight>(
            "highlights",
            "Highlights, including the built-in ones (whispers, your "
            "username, subscriptions...). User-defined types are \"filter\" "
            "(filterText), \"message\" (pattern), \"user\" (pattern) and "
            "\"badge\". Outcome fields: showInMentions, alert, sound, "
            "backgroundColor (\"#AARRGGBB\").",
            "highlight id"),
        makeSection<IgnorePhrase>(
            "ignores",
            "Ignore phrases. isBlock=true hides matching messages; "
            "otherwise matches are replaced with replaceWith. With isBlock "
            "and highlightOnly, the message is never hidden; when it is "
            "highlighted, suppressMentions keeps it out of /mentions and "
            "suppressHighlight drops its highlight color.",
            {}),
        makeSection<FilterRecordPtr>(
            "filters",
            "Named filters that can be applied to splits (name, filter, id).",
            "filter id"),
        makeSection<Command>(
            "commands",
            "Custom commands (name, func, showInMsgContextMenu). func may use "
            "{1}, {2}, {1+}, {channel.name}, {user.name} and similar.",
            "command name"),
        makeSection<HighlightBlacklistUser>(
            "highlight_blacklist",
            "Users whose messages are never highlighted (pattern, regex).",
            {}),
        makeSection<Nickname>("nicknames",
                              "Nicknames shown instead of user names (name, "
                              "replace, isRegex, isCaseSensitive).",
                              {}),
    };
    return s;
}

const Section *findSection(const QString &name)
{
    for (const auto &section : sections())
    {
        if (section.name == name)
        {
            return &section;
        }
    }
    return nullptr;
}

QStringList sectionNames()
{
    QStringList names;
    for (const auto &section : sections())
    {
        names.append(section.name);
    }
    return names;
}

QString backupDirectory()
{
    return QDir(getApp()->getPaths().settingsDirectory)
        .filePath(u"mcp-backups"_s);
}

void pruneBackups()
{
    QDir dir(backupDirectory());
    auto files = dir.entryList({u"*.json"_s}, QDir::Files, QDir::Name);
    while (files.size() > MAX_BACKUPS)
    {
        dir.remove(files.takeFirst());
    }
}

QJsonObject readBackup(const QString &id, QString &error)
{
    if (id.contains('/') || id.contains('\\') || id.contains(u".."_s))
    {
        error = u"Invalid backup id"_s;
        return {};
    }
    QFile file(QDir(backupDirectory()).filePath(id + u".json"_s));
    if (!file.open(QIODevice::ReadOnly))
    {
        error = u"No backup with id \"%1\""_s.arg(id);
        return {};
    }
    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
    {
        error = u"Backup \"%1\" is not valid JSON"_s.arg(id);
        return {};
    }
    return doc.object();
}

/// Runs `change` after saving a backup. Returns the error of either.
QJsonObject changeWithBackup(const QString &reason,
                             const std::function<QString()> &change,
                             QString &error)
{
    auto backupId = saveBackup(reason, error);
    if (!error.isEmpty())
    {
        return {};
    }
    error = change();
    if (!error.isEmpty())
    {
        // Nothing was changed, the backup isn't needed
        QFile::remove(
            QDir(backupDirectory()).filePath(backupId + u".json"_s));
        return {};
    }
    scheduleSave();
    return {{"ok", true}, {"backup", backupId}};
}

QJsonObject sectionProperty()
{
    return schema::stringEnum("Which settings list", sectionNames());
}

QString sectionHelp()
{
    QString help;
    for (const auto &section : sections())
    {
        help += u"- %1: %2"_s.arg(section.name, section.description);
        if (!section.idMeaning.isEmpty())
        {
            help += u" Items can be addressed by \"id\" (%1)."_s.arg(
                section.idMeaning);
        }
        help += '\n';
    }
    return help;
}

}  // namespace

QString saveBackup(const QString &reason, QString &error)
{
    QJsonObject sectionValues;
    for (const auto &section : sections())
    {
        sectionValues.insert(section.name, section.rawValues());
    }

    auto now = QDateTime::currentDateTime();
    auto id = now.toString(u"yyyyMMdd-HHmmss-zzz"_s);
    QDir dir(backupDirectory());
    if (!dir.mkpath(u"."_s))
    {
        error = u"Could not create %1"_s.arg(dir.path());
        return {};
    }
    // Two changes in the same millisecond
    for (int i = 2; dir.exists(id + u".json"_s); ++i)
    {
        id = now.toString(u"yyyyMMdd-HHmmss-zzz"_s) + u"-%1"_s.arg(i);
    }

    QJsonObject root{
        {"version", 1},
        {"createdAt", now.toString(Qt::ISODateWithMs)},
        {"reason", reason},
        {"sections", sectionValues},
    };

    QSaveFile file(dir.filePath(id + u".json"_s));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit())
    {
        error = u"Could not write backup: %1"_s.arg(file.errorString());
        return {};
    }

    pruneBackups();
    qCDebug(LOG) << "Saved backup" << id << reason;
    return id;
}

std::variant<highlights::AnyHighlight, QString> parseHighlight(
    const QJsonObject &value, const highlights::AnyHighlight *previous)
{
    return SectionTraits<highlights::AnyHighlight>::parse(value, previous);
}

std::variant<IgnorePhrase, QString> parseIgnorePhrase(const QJsonObject &value)
{
    return SectionTraits<IgnorePhrase>::parse(value, nullptr);
}

QJsonArray currentSectionValues(const QString &section)
{
    if (const auto *s = findSection(section))
    {
        return s->rawValues();
    }
    return {};
}

std::vector<Tool> makeConfigTools()
{
    std::vector<Tool> tools;

    tools.push_back({
        .name = "list_config",
        .description =
            u"List one settings list. Each entry has its \"index\", its "
            "\"value\" (the exact JSON stored in Chatterino's settings, which "
            "is also what the change tools take) and, for some lists, extra "
            "read-only info such as \"id\" or \"error\".\n\nLists:\n"_s +
            sectionHelp(),
        .inputSchema = schema::object({{"section", sectionProperty()}},
                                      {"section"}),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                const auto *section = findSection(args::string(a, "section"));
                if (!section)
                {
                    done(ToolResult::fail(u"Unknown section"_s));
                    return;
                }
                auto items = section->list();
                done(ToolResult::ok(QJsonObject{
                    {"section", section->name},
                    {"count", items.size()},
                    {"items", items},
                }));
            },
    });

    tools.push_back({
        .name = "add_config_items",
        .description =
            "Add items to a settings list. All items are checked first; if "
            "any is invalid nothing is added. New highlights and filters get "
            "an id when none is given. Order matters for highlights: the "
            "first matching highlight decides the color. A backup is saved "
            "first.",
        .inputSchema = schema::object(
            {
                {"section", sectionProperty()},
                {"items", schema::array("Values in the same format as "
                                        "list_config's \"value\"",
                                        schema::anyObject("Item"))},
                {"position",
                 schema::integer("Index to insert at. Default: the end.")},
            },
            {"section", "items"}),
        .changesSettings = true,
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                const auto *section = findSection(args::string(a, "section"));
                if (!section)
                {
                    done(ToolResult::fail(u"Unknown section"_s));
                    return;
                }
                auto items = a.value("items").toArray();
                auto position = args::integer(a, "position", -1);
                QString error;
                auto result = changeWithBackup(
                    u"add %1 %2"_s.arg(items.size()).arg(section->name),
                    [&] {
                        return section->add(items, position);
                    },
                    error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                done(ToolResult::ok(result));
            },
    });

    tools.push_back({
        .name = "update_config_items",
        .description =
            "Replace items in a settings list. Each update names the item by "
            "\"index\" or \"id\" and gives its complete new \"value\". All "
            "updates are checked first; if any is invalid nothing changes. A "
            "backup is saved first.",
        .inputSchema = schema::object(
            {
                {"section", sectionProperty()},
                {"updates",
                 schema::array(
                     "Updates",
                     schema::object(
                         {
                             {"index", schema::integer("Item index")},
                             {"id", schema::string("Item id")},
                             {"value", schema::anyObject("Complete new value")},
                         },
                         {"value"}))},
            },
            {"section", "updates"}),
        .changesSettings = true,
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                const auto *section = findSection(args::string(a, "section"));
                if (!section)
                {
                    done(ToolResult::fail(u"Unknown section"_s));
                    return;
                }
                auto updates = a.value("updates").toArray();
                QString error;
                auto result = changeWithBackup(
                    u"update %1 %2"_s.arg(updates.size()).arg(section->name),
                    [&] {
                        return section->update(updates);
                    },
                    error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                done(ToolResult::ok(result));
            },
    });

    tools.push_back({
        .name = "remove_config_items",
        .description =
            "Remove items from a settings list by index and/or id. Indexes "
            "refer to the list before anything is removed. A backup is saved "
            "first.",
        .inputSchema = schema::object(
            {
                {"section", sectionProperty()},
                {"indexes",
                 schema::array("Item indexes", schema::integer("Index"))},
                {"ids", schema::array("Item ids", schema::string("Id"))},
            },
            {"section"}),
        .changesSettings = true,
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                const auto *section = findSection(args::string(a, "section"));
                if (!section)
                {
                    done(ToolResult::fail(u"Unknown section"_s));
                    return;
                }
                std::vector<int> indexes;
                for (const auto &v : a.value("indexes").toArray())
                {
                    indexes.push_back(v.toInt(-1));
                }
                QStringList ids;
                for (const auto &v : a.value("ids").toArray())
                {
                    ids.append(v.toString());
                }
                if (indexes.empty() && ids.isEmpty())
                {
                    done(ToolResult::fail(u"Give \"indexes\" or \"ids\""_s));
                    return;
                }
                QString error;
                auto result = changeWithBackup(
                    u"remove %1 %2"_s.arg(indexes.size() + ids.size())
                        .arg(section->name),
                    [&] {
                        return section->remove(indexes, ids);
                    },
                    error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                done(ToolResult::ok(result));
            },
    });

    tools.push_back({
        .name = "move_config_item",
        .description = "Move one item in a settings list to another index. "
                       "A backup is saved first.",
        .inputSchema = schema::object(
            {
                {"section", sectionProperty()},
                {"from", schema::integer("Current index")},
                {"to", schema::integer("New index")},
            },
            {"section", "from", "to"}),
        .changesSettings = true,
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                const auto *section = findSection(args::string(a, "section"));
                if (!section)
                {
                    done(ToolResult::fail(u"Unknown section"_s));
                    return;
                }
                auto from = args::integer(a, "from", -1);
                auto to = args::integer(a, "to", -1);
                QString error;
                auto result = changeWithBackup(
                    u"move %1 item"_s.arg(section->name),
                    [&] {
                        return section->move(from, to);
                    },
                    error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                done(ToolResult::ok(result));
            },
    });

    tools.push_back({
        .name = "list_backups",
        .description =
            "List the backups saved before each change, newest first, with "
            "the number of items in each list.",
        .inputSchema = schema::object({
            {"limit", schema::integer("Maximum backups to list. Default 30.")},
        }),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto limit = args::integer(a, "limit", 30);
                QDir dir(backupDirectory());
                auto files =
                    dir.entryList({u"*.json"_s}, QDir::Files, QDir::Name);
                std::ranges::reverse(files);
                QJsonArray out;
                for (const auto &file : files)
                {
                    if (out.size() >= limit)
                    {
                        break;
                    }
                    auto id = file.chopped(5);
                    QString error;
                    auto backup = readBackup(id, error);
                    QJsonObject counts;
                    auto backupSections = backup.value("sections").toObject();
                    for (auto it = backupSections.begin();
                         it != backupSections.end(); ++it)
                    {
                        counts.insert(it.key(), it.value().toArray().size());
                    }
                    out.append(QJsonObject{
                        {"id", id},
                        {"createdAt", backup.value("createdAt")},
                        {"reason", backup.value("reason")},
                        {"counts", counts},
                    });
                }
                done(ToolResult::ok(QJsonObject{
                    {"directory", dir.path()},
                    {"backups", out},
                }));
            },
    });

    tools.push_back({
        .name = "create_backup",
        .description = "Save a backup of all settings lists now, e.g. before "
                       "starting a larger change.",
        .inputSchema = schema::object({
            {"reason", schema::string("Short description")},
        }),
        .changesSettings = true,
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                QString error;
                auto id = saveBackup(args::string(a, "reason", u"manual"_s),
                                     error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                done(ToolResult::ok(QJsonObject{{"backup", id}}));
            },
    });

    tools.push_back({
        .name = "restore_backup",
        .description =
            "Restore settings lists from a backup. Without \"sections\", "
            "every list in the backup is restored. The current state is "
            "backed up first, so a restore can be undone too.",
        .inputSchema = schema::object(
            {
                {"id", schema::string("Backup id from list_backups")},
                {"sections",
                 schema::array("Lists to restore. Default: all.",
                               sectionProperty())},
            },
            {"id"}),
        .changesSettings = true,
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto id = args::string(a, "id");
                QString error;
                auto backup = readBackup(id, error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                auto backupSections = backup.value("sections").toObject();
                QStringList names;
                for (const auto &v : a.value("sections").toArray())
                {
                    names.append(v.toString());
                }
                if (names.isEmpty())
                {
                    names = backupSections.keys();
                }

                std::vector<std::pair<const Section *, QJsonArray>> restores;
                for (const auto &name : names)
                {
                    const auto *section = findSection(name);
                    if (!section || !backupSections.contains(name))
                    {
                        done(ToolResult::fail(
                            u"The backup has no list \"%1\""_s.arg(name)));
                        return;
                    }
                    restores.emplace_back(section,
                                          backupSections.value(name).toArray());
                }

                auto result = changeWithBackup(
                    u"before restoring %1"_s.arg(id),
                    [&] {
                        for (const auto &[section, values] : restores)
                        {
                            if (auto e = section->replaceAll(values);
                                !e.isEmpty())
                            {
                                return u"%1: %2"_s.arg(section->name, e);
                            }
                        }
                        return QString();
                    },
                    error);
                if (!error.isEmpty())
                {
                    done(ToolResult::fail(error));
                    return;
                }
                result.insert("restored", QJsonArray::fromStringList(names));
                done(ToolResult::ok(result));
            },
    });

    tools.push_back({
        .name = "validate_filter",
        .description =
            "Check a filter expression (as used by filter highlights and "
            "split filters). Returns whether it parses, the error if not, "
            "its result type, and how Chatterino parsed it (with "
            "parentheses showing precedence).",
        .inputSchema = schema::object(
            {{"filter", schema::string("Filter expression")}}, {"filter"}),
        .run =
            [](const QJsonObject &a, const ToolDone &done) {
                auto res = filters::Filter::fromString(args::string(a, "filter"));
                if (auto *error = std::get_if<filters::FilterError>(&res))
                {
                    done(ToolResult::ok(QJsonObject{
                        {"valid", false},
                        {"error", error->message},
                    }));
                    return;
                }
                const auto &filter = std::get<filters::Filter>(res);
                done(ToolResult::ok(QJsonObject{
                    {"valid", true},
                    {"returnType", filters::typeToString(filter.returnType())},
                    {"parsed", filter.filterString()},
                }));
            },
    });

    tools.push_back({
        .name = "filter_reference",
        .description = "List the variables and operators of the filter "
                       "language.",
        .inputSchema = schema::object({}),
        .run =
            [](const QJsonObject & /*args*/, const ToolDone &done) {
                QJsonObject identifiers;
                for (auto it = filters::VALID_IDENTIFIERS_MAP.begin();
                     it != filters::VALID_IDENTIFIERS_MAP.end(); ++it)
                {
                    identifiers.insert(it.key(), it.value());
                }
                done(ToolResult::ok(QJsonObject{
                    {"identifiers", identifiers},
                    {"notes",
                     QJsonArray{
                         "author.name is the display name (as shown in "
                         "chat), compared case-insensitively.",
                         "Strings: \"text\". Regex: r\"pattern\" "
                         "(case-sensitive) or ri\"pattern\" "
                         "(case-insensitive). Lists: {\"a\", \"b\"}.",
                         "Logic: && || ! and parentheses.",
                         "Comparison: == != < <= > >= (string == and != "
                         "ignore case).",
                         "String/list operators: contains, startswith, "
                         "endswith (ignore case), match (regex; a list on "
                         "the right side like {r\"(\\d+)\", 1} returns "
                         "capture group 1).",
                         "Math: + - * / %.",
                         "author.badges is a list like {\"moderator\", "
                         "\"subscriber\"}; test it with contains.",
                         "Wiki: https://wiki.chatterino.com/Filters/",
                     }},
                }));
            },
    });

    return tools;
}

}  // namespace chatterino::mcp
