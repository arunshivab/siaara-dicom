// Regression tests for the 2026-11-09 security review, C API findings (R-013, R-033).
// R-013 is also checked statically by tests/check_capi_guarded.py (ctest: capi_guarded).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/transfer_syntax.hpp"
#include "core/writer.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

using namespace siaara;

// Trivial exports are noexcept for C++ callers; everything else is guarded (R-013).
static_assert(noexcept(siaara_abi_version()));
static_assert(noexcept(siaara_last_error_message()));
static_assert(noexcept(siaara_file_free(nullptr)));

TEST_CASE("R-013: error text lives in a fixed buffer and is cleared on success") {
    siaara_dataset* out = nullptr;
    CHECK(siaara_file_dataset(nullptr, &out) == SIAARA_ERR_INVALID_ARGUMENT);
    const char* msg = siaara_last_error_message();
    REQUIRE(msg != nullptr);
    CHECK(std::strlen(msg) > 0);
    CHECK(std::strlen(msg) < 512);
    siaara_dict_entry e;
    std::memset(&e, 0, sizeof e);
    e.struct_size = sizeof e;
    CHECK(siaara_dict_lookup(0x00100010, &e) == SIAARA_OK);
    CHECK(std::strlen(siaara_last_error_message()) == 0);
    // A parse error with an offset keeps the offset suffix.
    const std::uint8_t junk[200] = {'D', 'I', 'C', 'M'};
    siaara_read_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.strict = 1;
    o.allow_raw_dataset = 0;
    siaara_file* f = nullptr;
    CHECK(siaara_file_open_memory(junk, sizeof junk, &o, &f) != SIAARA_OK);
    CHECK(f == nullptr);
    CHECK(std::strlen(siaara_last_error_message()) < 512);
}

TEST_CASE("R-033: an item stays at the same address while siblings are added or removed") {
    Element seq;
    seq.tag = 0x00081140;
    seq.vr = VR::SQ;
    for (int i = 0; i < 2; ++i) {
        Dataset it;
        (void)it.set_string(0x00081155, VR::UI, "2.25." + std::to_string(i + 1));
        seq.items.push_back(std::move(it));
    }
    Dataset parent;
    parent.put(std::move(seq));
    Element* e = parent.find(0x00081140);
    REQUIRE(e != nullptr);
    const Dataset* first = &e->items[0];
    // Many sibling items (the sequence storage grows and reallocates).
    for (int i = 0; i < 1000; ++i) e->items.push_back(Dataset{});
    CHECK(&e->items[0] == first);
    CHECK_EQ(first->get_string(0x00081155).value_or(""), "2.25.1");
    // Removing a sibling before it keeps the other items in place too.
    const Dataset* second = &e->items[1];
    e->items.erase(e->items.begin());
    CHECK(&e->items[0] == second);
    CHECK_EQ(second->get_string(0x00081155).value_or(""), "2.25.2");
    // The parent growing (its element storage reallocates) does not move items either.
    for (std::uint16_t g = 0; g < 2000; ++g) (void)parent.set_uint16(0x00080000u + 2u * g, 1);
    e = parent.find(0x00081140);
    REQUIRE(e != nullptr);
    CHECK(&e->items[0] == second);
    // Copies are deep.
    Dataset copy = parent;
    CHECK(&copy.find(0x00081140)->items[0] != second);
    CHECK_EQ(copy.find(0x00081140)->items[0].get_string(0x00081155).value_or(""), "2.25.2");
}

TEST_CASE("R-033: C API item handles survive changes to other elements of the parent") {
    DicomFile f;
    f.transfer_syntax = std::string(ts::ExplicitVRLittleEndian);
    (void)f.dataset.set_string(tags::SOPInstanceUID, VR::UI, "2.25.1");
    Element seq;
    seq.tag = 0x00081140;
    seq.vr = VR::SQ;
    for (int i = 0; i < 3; ++i) {
        Dataset it;
        (void)it.set_string(0x00081155, VR::UI, "2.25.9" + std::to_string(i));
        seq.items.push_back(std::move(it));
    }
    f.dataset.put(std::move(seq));
    std::vector<std::uint8_t> bytes;
    REQUIRE(write_part10(f, bytes).is_ok());
    siaara_file* h = nullptr;
    REQUIRE(siaara_file_open_memory(bytes.data(), bytes.size(), nullptr, &h) == SIAARA_OK);
    siaara_dataset* ds = nullptr;
    REQUIRE(siaara_file_dataset(h, &ds) == SIAARA_OK);
    siaara_dataset* item = nullptr;
    REQUIRE(siaara_dataset_item(ds, 0x00081140, 1, &item) == SIAARA_OK);
    for (std::uint32_t g = 0; g < 2000; ++g) (void)siaara_dataset_set_uint16(ds, 0x00080000u + g * 2u, 1);
    for (std::uint32_t g = 0; g < 500; ++g) (void)siaara_dataset_remove(ds, 0x00080000u + g * 2u);
    char buf[64];
    size_t n = 0;
    CHECK(siaara_dataset_get_string(item, 0x00081155, 0, buf, sizeof buf, &n) == SIAARA_OK);
    CHECK_EQ(std::string(buf), "2.25.91");
    siaara_file_free(h);
}
