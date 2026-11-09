// SIAARA DICOM Toolkit - DICOMweb shared helpers.
#include "web/dicomweb.hpp"

#include <string_view>

namespace siaara::web {

std::string_view media_type_for_transfer_syntax(std::string_view ts) noexcept {
    if (ts == "1.2.840.10008.1.2.4.50" || ts == "1.2.840.10008.1.2.4.51" || ts == "1.2.840.10008.1.2.4.57" ||
        ts == "1.2.840.10008.1.2.4.70")
        return "image/jpeg";
    if (ts == "1.2.840.10008.1.2.4.80" || ts == "1.2.840.10008.1.2.4.81") return "image/jls";
    if (ts == "1.2.840.10008.1.2.4.90" || ts == "1.2.840.10008.1.2.4.91") return "image/jp2";
    if (ts == "1.2.840.10008.1.2.4.92" || ts == "1.2.840.10008.1.2.4.93") return "image/jpx";
    if (ts == "1.2.840.10008.1.2.4.201" || ts == "1.2.840.10008.1.2.4.202" || ts == "1.2.840.10008.1.2.4.203")
        return "image/jphc";
    if (ts == "1.2.840.10008.1.2.4.110" || ts == "1.2.840.10008.1.2.4.111" || ts == "1.2.840.10008.1.2.4.112")
        return "image/jxl";
    if (ts == "1.2.840.10008.1.2.5") return "image/dicom-rle";
    if (ts == "1.2.840.10008.1.2.4.100" || ts == "1.2.840.10008.1.2.4.101") return "video/mpeg";
    if (ts.substr(0, 22) == "1.2.840.10008.1.2.4.10" && ts.size() == 23) return "video/mp4";
    return "application/octet-stream";
}

bool is_uid(std::string_view s) noexcept {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

}  // namespace siaara::web
