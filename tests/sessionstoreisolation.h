/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef KATEAI_TEST_SESSIONSTORE_ISOLATION_H
#define KATEAI_TEST_SESSIONSTORE_ISOLATION_H

#include <QDir>
#include <QTemporaryDir>

// Points the session store at a scratch database for the rest of the process.
//
// SessionStore otherwise lives at ~/.local/share/kateai/sessions.sqlite, which
// is the user's real conversation history, and Kate reopens the *active*
// conversation when the plugin view is created. A test that builds a ChatWidget
// (it autosaves its transcript while being destroyed) or that calls SessionStore
// directly writes straight into that database, so a plain test run wipes the
// user's threads and leaves a mock prompt as the thread Kate opens on the next
// start -- the user then sees a leftover test message on an empty panel instead
// of the welcome screen.
//
// Call it before the first SessionStore call: the path override is read once,
// when the per-thread connection is opened, so setting it later has no effect
// on a thread that already talked to the store.
inline void isolateSessionStore()
{
    static QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return;
    }

    const QString path = QDir(scratch.path()).filePath(QStringLiteral("sessions.sqlite"));
    qputenv("KATEAI_SESSION_DB", path.toLocal8Bit());
}

#endif // KATEAI_TEST_SESSIONSTORE_ISOLATION_H
