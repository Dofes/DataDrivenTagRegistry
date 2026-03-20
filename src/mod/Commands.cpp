

#include <string>
#include <string_view>
#include <vector>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/registry/ItemRegistryRef.h"
#include "mc/world/level/Level.h"
#include "mc\world\item\ItemTag.h"

namespace data_driven_tag_registry::command {

struct GetItemTagsParam {
    std::string typeStr;
};

namespace {

std::string normalizeTypeStr(std::string const& id) {
    if (id.find(':') == std::string::npos) {
        return std::string{"minecraft:"} + id;
    }
    return id;
}

std::string joinTags(std::vector<std::string> const& tags) {
    if (tags.empty()) {
        return {};
    }
    std::string out = tags.front();
    for (size_t i = 1; i < tags.size(); ++i) {
        out += ", ";
        out += tags[i];
    }
    return out;
}

} // namespace

void registerGetItemTagsCommand(bool isClientSide) {
    auto& cmd = ll::command::CommandRegistrar::getInstance(isClientSide)
                    .getOrCreateCommand(
                        "getitemtags",
                        "Get all tags of an item by type string",
                        ::CommandPermissionLevel::GameDirectors
                    );

    cmd.overload<GetItemTagsParam>().required("typeStr").execute(
        [](CommandOrigin const& origin, CommandOutput& output, GetItemTagsParam const& param) {
            auto* level = origin.getLevel();
            if (level == nullptr) {
                output.error("Level not available");
                return;
            }

            auto        itemRegistry = level->getItemRegistry();
            std::string query        = param.typeStr;

            auto itemWeak = itemRegistry.lookupByNameNoAlias(std::string_view{query});
            if (!itemWeak) {
                auto normalized = normalizeTypeStr(query);
                itemWeak        = itemRegistry.lookupByNameNoAlias(std::string_view{normalized});
                query           = std::move(normalized);
            }

            if (!itemWeak) {
                output.error("Item not found: {}", param.typeStr);
                return;
            }

            auto* item = itemWeak.get();
            if (item == nullptr) {
                output.error("Item resolve failed: {}", param.typeStr);
                return;
            }

            std::vector<std::string> tags;
            auto                     size = (item->mTags)->size();
            tags.reserve(size);
            for (auto const& tag : *item->mTags) {
                tags.emplace_back(tag.getString());
            }

            if (tags.empty()) {
                output.success("{} has no tags", query);
                return;
            }

            output.success("{} tags({}): {}", query, tags.size(), joinTags(tags));
        }
    );
}

} // namespace data_driven_tag_registry::command


#ifdef LL_PLAT_S
#include "ll/api/memory/Hook.h"

#include "mc/server/DedicatedServerCommands.h"

namespace data_driven_tag_registry::command {
LL_AUTO_TYPE_STATIC_HOOK(
    RegisterBuiltinCommands,
    ll::memory::HookPriority::Highest,
    DedicatedServerCommands,
    &DedicatedServerCommands::setupStandaloneServer,
    void,
    ::Bedrock::NotNullNonOwnerPtr<::Minecraft> const& minecraft,
    ::IMinecraftApp&                                  app,
    ::Level&                                          level,
    ::LevelStorage&                                   levelStorage,
    ::DedicatedServer&                                dedicatedServer,
    ::AllowListFile&                                  allowListFile,
    ::ScriptSettings*                                 scriptSettings
) {
    origin(minecraft, app, level, levelStorage, dedicatedServer, allowListFile, scriptSettings);
    registerGetItemTagsCommand(false);
}
} // namespace data_driven_tag_registry::command
#endif
