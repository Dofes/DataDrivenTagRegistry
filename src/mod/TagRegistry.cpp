
#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/file/Path.h"
#include "mc/deps/core/resource/PackType.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemTag.h"
#include "mc/world/item/registry/ItemRegistryRef.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"

#include "mc/resources/Pack.h"
#include "mc/resources/PackManifest.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackStack.h"

#include "ll/api/io/Logger.h"

#include "mod/DataDrivenTagRegistryMod.h"


auto getLogger() -> ll::io::Logger& {
    return data_driven_tag_registry::DataDrivenTagRegistryMod::getInstance().getSelf().getLogger();
}


namespace data_driven_tag_registry {

namespace {

struct RawEntry {
    std::string              type;
    std::string              identifier;
    std::vector<std::string> items;
};

struct TagNode {
    std::unordered_set<std::string> directItems;
    std::unordered_set<std::string> refs;
};

struct ResolveOneTypeResult {
    std::unordered_map<std::string, std::unordered_set<std::string>> resolved;
    std::vector<std::string>                                         warnings;
    std::vector<std::vector<std::string>>                            cycles;
};

struct ResolveAllResult {
    std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_set<std::string>>> resolved;
    std::vector<std::string>                                                                          warnings;
    std::unordered_map<std::string, std::vector<std::vector<std::string>>>                            cycles;
};

struct ApplyStats {
    size_t jsonFiles      = 0;
    size_t entryCount     = 0;
    size_t itemBindings   = 0;
    size_t blockBindings  = 0;
    size_t itemTagAdds    = 0;
    size_t blockTagAdds   = 0;
    size_t warningCount   = 0;
    size_t cycleTypeCount = 0;
};

std::string trim(std::string_view sv) {
    size_t begin = 0;
    size_t end   = sv.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(sv[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(sv[end - 1])) != 0) {
        --end;
    }
    return std::string{sv.substr(begin, end - begin)};
}

bool endsWithCaseInsensitive(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    size_t offset = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        auto a = static_cast<unsigned char>(s[offset + i]);
        auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::string normalizeId(std::string const& id) {
    if (id.find(':') == std::string::npos) {
        return std::string{"minecraft:"} + id;
    }
    return id;
}

std::string joinStrings(std::vector<std::string> const& values, std::string_view sep) {
    if (values.empty()) {
        return {};
    }
    std::string out = values.front();
    for (size_t i = 1; i < values.size(); ++i) {
        out += sep;
        out += values[i];
    }
    return out;
}

std::vector<std::vector<std::string>>
tarjanScc(std::unordered_map<std::string, std::unordered_set<std::string>> const& graph) {
    int                                   index = 0;
    std::vector<std::string>              stack;
    std::unordered_set<std::string>       onStack;
    std::unordered_map<std::string, int>  idx;
    std::unordered_map<std::string, int>  low;
    std::vector<std::vector<std::string>> sccs;

    std::function<void(std::string const&)> dfs = [&](std::string const& v) {
        idx[v] = index;
        low[v] = index;
        ++index;
        stack.push_back(v);
        onStack.insert(v);

        auto found = graph.find(v);
        if (found != graph.end()) {
            for (auto const& w : found->second) {
                if (idx.find(w) == idx.end()) {
                    dfs(w);
                    low[v] = std::min(low[v], low[w]);
                } else if (onStack.find(w) != onStack.end()) {
                    low[v] = std::min(low[v], idx[w]);
                }
            }
        }

        if (low[v] == idx[v]) {
            std::vector<std::string> comp;
            while (!stack.empty()) {
                auto x = stack.back();
                stack.pop_back();
                onStack.erase(x);
                comp.push_back(std::move(x));
                if (comp.back() == v) {
                    break;
                }
            }
            sccs.push_back(std::move(comp));
        }
    };

    for (auto const& [v, _] : graph) {
        if (idx.find(v) == idx.end()) {
            dfs(v);
        }
    }
    return sccs;
}

ResolveOneTypeResult resolveOneType(std::unordered_map<std::string, TagNode> const& tags, bool strictCycle = false) {
    ResolveOneTypeResult result;

    std::unordered_set<std::string> nodes;
    nodes.reserve(tags.size());
    for (auto const& [name, _] : tags) {
        nodes.insert(name);
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> graph;
    graph.reserve(nodes.size());
    for (auto const& n : nodes) {
        graph.emplace(n, std::unordered_set<std::string>{});
    }

    for (auto const& [name, node] : tags) {
        for (auto const& ref : node.refs) {
            if (nodes.find(ref) != nodes.end()) {
                graph[name].insert(ref);
            } else {
                result.warnings.emplace_back(fmt::format("标签 #{} 引用未定义的标签 #{}", name, ref));
            }
        }
    }

    auto sccs = tarjanScc(graph);

    std::unordered_map<std::string, int> compId;
    compId.reserve(nodes.size());
    for (int i = 0; i < static_cast<int>(sccs.size()); ++i) {
        for (auto const& n : sccs[i]) {
            compId[n] = i;
        }
    }

    for (auto const& comp : sccs) {
        if (comp.size() > 1) {
            result.cycles.push_back(comp);
            continue;
        }
        if (!comp.empty()) {
            auto const& n  = comp.front();
            auto        it = graph.find(n);
            if (it != graph.end() && it->second.find(n) != it->second.end()) {
                result.cycles.push_back(comp);
            }
        }
    }

    if (strictCycle && !result.cycles.empty()) {
        throw std::runtime_error("检测到循环引用");
    }

    auto                                         compCount = static_cast<int>(sccs.size());
    std::vector<std::unordered_set<std::string>> compDirect(static_cast<size_t>(compCount));
    std::vector<std::unordered_set<int>>         compSucc(static_cast<size_t>(compCount));

    for (auto const& [name, node] : tags) {
        int   c      = compId[name];
        auto& direct = compDirect[static_cast<size_t>(c)];
        direct.insert(node.directItems.begin(), node.directItems.end());
        for (auto const& r : graph[name]) {
            int c2 = compId[r];
            if (c != c2) {
                compSucc[static_cast<size_t>(c)].insert(c2);
            }
        }
    }

    std::vector<int> indeg(static_cast<size_t>(compCount), 0);
    for (int c = 0; c < compCount; ++c) {
        for (int d : compSucc[static_cast<size_t>(c)]) {
            ++indeg[static_cast<size_t>(d)];
        }
    }

    std::deque<int> q;
    for (int c = 0; c < compCount; ++c) {
        if (indeg[static_cast<size_t>(c)] == 0) {
            q.push_back(c);
        }
    }

    std::vector<int> topo;
    topo.reserve(static_cast<size_t>(compCount));
    while (!q.empty()) {
        int c = q.front();
        q.pop_front();
        topo.push_back(c);
        for (int d : compSucc[static_cast<size_t>(c)]) {
            auto& ref = indeg[static_cast<size_t>(d)];
            --ref;
            if (ref == 0) {
                q.push_back(d);
            }
        }
    }

    if (static_cast<int>(topo.size()) != compCount) {
        throw std::runtime_error("内部错误：压缩图不是 DAG");
    }

    std::vector<std::unordered_set<std::string>> compResolved(static_cast<size_t>(compCount));
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        int  c = *it;
        auto r = compDirect[static_cast<size_t>(c)];
        for (int dep : compSucc[static_cast<size_t>(c)]) {
            auto const& depSet = compResolved[static_cast<size_t>(dep)];
            r.insert(depSet.begin(), depSet.end());
        }
        compResolved[static_cast<size_t>(c)] = std::move(r);
    }

    for (auto const& n : nodes) {
        auto c             = compId[n];
        result.resolved[n] = compResolved[static_cast<size_t>(c)];
    }

    return result;
}

ResolveAllResult resolveDataDrivenTags(std::vector<RawEntry> const& entries, bool strictCycle = false) {
    ResolveAllResult all;

    std::unordered_map<std::string, std::unordered_map<std::string, TagNode>> byType;

    for (auto const& e : entries) {
        auto& node = byType[e.type][e.identifier];
        for (auto const& raw : e.items) {
            auto s = trim(raw);
            if (s.empty()) {
                continue;
            }
            if (s.front() == '#') {
                if (s.size() > 1) {
                    node.refs.insert(s.substr(1));
                }
            } else {
                node.directItems.insert(s);
            }
        }
    }

    for (auto const& [type, tags] : byType) {
        auto resolved      = resolveOneType(tags, strictCycle);
        all.resolved[type] = std::move(resolved.resolved);
        for (auto& w : resolved.warnings) {
            all.warnings.emplace_back("[" + type + "] " + std::move(w));
        }
        all.cycles[type] = std::move(resolved.cycles);
    }

    return all;
}

std::optional<std::string> getOptionalTrimmedString(nlohmann::json const& obj, char const* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::nullopt;
    }
    auto s = trim(it->get<std::string>());
    if (s.empty()) {
        return std::nullopt;
    }
    return s;
}

void iteratePacksCompat(ResourcePackManager const& rpm, std::function<void(PackInstance const&)> const& pred) {
    if (!pred) {
        throw std::bad_function_call{};
    }

    auto forEachInStackSlot = [&](ll::UntypedStorage<8, 8> const& slot) {
        auto const& stackPtr = slot.as<std::unique_ptr<ResourcePackStack>>();
        if (!stackPtr) {
            return;
        }
        auto const& packs = *stackPtr->mStack;
        for (auto const& pack : packs) {
            pred(pack);
        }
    };

    forEachInStackSlot(rpm.mUnk809618);
    forEachInStackSlot(rpm.mUnkd638dd);
    forEachInStackSlot(rpm.mUnk7a20a8);
    forEachInStackSlot(rpm.mUnk296ec8);
    forEachInStackSlot(rpm.mUnk9cce53);
}

void collectEntries(ResourcePackManager const& rpm, std::vector<RawEntry>& entries, ApplyStats& stats) {
    iteratePacksCompat(rpm, [&](PackInstance const& pack) {
        auto const packType = pack.mPack->mPack->mManifest->mPackType;
        if (packType != PackType::Behavior) {
            return;
        }

        auto const packName = static_cast<std::string>(*pack.mPack->mPack->mManifest->mName);

        pack.mPack->forEachIn(
            Core::Path{"data_driven_tags"},
            [&](Core::Path const& path) {
                auto const& pathStr = path.getUtf8StdString();
                if (!endsWithCaseInsensitive(pathStr, ".json")) {
                    return;
                }

                std::string raw;
                if (!pack.getResource(path, raw)) {
                    getLogger().warn("[DataDrivenTags] 无法读取文件: pack={}, file={}", packName, pathStr);
                    ++stats.warningCount;
                    return;
                }

                try {
                    auto j = nlohmann::json::parse(raw, nullptr, true, true);
                    if (!j.is_object()) {
                        getLogger().warn("[DataDrivenTags] JSON 根节点必须是对象: pack={}, file={}", packName, pathStr);
                        ++stats.warningCount;
                        return;
                    }

                    auto typeOpt       = getOptionalTrimmedString(j, "type");
                    auto identifierOpt = getOptionalTrimmedString(j, "identifier");
                    if (!typeOpt || !identifierOpt) {
                        getLogger()
                            .warn("[DataDrivenTags] 缺少有效的 type/identifier: pack={}, file={}", packName, pathStr);
                        ++stats.warningCount;
                        return;
                    }

                    auto itemsIt = j.find("items");
                    if (itemsIt == j.end() || !itemsIt->is_array()) {
                        getLogger().warn("[DataDrivenTags] items 必须是数组: pack={}, file={}", packName, pathStr);
                        ++stats.warningCount;
                        return;
                    }

                    RawEntry entry;
                    entry.type       = *typeOpt;
                    entry.identifier = *identifierOpt;
                    entry.items.reserve(itemsIt->size());
                    for (auto const& v : *itemsIt) {
                        if (v.is_string()) {
                            entry.items.emplace_back(v.get<std::string>());
                        }
                    }

                    entries.emplace_back(std::move(entry));
                    ++stats.jsonFiles;
                } catch (std::exception const& e) {
                    getLogger()
                        .warn("[DataDrivenTags] JSON 解析失败: pack={}, file={}, err={}", packName, pathStr, e.what());
                    ++stats.warningCount;
                }
            },
            pack.mSubpackIndex,
            true
        );
    });
}

void logCycles(
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> const& cycles,
    ApplyStats&                                                                   stats
) {
    for (auto const& [type, compList] : cycles) {
        if (compList.empty()) {
            continue;
        }
        ++stats.cycleTypeCount;
        for (auto const& comp : compList) {
            getLogger().warn("[DataDrivenTags] [{}] 检测到循环引用分量: {}", type, joinStrings(comp, " -> "));
        }
    }
}

Item* tryGetItemByName(ItemRegistryRef const& itemRegistry, std::string const& id) {
    auto direct = itemRegistry.lookupByNameNoAlias(std::string_view{id});
    if (direct) {
        return direct.get();
    }
    auto normalized = normalizeId(id);
    auto fallback   = itemRegistry.lookupByNameNoAlias(std::string_view{normalized});
    if (fallback) {
        return fallback.get();
    }
    return nullptr;
}

BlockType* tryGetBlockTypeByName(BlockTypeRegistry const& blockRegistry, std::string const& id) {
    auto direct = blockRegistry.lookupByName(HashedString{id}, false);
    if (direct) {
        return direct.get();
    }
    auto fallback = blockRegistry.lookupByName(HashedString{normalizeId(id)}, false);
    if (fallback) {
        return fallback.get();
    }
    return nullptr;
}

void applyResolvedTags(Level& level) {
    auto* rpm = level.getServerResourcePackManager();
    if (rpm == nullptr) {
        getLogger().warn("[DataDrivenTags] 跳过：ServerResourcePackManager 不可用");
        return;
    }

    ApplyStats            stats;
    std::vector<RawEntry> entries;
    collectEntries(*rpm, entries, stats);
    stats.entryCount = entries.size();

    if (entries.empty()) {
        getLogger().info("[DataDrivenTags] 未发现任何 data_driven_tags/*.json，跳过");
        return;
    }

    ResolveAllResult resolved;
    try {
        resolved = resolveDataDrivenTags(entries, false);
    } catch (std::exception const& e) {
        getLogger().error("[DataDrivenTags] 解析失败: {}", e.what());
        return;
    }

    stats.warningCount += resolved.warnings.size();
    for (auto const& w : resolved.warnings) {
        getLogger().warn("[DataDrivenTags] {}", w);
    }
    logCycles(resolved.cycles, stats);

    auto itemRegistry  = level.getItemRegistry();
    auto blockRegistry = level.getBlockTypeRegistry();

    for (auto const& [type, typeMap] : resolved.resolved) {
        if (type == "item") {
            for (auto const& [tagId, members] : typeMap) {
                ItemTag tag{tagId};
                for (auto const& memberId : members) {
                    Item* item = tryGetItemByName(itemRegistry, memberId);
                    if (item == nullptr) {
                        getLogger().warn("[DataDrivenTags] [item] 未找到物品: {} (for #{})", memberId, tagId);
                        ++stats.warningCount;
                        continue;
                    }
                    if (!item->hasTag(tag)) {
                        item->addTag(tag);
                        itemRegistry.addItemToTagMap(*item);
                        ++stats.itemTagAdds;
                    }
                    ++stats.itemBindings;
                }
            }
            continue;
        }

        if (type == "block") {
            for (auto const& [tagId, members] : typeMap) {
                HashedString tag{tagId};
                for (auto const& memberId : members) {
                    BlockType* blockType = tryGetBlockTypeByName(*blockRegistry, memberId);
                    if (blockType == nullptr) {
                        getLogger().warn("[DataDrivenTags] [block] 未找到方块: {} (for #{})", memberId, tagId);
                        ++stats.warningCount;
                        continue;
                    }
                    if (!blockType->hasTag(tag)) {
                        blockType->addTag(tag);
                        ++stats.blockTagAdds;
                    }
                    ++stats.blockBindings;
                }
            }
            continue;
        }

        getLogger().warn("[DataDrivenTags] 未支持的 type: {}", type);
        ++stats.warningCount;
    }

    getLogger().info(
        "[DataDrivenTags] 完成: files={}, entries={}, itemBindings={}, blockBindings={}, itemTagAdds={}, "
        "blockTagAdds={}, cyclesInTypes={}, warnings={}",
        stats.jsonFiles,
        stats.entryCount,
        stats.itemBindings,
        stats.blockBindings,
        stats.itemTagAdds,
        stats.blockTagAdds,
        stats.cycleTypeCount,
        stats.warningCount
    );
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelSetFinishedInitializingHook,
    ll::memory::HookPriority::Low,
    Level,
    &Level::$setFinishedInitializing,
    void
) {
    origin();

    auto* level = thisFor<Level>();
    if (level == nullptr || level->isClientSide()) {
        return;
    }
    try {
        applyResolvedTags(*level);
    } catch (...) {
        getLogger().error("[DataDrivenTags] 应用过程出现未捕获异常");
    }
}

} // namespace


} // namespace data_driven_tag_registry
