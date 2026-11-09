#include "repair/iod.hpp"

#include <map>
#include <mutex>
#include <string>

namespace siaara::repair {

namespace detail {

struct AttrRow {
    std::int8_t depth;
    Tag tag;
    Tag mask;
    std::uint8_t type;
    std::int16_t include;
    std::int16_t cond;
    std::int16_t enum_list;
};
struct AttrTable {
    std::string_view id;
    std::string_view caption;
    std::uint32_t first;
    std::uint32_t count;
};
struct ModuleRow {
    std::string_view ie;
    std::string_view name;
    std::int16_t table;
    std::uint8_t usage;
};
struct IodDef {
    std::string_view name;
    std::string_view id;
    std::uint32_t first;
    std::uint32_t count;
};
struct SopIod {
    std::string_view uid;
    std::string_view name;
    std::int16_t iod;
};
struct Condition {
    CondOp op;
    Tag tag;
    Tag mask;
    std::uint32_t first;
    std::uint32_t count;
};
struct EnumList {
    std::uint32_t first;
    std::uint32_t count;
};

#include "repair/iod_tables.inc"

}  // namespace detail

namespace {

using namespace detail;

constexpr std::size_t kTableCount = sizeof(kAttrTables) / sizeof(kAttrTables[0]);
constexpr std::size_t kCondCount = sizeof(kConditions) / sizeof(kConditions[0]);
constexpr int kMaxIncludeDepth = 16;

struct Cache {
    std::mutex mu;
    std::vector<std::unique_ptr<std::vector<ReqNode>>> tables{kTableCount};
    std::vector<repair::Condition> conditions;
    std::map<std::string, std::unique_ptr<IodReq>, std::less<>> iods;
    Cache() {
        conditions.reserve(kCondCount);
        for (const auto& c : kConditions)
            conditions.push_back({c.op, c.tag, c.mask,
                                  std::span<const std::string_view>(kStrings + c.first, c.count)});
    }
};

Cache& cache() {
    static Cache c;
    return c;
}

struct FlatRow {
    int depth;
    const AttrRow* row;
    std::vector<int> guards;  // condition indices of enclosing conditional includes (-1 = not parsed)
};

void flatten(int table, int depth_offset, int nesting, std::vector<int>& stack, std::vector<int>& guards,
             std::vector<FlatRow>& out) {
    if (table < 0 || static_cast<std::size_t>(table) >= kTableCount || nesting > kMaxIncludeDepth) return;
    for (int s : stack)
        if (s == table) return;  // recursive include (e.g. content items): stop at one level
    stack.push_back(table);
    const AttrTable& t = kAttrTables[table];
    for (std::uint32_t i = 0; i < t.count; ++i) {
        const AttrRow& r = kAttrRows[t.first + i];
        if (r.include >= 0) {
            const bool conditional = r.type == 6;
            if (conditional) guards.push_back(r.cond);
            flatten(r.include, depth_offset + r.depth, nesting + 1, stack, guards, out);
            if (conditional) guards.pop_back();
            continue;
        }
        out.push_back({depth_offset + r.depth, &r, guards});
    }
    stack.pop_back();
}

ReqNode node_of(const FlatRow& fr, Cache& c) {
    const AttrRow& r = *fr.row;
    ReqNode n;
    for (int g : fr.guards) {
        if (g < 0 || static_cast<std::size_t>(g) >= c.conditions.size()) n.guard_unevaluated = true;
        else n.guards.push_back(&c.conditions[static_cast<std::size_t>(g)]);
    }
    n.tag = r.tag;
    n.mask = r.mask;
    n.type = static_cast<AttrType>(r.type);
    if (r.cond >= 0 && static_cast<std::size_t>(r.cond) < c.conditions.size()) n.condition = &c.conditions[static_cast<std::size_t>(r.cond)];
    if (r.enum_list >= 0) {
        const EnumList& e = kEnumLists[r.enum_list];
        n.enumerated = std::span<const std::string_view>(kStrings + e.first, e.count);
    }
    return n;
}

// Merges a row into a list: the same tag listed twice (e.g. by two macros) keeps the strictest Type.
ReqNode* merge_into(std::vector<ReqNode>& list, ReqNode n) {
    for (ReqNode& x : list)
        if (x.tag == n.tag && x.mask == n.mask) {
            // Guards: an unguarded copy wins; two different guard sets cannot be combined here.
            const bool xg = !x.guards.empty() || x.guard_unevaluated;
            const bool ng = !n.guards.empty() || n.guard_unevaluated;
            if (!ng) {
                x.guards.clear();
                x.guard_unevaluated = false;
            } else if (xg && x.guards != n.guards) {
                x.guards.clear();
                x.guard_unevaluated = true;
            }
            if (static_cast<int>(n.type) < static_cast<int>(x.type)) {
                x.type = n.type;
                x.condition = n.condition;
            }
            if (x.enumerated.empty()) x.enumerated = n.enumerated;
            return &x;
        }
    list.push_back(std::move(n));
    return &list.back();
}

std::unique_ptr<std::vector<ReqNode>> build(int table, Cache& c) {
    std::vector<FlatRow> flat;
    std::vector<int> stack, guards;
    flatten(table, 0, 0, stack, guards, flat);
    auto root = std::make_unique<std::vector<ReqNode>>();
    // Build with index paths, since vectors may reallocate while children are added.
    std::vector<std::vector<std::size_t>> path_stack;  // path to the last node at each depth
    for (const FlatRow& fr : flat) {
        int d = fr.depth;
        if (d < 0) d = 0;
        // Parent: the last node at depth d-1 (rows deeper than parent + 1 attach to the deepest).
        if (static_cast<std::size_t>(d) > path_stack.size()) d = static_cast<int>(path_stack.size());
        std::vector<ReqNode>* list = root.get();
        std::vector<std::size_t> path;
        if (d > 0) {
            path = path_stack[static_cast<std::size_t>(d - 1)];
            for (std::size_t idx : path) list = &(*list)[idx].children;
        }
        ReqNode* placed = merge_into(*list, node_of(fr, c));
        path.push_back(static_cast<std::size_t>(placed - list->data()));
        path_stack.resize(static_cast<std::size_t>(d));
        path_stack.push_back(std::move(path));
    }
    return root;
}

const std::vector<ReqNode>* table_tree(int table) {
    if (table < 0 || static_cast<std::size_t>(table) >= kTableCount) return nullptr;
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mu);
    auto& slot = c.tables[static_cast<std::size_t>(table)];
    if (!slot) slot = build(table, c);
    return slot.get();
}

}  // namespace

