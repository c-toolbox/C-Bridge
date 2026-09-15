/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bridgeapplication.h"

int main(int argc, char *argv[])
{
    CBridge::BridgeApplication app(argc, argv);
    return app.run();
}
