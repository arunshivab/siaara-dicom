// SIAARA DICOM Toolkit - SPIR-V module builder (see spirv_builder.hpp).
#include "gpu/spirv_builder.hpp"

#include <cstring>

namespace siaara::gpu::spirv {

namespace {

// Enumerants (SPIR-V specification, chapter 3).
constexpr std::uint32_t kMagic = 0x07230203u;
constexpr std::uint32_t kVersion10 = 0x00010000u;
constexpr std::uint32_t kCapShader = 1;
constexpr std::uint32_t kAddressingLogical = 0;
constexpr std::uint32_t kMemoryGlsl450 = 1;
constexpr std::uint32_t kExecGLCompute = 5;
constexpr std::uint32_t kModeLocalSize = 17;
constexpr std::uint32_t kStorageUniformConstant = 0;
constexpr std::uint32_t kStorageInput = 1;
constexpr std::uint32_t kStorageUniform = 2;
constexpr std::uint32_t kStorageFunction = 7;
constexpr std::uint32_t kDecBufferBlock = 3;
constexpr std::uint32_t kDecArrayStride = 6;
constexpr std::uint32_t kDecBuiltIn = 11;
constexpr std::uint32_t kDecNonWritable = 24;
constexpr std::uint32_t kDecBinding = 33;
constexpr std::uint32_t kDecDescriptorSet = 34;
constexpr std::uint32_t kDecOffset = 35;
constexpr std::uint32_t kBuiltInGlobalInvocationId = 28;
constexpr std::uint32_t kImageOperandLod = 0x2;

}  // namespace

Module::Module() {
    glsl_ = fresh();
    main_ = fresh();
}

void Module::emit(std::vector<std::uint32_t>& s, std::uint16_t opcode, std::initializer_list<std::uint32_t> words) {
    s.push_back((static_cast<std::uint32_t>(words.size() + 1) << 16) | opcode);
    s.insert(s.end(), words.begin(), words.end());
}

void Module::emit_v(std::vector<std::uint32_t>& s, std::uint16_t opcode, const std::vector<std::uint32_t>& words) {
    s.push_back((static_cast<std::uint32_t>(words.size() + 1) << 16) | opcode);
    s.insert(s.end(), words.begin(), words.end());
}

// A literal string: UTF-8 bytes, NUL-terminated, padded with NULs to a word boundary (2.2.1).
void Module::append_string(std::vector<std::uint32_t>& w, const char* s) {
    const std::size_t n = std::strlen(s) + 1;
    const std::size_t words = (n + 3) / 4;
    const std::size_t at = w.size();
    w.resize(at + words, 0u);
    for (std::size_t i = 0; i < n - 1; ++i)
        w[at + i / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[i])) << (8 * (i % 4));
}

void Module::name(Id id, const char* n) {
    std::vector<std::uint32_t> w{id};
    append_string(w, n);
    emit_v(debug_, op::Name, w);
}

Id Module::type_key(const std::vector<std::uint32_t>& key, std::uint16_t opcode, std::vector<std::uint32_t> operands) {
    auto it = types_.find(key);
    if (it != types_.end()) return it->second;
    const Id id = fresh();
    operands.insert(operands.begin(), id);
    emit_v(globals_, opcode, operands);
    types_.emplace(key, id);
    return id;
}

