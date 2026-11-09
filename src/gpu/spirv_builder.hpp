// SIAARA DICOM Toolkit - a small SPIR-V module builder for the GPU compute kernels (internal).
//
// Written from the SPIR-V specification (Khronos, unified, version 1.0 subset) and the
// GLSL.std.450 extended instruction set specification. Only what the kernels in
// spirv_kernels.cpp need is here: scalar and vector types, 3D sampled images, a 2D storage image,
// runtime-array storage buffers (Uniform storage class + BufferBlock, valid for every Vulkan
// version), function-local variables, structured selection and loops, and the arithmetic,
// conversion, comparison, image and extended instructions used.
//
// The module layout follows the specification's "Logical Layout of a Module" (2.4): capabilities,
// extended instruction imports, memory model, entry point, execution mode, debug names,
// annotations, types / constants / global variables, then the function. Local variables are
// collected separately and placed at the start of the entry block, as required for OpVariable
// with the Function storage class.
//
// Control flow is structured (2.11): if_() emits OpSelectionMerge + OpBranchConditional, while_()
// emits a header block with OpLoopMerge, a condition block, the body and a continue block that
// branches back to the header. No break / continue / phi: kernels keep state in local variables.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace siaara::gpu::spirv {

using Id = std::uint32_t;

// Opcodes (SPIR-V specification, 3.52 Instructions).
namespace op {
constexpr std::uint16_t Name = 5, ExtInstImport = 11, ExtInst = 12, MemoryModel = 14, EntryPoint = 15,
                        ExecutionMode = 16, Capability = 17, TypeVoid = 19, TypeBool = 20, TypeInt = 21,
                        TypeFloat = 22, TypeVector = 23, TypeImage = 25, TypeSampledImage = 27,
                        TypeRuntimeArray = 29, TypeStruct = 30, TypePointer = 32, TypeFunction = 33,
                        ConstantTrue = 41, ConstantFalse = 42, Constant = 43, Function = 54, FunctionEnd = 56,
                        Variable = 59, Load = 61, Store = 62, AccessChain = 65, Decorate = 71, MemberDecorate = 72,
                        CompositeConstruct = 80, CompositeExtract = 81, ImageSampleExplicitLod = 88,
                        ImageFetch = 95, ImageWrite = 99, Image = 100, ConvertFToU = 109, ConvertFToS = 110,
                        ConvertSToF = 111, ConvertUToF = 112, Bitcast = 124, FNegate = 127, IAdd = 128,
                        FAdd = 129, ISub = 130, FSub = 131, IMul = 132, FMul = 133, UDiv = 134, FDiv = 136,
                        UMod = 137, IsNan = 156, IsInf = 157, LogicalOr = 166, LogicalAnd = 167,
                        LogicalNot = 168, Select = 169, IEqual = 170, INotEqual = 171, UGreaterThan = 172,
                        SGreaterThan = 173, UGreaterThanEqual = 174, SGreaterThanEqual = 175, ULessThan = 176,
                        SLessThan = 177, ULessThanEqual = 178, SLessThanEqual = 179, FOrdEqual = 180,
                        FOrdNotEqual = 182, FOrdLessThan = 184, FOrdGreaterThan = 186, FOrdLessThanEqual = 188,
                        FOrdGreaterThanEqual = 190, ShiftRightLogical = 194, ShiftLeftLogical = 196,
                        BitwiseOr = 197, BitwiseAnd = 199, LoopMerge = 246, SelectionMerge = 247, Label = 248,
                        Branch = 249, BranchConditional = 250, Return = 253;
}  // namespace op

// GLSL.std.450 extended instructions.
namespace glsl {
constexpr std::uint32_t Round = 1, FAbs = 4, Floor = 8, Ceil = 9, Pow = 26, Sqrt = 31, FMin = 37, UMin = 38,
                        SMin = 39, FMax = 40, UMax = 41, SMax = 42, FClamp = 43, SClamp = 45;
}

enum class Scalar : std::uint8_t { None, Bool, U32, I32, F32 };

// A typed SSA value.
struct V {
    Id id = 0;
    Id type = 0;
};

class Module {
public:
    Module();

    // ---- types (deduplicated)
    Id t_void();
    Id t_bool();
    Id t_u32();
    Id t_i32();
    Id t_f32();
    Id t_vec(Id component, std::uint32_t n);
    Id t_ptr(std::uint32_t storage_class, Id pointee);
    Id t_image(Id sampled_type, std::uint32_t dim, std::uint32_t sampled, std::uint32_t format);
    Id t_sampled_image(Id image);
    Id t_func(Id ret);

    // ---- constants (deduplicated)
    V c_u32(std::uint32_t v);
    V c_i32(std::int32_t v);
    V c_f32(float v);
    V c_bool(bool v);

