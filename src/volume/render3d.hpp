// SIAARA DICOM Toolkit - 3D views of a volume: MPR, thick slabs, curved MPR, ray-cast volume
// rendering and two-layer fusion, rendered on the CPU into RGBA8 (multithreaded over tiles).
//
// Screen geometry. A Plane gives the centre of the output image (point), the viewing direction
// (normal: from the viewer into the scene) and an up vector; up is made orthogonal to normal.
// Screen x (columns, left to right) is right = normal x up, screen y (rows, top to bottom) is
// down = -up. Output pixel (u, v) is at
//     point + (u - (width - 1) / 2) * pixel_spacing * right + (v - (height - 1) / 2) * pixel_spacing * down.
// pixel_spacing 0 means "fit": the whole volume box fits in the output.
// standard_plane() gives the usual radiological axial / coronal / sagittal planes.
//
// Modes:
//  - Mpr: one plane; with a slab thickness > 0 the samples along the normal within
//    [-thickness/2, +thickness/2] (step: half the smallest voxel spacing, at least 2 samples)
//    are combined by MIP, MinIP or average.
//  - CurvedStraightened / CurvedStretched: curved MPR along a path (polyline, or a uniform
//    Catmull-Rom spline through the points). Columns run along the path (column 0 = path start,
//    pixel_spacing mm per column); rows are offsets from the path, row (height - 1) / 2 on it.
//    Straightened: the offset direction at each path point is `down` made orthogonal to the path
//    tangent, columns are spaced by 3D arc length (the path appears as a straight line).
//    Stretched: the offset direction is the fixed `down` vector, columns are spaced by the arc
//    length of the path projected on the plane orthogonal to `down` (isometric in that plane).
//  - VolumeRender: orthographic ray casting along normal, front to back. Transfer function:
//    piecewise-linear opacity and colour on value, optional gradient-magnitude opacity factor,
//    opacity corrected for the sample distance, early ray termination, empty-space skipping with
//    a min/max brick grid, Phong shading (headlight) from central-difference gradients.
//  - VolumeMip / VolumeMinIp: maximum / minimum of the trilinear samples along each ray.
// Quality::Interactive renders at 1/interactive_downsample resolution (bilinear upsampling) with
// interactive_sample_factor x the sample distance; Final at full resolution and full sampling.
//
// Fusion: an optional second layer (e.g. PET over CT) with its own window and colour map is
// blended over the base layer with its opacity (values at or below the window bottom are left
// transparent when transparent_below_window). The overlay is sampled at exactly the base layer's
// sample points, i.e. resampled into the base geometry - on the fly by default, or once up front
// (set_fusion_layer(..., preresample = true), which costs a base-sized float volume; points
// outside the overlay then take the overlay's minimum value, so they are transparent with
// transparent_below_window, and pick reports them as inside the overlay). Fusion
// applies to MPR, slab and curved modes; the volume rendering modes show the base layer only.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/status.hpp"
#include "volume/geometry.hpp"
#include "volume/sampler.hpp"
#include "volume/volume.hpp"

namespace siaara::gpu {
struct ViewGpu;  // GPU state of a view (src/gpu/gpu_view.cpp)
}

