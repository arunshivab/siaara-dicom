#include "core/types.hpp"

#include <vector>

namespace siaara {

bool vr_is_known(VR vr) noexcept {
    switch (vr) {
        case VR::AE: case VR::AS: case VR::AT: case VR::CS: case VR::DA: case VR::DS:
        case VR::DT: case VR::FL: case VR::FD: case VR::IS: case VR::LO: case VR::LT:
        case VR::OB: case VR::OD: case VR::OF: case VR::OL: case VR::OV: case VR::OW:
        case VR::PN: case VR::SH: case VR::SL: case VR::SQ: case VR::SS: case VR::ST:
        case VR::SV: case VR::TM: case VR::UC: case VR::UI: case VR::UL: case VR::UN:
        case VR::UR: case VR::US: case VR::UT: case VR::UV:
            return true;
        default:
            return false;
    }
}

bool vr_has_long_length(VR vr) noexcept {
    switch (vr) {
        case VR::OB: case VR::OD: case VR::OF: case VR::OL: case VR::OV: case VR::OW:
        case VR::SQ: case VR::SV: case VR::UC: case VR::UN: case VR::UR: case VR::UT:
        case VR::UV:
            return true;
        default:
            return false;
    }
}

bool vr_is_string(VR vr) noexcept {
    switch (vr) {
        case VR::AE: case VR::AS: case VR::CS: case VR::DA: case VR::DS: case VR::DT:
        case VR::IS: case VR::LO: case VR::LT: case VR::PN: case VR::SH: case VR::ST:
        case VR::TM: case VR::UC: case VR::UI: case VR::UR: case VR::UT:
            return true;
        default:
            return false;
    }
}

std::size_t vr_value_size(VR vr) noexcept {
    switch (vr) {
        case VR::US: case VR::SS: case VR::OW: return 2;
        case VR::UL: case VR::SL: case VR::FL: case VR::OF: case VR::OL: case VR::AT: return 4;
        case VR::FD: case VR::OD: case VR::SV: case VR::UV: case VR::OV: return 8;
        case VR::OB: case VR::UN: return 1;
        default: return 0;
    }
}

char vr_pad_char(VR vr) noexcept {
    if (vr == VR::UI) return '\0';
    return vr_is_string(vr) ? ' ' : '\0';
}

std::string_view vr_name(VR vr) noexcept {
    switch (vr) {
#define SIAARA_VR_CASE(x) case VR::x: return #x;
        SIAARA_VR_CASE(AE) SIAARA_VR_CASE(AS) SIAARA_VR_CASE(AT) SIAARA_VR_CASE(CS)
        SIAARA_VR_CASE(DA) SIAARA_VR_CASE(DS) SIAARA_VR_CASE(DT) SIAARA_VR_CASE(FL)
        SIAARA_VR_CASE(FD) SIAARA_VR_CASE(IS) SIAARA_VR_CASE(LO) SIAARA_VR_CASE(LT)
        SIAARA_VR_CASE(OB) SIAARA_VR_CASE(OD) SIAARA_VR_CASE(OF) SIAARA_VR_CASE(OL)
        SIAARA_VR_CASE(OV) SIAARA_VR_CASE(OW) SIAARA_VR_CASE(PN) SIAARA_VR_CASE(SH)
        SIAARA_VR_CASE(SL) SIAARA_VR_CASE(SQ) SIAARA_VR_CASE(SS) SIAARA_VR_CASE(ST)
        SIAARA_VR_CASE(SV) SIAARA_VR_CASE(TM) SIAARA_VR_CASE(UC) SIAARA_VR_CASE(UI)
        SIAARA_VR_CASE(UL) SIAARA_VR_CASE(UN) SIAARA_VR_CASE(UR) SIAARA_VR_CASE(US)
        SIAARA_VR_CASE(UT) SIAARA_VR_CASE(UV)
#undef SIAARA_VR_CASE
        default: return "??";
    }
}

Bytes Bytes::copy_of(const void* data, std::size_t size) {
    auto store = std::make_shared<std::vector<std::uint8_t>>(size);
    if (size) std::memcpy(store->data(), data, size);
    const std::uint8_t* p = store->data();
    return Bytes(std::shared_ptr<const void>(store, store.get()), p, size);
}

}  // namespace siaara