Id Module::t_void() { return type_key({op::TypeVoid}, op::TypeVoid, {}); }
Id Module::t_bool() {
    const Id t = type_key({op::TypeBool}, op::TypeBool, {});
    scalar_[t] = Scalar::Bool;
    return t;
}
Id Module::t_u32() {
    const Id t = type_key({op::TypeInt, 32, 0}, op::TypeInt, {32, 0});
    scalar_[t] = Scalar::U32;
    return t;
}
Id Module::t_i32() {
    const Id t = type_key({op::TypeInt, 32, 1}, op::TypeInt, {32, 1});
    scalar_[t] = Scalar::I32;
    return t;
}
Id Module::t_f32() {
    const Id t = type_key({op::TypeFloat, 32}, op::TypeFloat, {32});
    scalar_[t] = Scalar::F32;
    return t;
}
Id Module::t_vec(Id component, std::uint32_t n) { return type_key({op::TypeVector, component, n}, op::TypeVector, {component, n}); }
Id Module::t_ptr(std::uint32_t storage_class, Id pointee) {
    return type_key({op::TypePointer, storage_class, pointee}, op::TypePointer, {storage_class, pointee});
}
Id Module::t_image(Id sampled_type, std::uint32_t dim, std::uint32_t sampled, std::uint32_t format) {
    // Depth 0, Arrayed 0, MS 0.
    return type_key({op::TypeImage, sampled_type, dim, sampled, format}, op::TypeImage, {sampled_type, dim, 0, 0, 0, sampled, format});
}
Id Module::t_sampled_image(Id image) { return type_key({op::TypeSampledImage, image}, op::TypeSampledImage, {image}); }
Id Module::t_func(Id ret) { return type_key({op::TypeFunction, ret}, op::TypeFunction, {ret}); }

Scalar Module::scalar_of(Id type) const {
    auto it = scalar_.find(type);
    return it == scalar_.end() ? Scalar::None : it->second;
}

V Module::c_u32(std::uint32_t v) {
    const Id t = t_u32();
    const std::vector<std::uint32_t> key{t, v};
    auto it = consts_.find(key);
    if (it != consts_.end()) return it->second;
    const V r{fresh(), t};
    emit(globals_, op::Constant, {t, r.id, v});
    consts_.emplace(key, r);
    return r;
}
V Module::c_i32(std::int32_t v) {
    const Id t = t_i32();
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    const std::vector<std::uint32_t> key{t, bits};
    auto it = consts_.find(key);
    if (it != consts_.end()) return it->second;
    const V r{fresh(), t};
    emit(globals_, op::Constant, {t, r.id, bits});
    consts_.emplace(key, r);
    return r;
}
V Module::c_f32(float v) {
    const Id t = t_f32();
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    const std::vector<std::uint32_t> key{t, bits};
    auto it = consts_.find(key);
    if (it != consts_.end()) return it->second;
    const V r{fresh(), t};
    emit(globals_, op::Constant, {t, r.id, bits});
    consts_.emplace(key, r);
    return r;
}
V Module::c_bool(bool v) {
    const Id t = t_bool();
    const std::vector<std::uint32_t> key{t, v ? 1u : 0u, 0xB001u};
    auto it = consts_.find(key);
    if (it != consts_.end()) return it->second;
    const V r{fresh(), t};
    emit(globals_, v ? op::ConstantTrue : op::ConstantFalse, {t, r.id});
    consts_.emplace(key, r);
    return r;
}

Id Module::buffer_u32(std::uint32_t set, std::uint32_t binding, const char* nm) {
    if (block_struct_ == 0) {
        const Id u = t_u32();
        const Id arr = type_key({op::TypeRuntimeArray, u}, op::TypeRuntimeArray, {u});
        emit(annotations_, op::Decorate, {arr, kDecArrayStride, 4});
        block_struct_ = type_key({op::TypeStruct, arr}, op::TypeStruct, {arr});
        emit(annotations_, op::Decorate, {block_struct_, kDecBufferBlock});
        emit(annotations_, op::MemberDecorate, {block_struct_, 0, kDecOffset, 0});
        emit(annotations_, op::MemberDecorate, {block_struct_, 0, kDecNonWritable});
    }
    const Id ptr = t_ptr(kStorageUniform, block_struct_);
    const Id v = fresh();
    emit(globals_, op::Variable, {ptr, v, kStorageUniform});
    emit(annotations_, op::Decorate, {v, kDecDescriptorSet, set});
    emit(annotations_, op::Decorate, {v, kDecBinding, binding});
    name(v, nm);
    return v;
}

