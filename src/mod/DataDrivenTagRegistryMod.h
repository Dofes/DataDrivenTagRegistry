#pragma once

#include "ll/api/mod/NativeMod.h"

namespace data_driven_tag_registry {

class DataDrivenTagRegistryMod {

public:
    static DataDrivenTagRegistryMod& getInstance();

    DataDrivenTagRegistryMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    // TODO: Implement this method if you need to unload the mod.
    // /// @return True if the mod is unloaded successfully.
    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace data_driven_tag_registry