std::string_view attr_type_name(AttrType t) noexcept {
    switch (t) {
        case AttrType::T1: return "1";
        case AttrType::T1C: return "1C";
        case AttrType::T2: return "2";
        case AttrType::T2C: return "2C";
        case AttrType::T3: return "3";
        case AttrType::None: break;
    }
    return "";
}

const std::vector<ReqNode>* module_attributes(std::string_view table_id) {
    for (std::size_t i = 0; i < kTableCount; ++i)
        if (kAttrTables[i].id == table_id) return table_tree(static_cast<int>(i));
    return nullptr;
}

const IodReq* iod_for_sop_class(std::string_view uid) {
    const SopIod* sop = nullptr;
    for (const SopIod& s : kSopClasses)
        if (s.uid == uid) {
            sop = &s;
            break;
        }
    if (!sop || sop->iod < 0) return nullptr;
    const IodDef& def = kIods[sop->iod];
    {
        Cache& c = cache();
        std::lock_guard<std::mutex> lock(c.mu);
        auto it = c.iods.find(def.id);
        if (it != c.iods.end()) return it->second.get();
    }
    auto iod = std::make_unique<IodReq>();
    iod->name = def.name;
    iod->table_id = def.id;
    for (std::uint32_t i = 0; i < def.count; ++i) {
        const ModuleRow& m = kModuleRows[def.first + i];
        ModuleReq mr;
        mr.ie = m.ie;
        mr.name = m.name;
        mr.table_id = kAttrTables[m.table].id;
        mr.usage = static_cast<ModuleUsage>(m.usage);
        mr.attributes = table_tree(m.table);
        iod->modules.push_back(mr);
    }
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mu);
    auto [it, inserted] = c.iods.emplace(std::string(def.id), std::move(iod));
    return it->second.get();
}

IodTableStats iod_table_stats() noexcept {
    return {kTableCount, sizeof(kAttrRows) / sizeof(kAttrRows[0]), sizeof(kIods) / sizeof(kIods[0]),
            sizeof(kModuleRows) / sizeof(kModuleRows[0]), sizeof(kSopClasses) / sizeof(kSopClasses[0]), kCondCount,
            sizeof(kEnumLists) / sizeof(kEnumLists[0])};
}

}  // namespace siaara::repair