Id Module::sampled_image_3d(std::uint32_t set, std::uint32_t binding, const char* nm) {
    const Id img = t_image(t_f32(), 2 /* Dim 3D */, 1 /* sampled */, 0 /* Unknown */);
    const Id si = t_sampled_image(img);
    const Id ptr = t_ptr(kStorageUniformConstant, si);
    const Id v = fresh();
    emit(globals_, op::Variable, {ptr, v, kStorageUniformConstant});
    emit(annotations_, op::Decorate, {v, kDecDescriptorSet, set});
    emit(annotations_, op::Decorate, {v, kDecBinding, binding});
    name(v, nm);
    return v;
}

Id Module::storage_image_2d_r32ui(std::uint32_t set, std::uint32_t binding, const char* nm) {
    const Id img = t_image(t_u32(), 1 /* Dim 2D */, 2 /* storage */, 33 /* R32ui */);
    const Id ptr = t_ptr(kStorageUniformConstant, img);
    const Id v = fresh();
    emit(globals_, op::Variable, {ptr, v, kStorageUniformConstant});
    emit(annotations_, op::Decorate, {v, kDecDescriptorSet, set});
    emit(annotations_, op::Decorate, {v, kDecBinding, binding});
    name(v, nm);
    return v;
}

Id Module::global_invocation_id() {
    if (gid_) return gid_;
    const Id ptr = t_ptr(kStorageInput, t_vec(t_u32(), 3));
    gid_ = fresh();
    emit(globals_, op::Variable, {ptr, gid_, kStorageInput});
    emit(annotations_, op::Decorate, {gid_, kDecBuiltIn, kBuiltInGlobalInvocationId});
    name(gid_, "gid");
    interface_.push_back(gid_);
    return gid_;
}

void Module::begin_main(std::uint32_t local_x, std::uint32_t local_y) {
    local_x_ = local_x;
    local_y_ = local_y;
    name(main_, "main");
}

std::vector<std::uint32_t> Module::finish() {
    const Id fnty = t_func(t_void());
    const Id vt = t_void();
    const Id entry = fresh();
    std::vector<std::uint32_t> out{kMagic, kVersion10, 0u, 0u /* bound, patched below */, 0u};
    emit(out, op::Capability, {kCapShader});
    {
        std::vector<std::uint32_t> w{glsl_};
        append_string(w, "GLSL.std.450");
        emit_v(out, op::ExtInstImport, w);
    }
    emit(out, op::MemoryModel, {kAddressingLogical, kMemoryGlsl450});
    {
        std::vector<std::uint32_t> w{kExecGLCompute, main_};
        append_string(w, "main");
        w.insert(w.end(), interface_.begin(), interface_.end());
        emit_v(out, op::EntryPoint, w);
    }
    emit(out, op::ExecutionMode, {main_, kModeLocalSize, local_x_, local_y_, 1});
    out.insert(out.end(), debug_.begin(), debug_.end());
    out.insert(out.end(), annotations_.begin(), annotations_.end());
    out.insert(out.end(), globals_.begin(), globals_.end());
    emit(out, op::Function, {vt, main_, 0 /* None */, fnty});
    emit(out, op::Label, {entry});
    out.insert(out.end(), locals_.begin(), locals_.end());
    out.insert(out.end(), body_.begin(), body_.end());
    emit(out, op::Return, {});
    emit(out, op::FunctionEnd, {});
    out[3] = next_;
    return out;
}

Id Module::var(Id type) {
    const Id ptr = t_ptr(kStorageFunction, type);
    const Id v = fresh();
    emit(locals_, op::Variable, {ptr, v, kStorageFunction});
    return v;
}

V Module::load(Id ptr, Id type) {
    const V r{fresh(), type};
    emit(body_, op::Load, {type, r.id, ptr});
    return r;
}

void Module::store(Id ptr, V v) { emit(body_, op::Store, {ptr, v.id}); }

V Module::buf_load(Id buffer_var, V index) {
    const Id u = t_u32();
    const Id ptr = t_ptr(kStorageUniform, u);
    const Id chain = fresh();
    emit(body_, op::AccessChain, {ptr, chain, buffer_var, c_u32(0).id, index.id});
    return load(chain, u);
}

