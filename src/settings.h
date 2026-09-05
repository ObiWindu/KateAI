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
