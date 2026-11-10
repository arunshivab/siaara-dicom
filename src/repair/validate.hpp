// SIAARA DICOM Toolkit - DICOM file validator.
// Checks, with Error / Warning / Info severities:
//  - structure (repair/scan.hpp): preamble, File Meta Information, element order, duplicates,
//    odd lengths, group lengths, sequence / item delimitation, truncation;
//  - every element against the data dictionary (PS3.6): known tag, retired, VR, VM, value length
//    and character repertoire / format per VR (PS3.5 6.2), UID syntax (PS3.5 9.1), private
//    creators (PS3.5 7.8);
//  - text against the Specific Character Set (PS3.3 C.12.1.1.2, PS3.5 6.1);
//  - File Meta Information content (PS3.10 7.1) and its agreement with the data set;
//  - the IOD's modules and attributes (PS3.3, generated tables in repair/iod.hpp): Type 1, 1C, 2,
//    2C requirements (the machine-checkable conditions are evaluated - docs/VALIDATION.md lists
//    them; the others are counted and reported once per module as "condition not evaluated", an
//    Info), 1C/2C attributes present although their condition does not hold
//    ("present-condition-unmet", PS3.5 7.4.4 / 7.4.6) and Enumerated Values;
//  - header-to-pixel consistency (repair/pixels.hpp).
// Messages never contain values from the data set.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "repair/report.hpp"
#include "repair/scan.hpp"

namespace siaara::repair {

struct ValidateOptions {
    bool check_dictionary = true;
    bool check_iod = true;
    bool check_pixels = true;
    bool check_charset = true;
    std::size_t max_findings = 5000;
};

std::vector<Finding> validate(const ScanResult& scan, const ValidateOptions& opt = {});
Result<std::vector<Finding>> validate_bytes(const Bytes& data, const ValidateOptions& opt = {});
Result<std::vector<Finding>> validate_file(const std::string& utf8_path, const ValidateOptions& opt = {});

// IOD requirement findings only (used by the attribute repair to know what to add).
std::vector<Finding> check_iod(const DicomFile& file, const ValidateOptions& opt = {});

}  // namespace siaara::repair
