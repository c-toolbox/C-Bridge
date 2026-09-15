/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QString>

namespace CBridge {

/// One ICE server advertised by a WHEP server in a Link header.
struct IceServerSpec {
    QString url;
    QString username;
    QString credential;
};

/// Parses RFC 5988 Link header values and returns the entries with
/// rel="ice-server". MediaMTX emits these from its webrtcICEServers2 config.
///
/// Accepts both one entry per header and several comma-separated entries in a
/// single header, and tolerates commas inside quoted parameter values.
QList<IceServerSpec> parseIceServerLinkHeader(const QString &headerValue);

} // namespace CBridge