namespace siaara::volume {

// Where View::render_rgba8 renders. Cpu (the default) is the reference renderer below. Auto uses a
// hardware GPU (Vulkan or Metal, src/gpu) when one is present and the view's mode is supported
// there, else the CPU; Gpu also accepts a software (CPU-type) GPU device. Both fall back to the
// CPU renderer whenever the GPU cannot render a frame; last_backend() tells which one did.
enum class RenderBackend : std::uint8_t { Auto, Cpu, Gpu };

enum class ViewMode : std::uint8_t { Mpr, CurvedStraightened, CurvedStretched, VolumeRender, VolumeMip, VolumeMinIp };
enum class SlabMode : std::uint8_t { None, Mip, MinIp, Average };
enum class Quality : std::uint8_t { Interactive, Final };
enum class ColorMap : std::uint8_t { Gray, InverseGray, HotIron, Pet, HotMetalBlue, Pet20Step };
enum class StandardOrientation : std::uint8_t { Axial, Coronal, Sagittal };

struct Plane {
    Vec3 point;
    Vec3 normal{0, 0, 1};
    Vec3 up{0, -1, 0};
    double pixel_spacing = 0.0;  // mm per output pixel; 0 = fit the volume
};

// Patient-based planes through the volume centre (LPS): axial looks from the feet (normal +S,
// up = anterior), coronal from the front (normal +P, up = +S), sagittal from the patient's left
// (normal -L, i.e. towards the right; up = +S). pixel_spacing 0 (fit).
Plane standard_plane(const Grid& g, StandardOrientation o);
// A plane through voxel centres of the grid: axis 2 shows slice k = index (right = axes[0],
// down = axes[1]); axis 1 the j = index plane (right = axes[0], up = axes[2]); axis 0 the i = index
// plane (right = axes[1], up = axes[2]). pixel_spacing = the smaller in-plane spacing; with an
// output size equal to the grid's in-plane dims and equal in-plane spacings, output pixels land
// exactly on voxel centres.
Plane grid_plane(const Grid& g, int axis, double index);

struct ValuePoint {
    double value = 0.0;
    double y = 0.0;  // opacity (0..1) or factor (0..1)
};
struct ColorPoint {
    double value = 0.0;
    double r = 1.0, g = 1.0, b = 1.0;  // 0..1
};
// Piecewise-linear function through sorted points, constant beyond the ends (0 when empty).
double evaluate_piecewise(std::span<const ValuePoint> pts, double x) noexcept;

struct TransferFunction {
    std::vector<ValuePoint> opacity;           // value -> opacity per unit_distance
    std::vector<ColorPoint> color;             // value -> colour (empty: white)
    std::vector<ValuePoint> gradient_opacity;  // |gradient| (value units per mm) -> factor; empty: off
    bool shading = true;
    double ambient = 0.25, diffuse = 0.7, specular = 0.25, shininess = 24.0;
    double unit_distance = 1.0;                // mm
};

struct LayerStyle {
    double window_center = 0.0;
    double window_width = 0.0;   // 0: the layer's full value range
    ColorMap colormap = ColorMap::Gray;
    double opacity = 1.0;        // overlay blending weight (base layer: ignored)
    bool transparent_below_window = true;  // overlay only
};

struct RenderSettings {
    double sample_distance = 0.0;          // mm; 0 = half the smallest voxel spacing
    double interactive_sample_factor = 2.0;
    std::uint32_t interactive_downsample = 2;
    double early_termination = 0.99;       // accumulated opacity that ends a ray (> 1: never)
    std::array<std::uint8_t, 3> background{0, 0, 0};
    bool empty_space_skipping = true;
    std::uint32_t brick_size = 8;
    unsigned threads = 0;                  // 0 = hardware concurrency
};

struct PickResult {
    bool hit = false;        // the screen point maps to a patient point (VR: a surface was hit)
    Vec3 patient;            // mm, LPS
    Vec3 index;              // continuous voxel index in the base volume
    bool inside = false;     // inside the base volume
    double value = 0.0;      // base value at the point (view interpolation; slab: the slab result)
    bool overlay_inside = false;
    double overlay_value = 0.0;
};

class View {
public:
    View() = default;
    View(View&&) noexcept = default;
    View& operator=(View&&) noexcept = default;

    static Result<View> create(std::shared_ptr<const Volume> base, std::uint32_t width, std::uint32_t height);

    Status set_size(std::uint32_t width, std::uint32_t height);
    Status set_plane(const Plane& p);
    Status set_mode(ViewMode mode, Interpolation interp = Interpolation::Linear, SlabMode slab = SlabMode::None,
                    double slab_thickness = 0.0);
    void set_quality(Quality q) noexcept { quality_ = q; }
    Status set_render_settings(const RenderSettings& s);
    // layer 0 = base, 1 = fusion overlay.
    Status set_window(int layer, double center, double width);
    Status set_colormap(int layer, ColorMap map);
    Status set_transfer_function(const TransferFunction& tf);
    // overlay == nullptr removes the layer.
    Status set_fusion_layer(std::shared_ptr<const Volume> overlay, const LayerStyle& style, bool preresample = false);
    Status set_curve(std::span<const Vec3> points, bool spline);