    // ---- globals
    // A storage buffer: struct { uint data[]; } decorated BufferBlock in the Uniform storage class,
    // at (set, binding). The kernels only read buffers.
    Id buffer_u32(std::uint32_t set, std::uint32_t binding, const char* name);
    // A combined image + sampler of a 3D float image.
    Id sampled_image_3d(std::uint32_t set, std::uint32_t binding, const char* name);
    // A 2D r32ui storage image (write only).
    Id storage_image_2d_r32ui(std::uint32_t set, std::uint32_t binding, const char* name);
    // The GlobalInvocationId built-in (uvec3 Input variable).
    Id global_invocation_id();

    // ---- function body (one entry point "main", GLCompute)
    void begin_main(std::uint32_t local_x, std::uint32_t local_y);
    std::vector<std::uint32_t> finish();

    // local variables (Function storage class)
    Id var(Id type);
    V load(Id ptr, Id type);
    void store(Id ptr, V v);

    // buffer element: data[index] of a buffer_u32 variable
    V buf_load(Id buffer_var, V index);

    // input built-in load
    V load_input_uvec3(Id var);

    // ---- arithmetic and logic (scalar types; type-dispatching)
    V add(V a, V b);
    V sub(V a, V b);
    V mul(V a, V b);
    V div(V a, V b);   // F32 or U32
    V umod(V a, V b);
    V neg(V a);        // F32
    V lt(V a, V b);
    V le(V a, V b);
    V gt(V a, V b);
    V ge(V a, V b);
    V eq(V a, V b);
    V ne(V a, V b);
    V land(V a, V b);
    V lor(V a, V b);
    V lnot(V a);
    V select(V cond, V a, V b);
    V band(V a, V b);
    V bor(V a, V b);
    V shl(V a, V b);
    V shr(V a, V b);
    V is_nan(V a);
    V is_inf(V a);
    // conversions
    V u2f(V a);
    V i2f(V a);
    V f2i(V a);
    V f2u(V a);
    V bitcast(Id type, V a);
    // GLSL.std.450
    V ext(std::uint32_t inst, Id type, std::initializer_list<V> args);
    V floor(V a) { return ext(glsl::Floor, a.type, {a}); }
    V ceil(V a) { return ext(glsl::Ceil, a.type, {a}); }
    V round(V a) { return ext(glsl::Round, a.type, {a}); }
    V fabs(V a) { return ext(glsl::FAbs, a.type, {a}); }
    V sqrt(V a) { return ext(glsl::Sqrt, a.type, {a}); }
    V pow(V a, V b) { return ext(glsl::Pow, a.type, {a, b}); }
    V fmin(V a, V b) { return ext(glsl::FMin, a.type, {a, b}); }
    V fmax(V a, V b) { return ext(glsl::FMax, a.type, {a, b}); }
    V smin(V a, V b) { return ext(glsl::SMin, a.type, {a, b}); }
    V smax(V a, V b) { return ext(glsl::SMax, a.type, {a, b}); }
    V umin(V a, V b) { return ext(glsl::UMin, a.type, {a, b}); }
    V sclamp(V x, V lo, V hi) { return ext(glsl::SClamp, x.type, {x, lo, hi}); }
    // composites
    V construct(Id type, std::initializer_list<V> parts);
    V extract(Id type, V composite, std::uint32_t index);
    // images
    V sample_lod0(Id sampled_image_var, V coord_vec3);           // vec4
    V fetch(Id sampled_image_var, V icoord_ivec3);                // vec4, Lod 0
    void image_write(Id storage_image_var, V icoord_ivec2, V texel_uvec4);

    // ---- structured control flow
    void if_(V cond, const std::function<void()>& then_fn, const std::function<void()>& else_fn = {});
    void while_(const std::function<V()>& cond_fn, const std::function<void()>& body_fn);

    Scalar scalar_of(Id type) const;
    std::uint32_t id_bound() const noexcept { return next_; }

private:
    Id fresh() { return next_++; }
    static void emit(std::vector<std::uint32_t>& s, std::uint16_t opcode, std::initializer_list<std::uint32_t> words);
    static void emit_v(std::vector<std::uint32_t>& s, std::uint16_t opcode, const std::vector<std::uint32_t>& words);
    static void append_string(std::vector<std::uint32_t>& w, const char* s);
    Id type_key(const std::vector<std::uint32_t>& key, std::uint16_t opcode, std::vector<std::uint32_t> operands);
    V binop(std::uint16_t opcode, Id result_type, V a, V b);
    V cmp(std::uint16_t fop, std::uint16_t sop, std::uint16_t uop, V a, V b);
    void label(Id l);
    void name(Id id, const char* n);

    Id next_ = 1;
    Id glsl_ = 0;
    Id main_ = 0;
    Id gid_ = 0;
    Id block_struct_ = 0;
    std::uint32_t local_x_ = 1, local_y_ = 1;
    std::vector<std::uint32_t> debug_, annotations_, globals_, locals_, body_;
    std::map<std::vector<std::uint32_t>, Id> types_;
    std::map<Id, Scalar> scalar_;
    std::map<std::vector<std::uint32_t>, V> consts_;
    std::vector<Id> interface_;
};

}  // namespace siaara::gpu::spirv
