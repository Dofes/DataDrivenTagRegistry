#include "mod/DataDrivenTagRegistryMod.h"

#include "ll/api/mod/RegisterHelper.h"

namespace data_driven_tag_registry {

DataDrivenTagRegistryMod& DataDrivenTagRegistryMod::getInstance() {
    static DataDrivenTagRegistryMod instance;
    return instance;
}

bool DataDrivenTagRegistryMod::load() {
    // Code for loading the mod goes here.
    return true;
}

bool DataDrivenTagRegistryMod::enable() {
    // Code for enabling the mod goes here.
    return true;
}

bool DataDrivenTagRegistryMod::disable() {
    // Code for disabling the mod goes here.
    return true;
}

} // namespace data_driven_tag_registry

LL_REGISTER_MOD(
    data_driven_tag_registry::DataDrivenTagRegistryMod,
    data_driven_tag_registry::DataDrivenTagRegistryMod::getInstance()
);