    std::uint32_t width() const noexcept { return w_; }
    std::uint32_t height() const noexcept { return h_; }
    const Plane& plane() const noexcept { return plane_; }
    double pixel_spacing() const noexcept { return ps_; }
    Vec3 right() const noexcept { return right_; }
    Vec3 down() const noexcept { return down_; }
    Vec3 normal() const noexcept { return n_; }
    ViewMode mode() const noexcept { return mode_; }
    Quality quality() const noexcept { return quality_; }
    double sample_distance(Quality q) const noexcept;
    const std::shared_ptr<const Volume>& base() const noexcept { return base_; }

    // width * height * 4 bytes, rows top to bottom. Thread-safe (const, no shared mutable state).
    Status render_rgba8(std::span<std::uint8_t> out) const;
    // Base layer values before windowing (MPR / slab, curved, VolumeMip / VolumeMinIp), always at
    // full resolution; `outside` where there is no sample.
    Status render_values(std::span<float> out, float outside) const;
    // Screen point (output pixel coordinates, may be fractional) -> patient point and values.
    Result<PickResult> pick(double x, double y) const;
    // Patient point -> screen point (x, y) and depth along the normal (planar modes only).
    Result<Vec3> project(Vec3 patient) const;

    // GPU rendering (see RenderBackend). set_backend(Gpu) fails with Unsupported when no GPU
    // device is available; Auto never fails.
    Status set_backend(RenderBackend b);
    RenderBackend backend() const noexcept { return backend_; }
    // Cpu or Gpu: the renderer of the last render_rgba8 call (Cpu before the first).
    RenderBackend last_backend() const noexcept;

    struct Bricks {  // min/max brick grid
        std::uint32_t size = 8;
        std::array<std::uint32_t, 3> nb{0, 0, 0};
        std::vector<float> mn, mx;
        std::size_t index(std::uint32_t bx, std::uint32_t by, std::uint32_t bz) const noexcept {
            return (static_cast<std::size_t>(bz) * nb[1] + by) * nb[0] + bx;
        }
    };
    friend struct RenderAccess;
    friend struct GpuAccess;

private:
    struct Overlay {
        std::shared_ptr<const Volume> vol;
        GridTransform t;
        LayerStyle style;
    };
    struct TfTable {
        double lo = 0.0, scale = 0.0;  // entry = (value - lo) * scale
        std::vector<float> r, g, b, a_final, a_inter;
        std::vector<std::uint32_t> nonzero_prefix;  // count of entries with opacity > 0 before e
    };

    Status update_plane();
    Status prepare_volume_rendering();
    void prepare_tf_alpha();

    std::shared_ptr<const Volume> base_;
    GridTransform bt_;
    std::uint32_t w_ = 0, h_ = 0;
    Plane plane_;
    Vec3 right_, down_, n_;
    double ps_ = 1.0;
    ViewMode mode_ = ViewMode::Mpr;
    Interpolation interp_ = Interpolation::Linear;
    SlabMode slab_ = SlabMode::None;
    double slab_thickness_ = 0.0;
    Quality quality_ = Quality::Final;
    RenderSettings rs_;
    LayerStyle base_style_;
    std::optional<Overlay> overlay_;
    TransferFunction tf_;
    TfTable tft_;
    std::shared_ptr<const Bricks> bricks_;
    std::vector<std::uint8_t> brick_empty_;
    std::vector<Vec3> curve_;
    bool curve_spline_ = false;
    RenderBackend backend_ = RenderBackend::Cpu;
    std::shared_ptr<gpu::ViewGpu> gpu_;
};

namespace detail {
// The CPU renderer's window mapping of a layer, t = clamp(value * a + b, 0, 1), and colour map
// entries; the GPU backends build their tables from these so that both renderers agree.
void window_coefficients(const LayerStyle& s, const Volume& v, double& a, double& b) noexcept;
std::array<std::uint8_t, 3> colormap_rgb(ColorMap m, std::uint32_t entry) noexcept;
}  // namespace detail

}  // namespace siaara::volume