V Module::load_input_uvec3(Id v) { return load(v, t_vec(t_u32(), 3)); }

V Module::binop(std::uint16_t opcode, Id result_type, V a, V b) {
    const V r{fresh(), result_type};
    emit(body_, opcode, {result_type, r.id, a.id, b.id});
    return r;
}

V Module::add(V a, V b) { return binop(scalar_of(a.type) == Scalar::F32 ? op::FAdd : op::IAdd, a.type, a, b); }
V Module::sub(V a, V b) { return binop(scalar_of(a.type) == Scalar::F32 ? op::FSub : op::ISub, a.type, a, b); }
V Module::mul(V a, V b) { return binop(scalar_of(a.type) == Scalar::F32 ? op::FMul : op::IMul, a.type, a, b); }
V Module::div(V a, V b) { return binop(scalar_of(a.type) == Scalar::F32 ? op::FDiv : op::UDiv, a.type, a, b); }
V Module::umod(V a, V b) { return binop(op::UMod, a.type, a, b); }
V Module::neg(V a) {
    const V r{fresh(), a.type};
    emit(body_, op::FNegate, {a.type, r.id, a.id});
    return r;
}

V Module::cmp(std::uint16_t fop, std::uint16_t sop, std::uint16_t uop, V a, V b) {
    const Scalar s = scalar_of(a.type);
    const std::uint16_t o = s == Scalar::F32 ? fop : (s == Scalar::I32 ? sop : uop);
    return binop(o, t_bool(), a, b);
}
V Module::lt(V a, V b) { return cmp(op::FOrdLessThan, op::SLessThan, op::ULessThan, a, b); }
V Module::le(V a, V b) { return cmp(op::FOrdLessThanEqual, op::SLessThanEqual, op::ULessThanEqual, a, b); }
V Module::gt(V a, V b) { return cmp(op::FOrdGreaterThan, op::SGreaterThan, op::UGreaterThan, a, b); }
V Module::ge(V a, V b) { return cmp(op::FOrdGreaterThanEqual, op::SGreaterThanEqual, op::UGreaterThanEqual, a, b); }
V Module::eq(V a, V b) { return cmp(op::FOrdEqual, op::IEqual, op::IEqual, a, b); }
V Module::ne(V a, V b) { return cmp(op::FOrdNotEqual, op::INotEqual, op::INotEqual, a, b); }
V Module::land(V a, V b) { return binop(op::LogicalAnd, t_bool(), a, b); }
V Module::lor(V a, V b) { return binop(op::LogicalOr, t_bool(), a, b); }
V Module::lnot(V a) {
    const V r{fresh(), t_bool()};
    emit(body_, op::LogicalNot, {r.type, r.id, a.id});
    return r;
}
V Module::select(V c, V a, V b) {
    const V r{fresh(), a.type};
    emit(body_, op::Select, {a.type, r.id, c.id, a.id, b.id});
    return r;
}
V Module::band(V a, V b) { return binop(op::BitwiseAnd, a.type, a, b); }
V Module::bor(V a, V b) { return binop(op::BitwiseOr, a.type, a, b); }
V Module::shl(V a, V b) { return binop(op::ShiftLeftLogical, a.type, a, b); }
V Module::shr(V a, V b) { return binop(op::ShiftRightLogical, a.type, a, b); }
V Module::is_nan(V a) {
    const V r{fresh(), t_bool()};
    emit(body_, op::IsNan, {r.type, r.id, a.id});
    return r;
}
V Module::is_inf(V a) {
    const V r{fresh(), t_bool()};
    emit(body_, op::IsInf, {r.type, r.id, a.id});
    return r;
}

