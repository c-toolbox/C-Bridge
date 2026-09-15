/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "webrtc/linkheaderparser.h"

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

/// Splits on commas that are outside <> brackets and outside quoted strings.
QStringList splitEntries(const QString &value)
{
    QStringList entries;
    QString current;
    bool inQuotes = false;
    bool inBrackets = false;

    for (const QChar ch : value) {
        if (ch == u'"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && ch == u'<') {
            inBrackets = true;
        } else if (!inQuotes && ch == u'>') {
            inBrackets = false;
        }

        if (ch == u',' && !inQuotes && !inBrackets) {
            entries.append(current);
            current.clear();
            continue;
        }
        current.append(ch);
    }

    if (!current.trimmed().isEmpty()) {
        entries.append(current);
    }
    return entries;
}

QStringList splitParameters(const QString &value)
{
    QStringList parameters;
    QString current;
    bool inQuotes = false;

    for (const QChar ch : value) {
        if (ch == u'"') {
            inQuotes = !inQuotes;
        }
        if (ch == u';' && !inQuotes) {
            parameters.append(current);
            current.clear();
            continue;
        }
        current.append(ch);
    }

    if (!current.trimmed().isEmpty()) {
        parameters.append(current);
    }
    return parameters;
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.startsWith(u'"') && value.endsWith(u'"')) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

} // namespace

QList<IceServerSpec> parseIceServerLinkHeader(const QString &headerValue)
{
    QList<IceServerSpec> servers;

    for (const QString &entry : splitEntries(headerValue)) {
        const int open = entry.indexOf(u'<');
        const int close = entry.indexOf(u'>', open + 1);
        if (open < 0 || close < 0) {
            continue;
        }

        IceServerSpec server;
        server.url = entry.mid(open + 1, close - open - 1).trimmed();
        if (server.url.isEmpty()) {
            continue;
        }

        bool isIceServer = false;
        for (const QString &parameter : splitParameters(entry.mid(close + 1))) {
            const int equals = parameter.indexOf(u'=');
            if (equals < 0) {
                continue;
            }
            const QString key = parameter.left(equals).trimmed().toLower();
            const QString value = unquote(parameter.mid(equals + 1));

            if (key == u"rel"_s && value.compare(u"ice-server"_s, Qt::CaseInsensitive) == 0) {
                isIceServer = true;
            } else if (key == u"username"_s) {
                server.username = value;
            } else if (key == u"credential"_s) {
                server.credential = value;
            }
        }

        if (isIceServer) {
            servers.append(server);
        }
    }

    return servers;
}

} // namespace CBridge
