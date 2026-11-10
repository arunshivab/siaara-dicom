// SIAARA DICOM Toolkit - IOD module / attribute requirements generated from DICOM PS3.3
// (Annex A Composite IOD Module tables, Annex C and Section 10 Module and Macro Attribute tables)
// and PS3.4 Table B.5-1 by tools/gen_iod.py (src/repair/iod_tables.inc).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.hpp"

namespace siaara::repair {

enum class AttrType : std::uint8_t { None = 0, T1 = 1, T1C = 2, T2 = 3, T2C = 4, T3 = 5 };
std::string_view attr_type_name(AttrType t) noexcept;

// Condition terms (tools/gen_iod.py; docs/VALIDATION.md lists the PS3.3 wordings behind them):
//   Present         the attribute is present (an empty value counts)
//   HasValue        present with a non-empty value (a sequence: at least one item)
//   Absent          not present
//   Equals          a value (value_index: that value only, 1-based) is one of `values`
//   NotEquals       present with values, none of them (or value value_index) in `values`
//   Greater         the (first / value_index) value is an integer greater than values[0]
//   AbsentOrEquals  Absent, or Equals
//   IncludesTag     an AT attribute (Frame Increment Pointer) lists the tag in values[0] (8 hex digits)
enum class CondOp : std::uint8_t { None, Present, Absent, Equals, NotEquals, Greater, HasValue, AbsentOrEquals, IncludesTag };
enum class ModuleUsage : std::uint8_t { M = 1, U = 2, C = 3 };

struct CondTerm {
    CondOp op = CondOp::None;
    Tag tag = 0;
    Tag mask = 0xFFFFFFFFu;
    std::uint8_t value_index = 0;  // 0: any value; else the 1-based value the condition names
    // Where the attribute is looked for: 0 the item, its enclosing items, the top level (and the
    // functional groups); 1 "at the image level": the top-level data set only; 2 anywhere.
    std::uint8_t scope = 0;
    std::span<const std::string_view> values;
};

// A Type 1C/2C condition (or the condition of a conditional Include row): its terms joined by
// AND (any == false) or OR (any == true). Mixed AND / OR wording is not parsed.
struct Condition {
    bool any = false;
    std::vector<CondTerm> terms;
};

// One attribute of a module after Include rows are expanded, with its sequence children.
struct ReqNode {
    Tag tag = 0;
    Tag mask = 0xFFFFFFFFu;
    AttrType type = AttrType::T3;
    const Condition* condition = nullptr;          // 1C/2C with a parsed condition, else nullptr
    std::span<const std::string_view> enumerated;  // "Enumerated Values:" of the description
    // Conditions of enclosing conditional Include rows ("Include ... if ..."): all must hold for the
    // attribute to be required. guard_unevaluated: a guard whose condition was not parsed (or two
    // differently guarded copies were merged), so the requirement cannot be evaluated.
    std::vector<const Condition*> guards;
    bool guard_unevaluated = false;
    // This row's Type replaces the Type other modules of the IOD give the attribute (PS3.3 wording
    // "This type definition shall override the definition in the ... Module").
    bool overrides = false;
    // Type 1C / 2C without "may be present otherwise": shall not be present when the condition
    // does not hold (PS3.5 7.4.4 / 7.4.6).
    bool absent_unless_required = false;
    // The description itself forbids presence when the condition does not hold ("Shall not be
    // present otherwise", "Required only if ..."). Without it, absent_unless_required is PS3.5
    // 7.4 applied to a condition read from prose: reported as a warning, not an error.
    bool forbidden_otherwise = false;
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
    // The IOD's Functional Group Macros (PS3.3 "<IOD> Functional Group Macros" table), empty when
    // it has none: name, usage and the macro's attributes (usually one sequence with its item
    // attributes). A macro is in the Shared Functional Groups Sequence item or in every Per-Frame
    // Functional Groups Sequence item.
    std::vector<ModuleReq> functional_groups;
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