V Module::u2f(V a) {
    const V r{fresh(), t_f32()};
    emit(body_, op::ConvertUToF, {r.type, r.id, a.id});
    return r;
}
V Module::i2f(V a) {
    const V r{fresh(), t_f32()};
    emit(body_, op::ConvertSToF, {r.type, r.id, a.id});
    return r;
}
V Module::f2i(V a) {
    const V r{fresh(), t_i32()};
    emit(body_, op::ConvertFToS, {r.type, r.id, a.id});
    return r;
}
V Module::f2u(V a) {
    const V r{fresh(), t_u32()};
    emit(body_, op::ConvertFToU, {r.type, r.id, a.id});
    return r;
}
V Module::bitcast(Id type, V a) {
    const V r{fresh(), type};
    emit(body_, op::Bitcast, {type, r.id, a.id});
    return r;
}

V Module::ext(std::uint32_t inst, Id type, std::initializer_list<V> args) {
    const V r{fresh(), type};
    std::vector<std::uint32_t> w{type, r.id, glsl_, inst};
    for (const V& a : args) w.push_back(a.id);
    emit_v(body_, op::ExtInst, w);
    return r;
}

V Module::construct(Id type, std::initializer_list<V> parts) {
    const V r{fresh(), type};
    std::vector<std::uint32_t> w{type, r.id};
    for (const V& p : parts) w.push_back(p.id);
    emit_v(body_, op::CompositeConstruct, w);
    return r;
}

V Module::extract(Id type, V composite, std::uint32_t index) {
    const V r{fresh(), type};
    emit(body_, op::CompositeExtract, {type, r.id, composite.id, index});
    return r;
}

V Module::sample_lod0(Id sampled_image_var, V coord) {
    const Id img = t_image(t_f32(), 2, 1, 0);
    const Id si_t = t_sampled_image(img);
    const V si = load(sampled_image_var, si_t);
    const Id v4 = t_vec(t_f32(), 4);
    const V zero = c_f32(0.0f);
    const V r{fresh(), v4};
    emit(body_, op::ImageSampleExplicitLod, {v4, r.id, si.id, coord.id, kImageOperandLod, zero.id});
    return r;
}

V Module::fetch(Id sampled_image_var, V icoord) {
    const Id img = t_image(t_f32(), 2, 1, 0);
    const Id si_t = t_sampled_image(img);
    const V si = load(sampled_image_var, si_t);
    const V im{fresh(), img};
    emit(body_, op::Image, {img, im.id, si.id});
    const Id v4 = t_vec(t_f32(), 4);
    const V zero = c_i32(0);
    const V r{fresh(), v4};
    emit(body_, op::ImageFetch, {v4, r.id, im.id, icoord.id, kImageOperandLod, zero.id});
    return r;
}

void Module::image_write(Id storage_image_var, V icoord, V texel) {
    const Id img = t_image(t_u32(), 1, 2, 33);
    const V im = load(storage_image_var, img);
    emit(body_, op::ImageWrite, {im.id, icoord.id, texel.id});
}

void Module::label(Id l) { emit(body_, op::Label, {l}); }

void Module::if_(V cond, const std::function<void()>& then_fn, const std::function<void()>& else_fn) {
    const Id merge = fresh();
    const Id then_l = fresh();
    const Id else_l = else_fn ? fresh() : merge;
    emit(body_, op::SelectionMerge, {merge, 0});
    emit(body_, op::BranchConditional, {cond.id, then_l, else_l});
    label(then_l);
    then_fn();
    emit(body_, op::Branch, {merge});
    if (else_fn) {
        label(else_l);
        else_fn();
        emit(body_, op::Branch, {merge});
    }
    label(merge);
}

void Module::while_(const std::function<V()>& cond_fn, const std::function<void()>& body_fn) {
    const Id header = fresh(), cond_l = fresh(), body_l = fresh(), cont = fresh(), merge = fresh();
    emit(body_, op::Branch, {header});
    label(header);
    emit(body_, op::LoopMerge, {merge, cont, 0});
    emit(body_, op::Branch, {cond_l});
    label(cond_l);
    const V c = cond_fn();
    emit(body_, op::BranchConditional, {c.id, body_l, merge});
    label(body_l);
    body_fn();
    emit(body_, op::Branch, {cont});
    label(cont);
    emit(body_, op::Branch, {header});
    label(merge);
}

}  // namespace siaara::gpu::spirv
