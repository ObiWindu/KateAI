#pragma once
/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Absolute path to python3, baked in by CMake at configure time. Tests spawn
 * python3 (mock LLM server, mock ACP agent) via QProcess; on macOS/Windows CI
 * Craft's craftenv.sh rewrites PATH, hiding the system python3 from the child
 * process. Using the configure-time absolute path sidesteps PATH entirely.
 */
#include <QString>

#ifdef KATEAI_PYTHON3
inline QString python3Command()
{
    return QString::fromUtf8(KATEAI_PYTHON3);
}
#else
inline QString python3Command()
{
    return QStringLiteral("python3");
}
#endif
