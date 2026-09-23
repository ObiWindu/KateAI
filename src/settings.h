/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

namespace KateAi
{

class SettingsStore
{
public:
    static Settings load();
    static void save(const Settings &settings);
};

} // namespace KateAi
