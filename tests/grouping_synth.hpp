// Test helpers for the grouping (tests/test_grouping.cpp, tests/test_grouping_alloc.cpp): synthetic
// files with a chosen geometry, and a small C API wrapper that reads a grouping's result back.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/dataset.hpp"
#include "core/writer.hpp"
#include "crosscheck/store_synth.hpp"
#include "siaara/siaara.h"

namespace grouping_synth {

using store_synth::Spec;

inline std::string ds_values(const double* v, std::size_t n) {
    std::string s;
    char b[40];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%.10g", v[i]);
        if (i) s.push_back('\\');
        s += b;
    }
    return s;
}

// A file of Spec s with Image Position / Orientation (Patient) set (DS), or removed when null.
inline siaara::DicomFile geometry_file(const Spec& s, const double* ipp, const double* iop) {
    siaara::DicomFile f = store_synth::make_file(s);
    if (ipp) (void)f.dataset.set_string(0x00200032, siaara::VR::DS, ds_values(ipp, 3));
    else (void)f.dataset.remove(0x00200032);
    if (iop) (void)f.dataset.set_string(0x00200037, siaara::VR::DS, ds_values(iop, 6));
    else (void)f.dataset.remove(0x00200037);
    return f;
}

inline std::vector<std::uint8_t> bytes_of(const siaara::DicomFile& f) {
    std::vector<std::uint8_t> out;
    (void)siaara::write_part10(f, out);
    return out;
}

// RAII and read-back of a siaara_grouping.
class Grouping {
public:
    explicit Grouping(const siaara_grouping_options* o = nullptr) { status_ = siaara_grouping_create(o, &g_); }
    ~Grouping() { siaara_grouping_free(g_); }
    Grouping(const Grouping&) = delete;
    Grouping& operator=(const Grouping&) = delete;

    siaara_status create_status() const { return status_; }
    siaara_grouping* get() const { return g_; }
    siaara_status add(const std::string& name, const std::vector<std::uint8_t>& b) {
        return siaara_grouping_add_file(g_, name.data(), name.size(), b.empty() ? nullptr : b.data(), b.size());
    }
    siaara_status add_dicomdir(const std::vector<std::uint8_t>& b) {
        return siaara_grouping_add_dicomdir(g_, b.empty() ? nullptr : b.data(), b.size());
    }
    siaara_grouping_summary result() {
        siaara_grouping_summary s;
        std::memset(&s, 0, sizeof s);
        s.struct_size = sizeof s;
        last_ = siaara_grouping_result(g_, &s);
        return s;
    }
    siaara_status last() const { return last_; }
    siaara_grouped_instance instance(uint32_t i) const {
        siaara_grouped_instance o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        (void)siaara_grouping_instance(g_, i, &o);
        return o;
    }
    siaara_grouped_series series(uint32_t i) const {
        siaara_grouped_series o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        (void)siaara_grouping_series(g_, i, &o);
        return o;
    }
    siaara_grouped_study study(uint32_t i) const {
        siaara_grouped_study o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        (void)siaara_grouping_study(g_, i, &o);
        return o;
    }
    siaara_grouped_patient patient(uint32_t i) const {
        siaara_grouped_patient o;
        std::memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        (void)siaara_grouping_patient(g_, i, &o);
        return o;
    }
    std::vector<siaara_grouping_issue> issues(uint64_t n) const {
        std::vector<siaara_grouping_issue> v;
        for (uint32_t i = 0; i < n; ++i) {
            siaara_grouping_issue o;
            std::memset(&o, 0, sizeof o);
            o.struct_size = sizeof o;
            (void)siaara_grouping_issue_get(g_, i, &o);
            v.push_back(o);
        }
        return v;
    }
    siaara_grouping_text text(int32_t level, uint32_t index, int32_t field) const {
        siaara_grouping_text t;
        std::memset(&t, 0, sizeof t);
        t.struct_size = sizeof t;
        (void)siaara_grouping_text_get(g_, level, index, field, &t);
        return t;
    }
    std::string stored(int32_t level, uint32_t index, int32_t field) const {
        const siaara_grouping_text t = text(level, index, field);
        return t.stored ? std::string(t.stored, t.stored_len) : std::string();
    }
    std::string display(int32_t level, uint32_t index, int32_t field) const {
        const siaara_grouping_text t = text(level, index, field);
        return t.display ? std::string(t.display, t.display_len) : std::string();
    }
    int32_t status(int32_t level, uint32_t index, int32_t field) const { return text(level, index, field).status; }

private:
    siaara_grouping* g_ = nullptr;
    siaara_status status_ = SIAARA_OK;
    siaara_status last_ = SIAARA_OK;
};

inline siaara_grouping_options options() {
    siaara_grouping_options o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    return o;
}

}  // namespace grouping_synth
