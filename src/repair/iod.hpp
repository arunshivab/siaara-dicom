// SIAARA DICOM Toolkit - IOD module / attribute requirements generated from DICOM PS3.3
// (Annex A Composite IOD Module tables, Annex C and Section 10 Module and Macro Attribute tables)
// and PS3.4 Table B.5-1 by tools/gen_iod.py (src/repair/iod_tables.inc).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "core/types.hpp"

namespace siaara::repair {

enum class AttrType : std::uint8_t { None = 0, T1 = 1, T1C = 2, T2 = 3, T2C = 4, T3 = 5 };
std::string_view attr_type_name(AttrType t) noexcept;

enum class CondOp : std::uint8_t { None, Present, Absent, Equals, NotEquals, Greater };
enum class ModuleUsage : std::uint8_t { M = 1, U = 2, C = 3 };

struct Condition {
    CondOp op = CondOp::None;
    Tag tag = 0;
    Tag mask = 0xFFFFFFFFu;
    std::span<const std::string_view> values;
};

// One attribute of a module after Include rows are expanded, with its sequence children.
struct ReqNode {
    Tag tag = 0;
    Tag mask = 0xFFFFFFFFu;
    AttrType type = AttrType::T3;
    const Condition* condition = nullptr;          // 1C/2C with a parsed condition, else nullptr
    std::span<const std::string_view> enumerated;  // "Enumerated Values:" of the description
    std::vector<ReqNode> children;                 // attributes of each item, when a sequence
    bool matches(Tag t) const noexcept { return (t & mask) == tag; }
};

struct ModuleReq {
    std::string_view ie;
    std::string_view name;
    std::string_view table_id;   // e.g. "C.7-1"
    ModuleUsage usage = ModuleUsage::M;
    const std::vector<ReqNode>* attributes = nullptr;
};

struct IodReq {
    std::string_view name;       // e.g. "CT Image"
    std::string_view table_id;   // e.g. "A.3-1"
    std::vector<ModuleReq> modules;
};

// The IOD for a SOP Class UID (PS3.4 B.5-1), or nullptr. Thread-safe; results are cached.
const IodReq* iod_for_sop_class(std::string_view sop_class_uid);
// The expanded attribute tree of an attribute table by its id ("C.7-1"), or nullptr.
const std::vector<ReqNode>* module_attributes(std::string_view table_id);

struct IodTableStats {
    std::size_t attribute_tables, attribute_rows, iods, module_rows, sop_classes, conditions, enum_lists;
};
IodTableStats iod_table_stats() noexcept;

}  // namespace siaara::repair
