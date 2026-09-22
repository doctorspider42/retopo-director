#pragma once

// The only way the director ever sees the model.
//
// One GPU path serves both jobs: the interactive viewport and the offscreen
// captures that go into the prompt. Using the same code for both means what
// the artist inspects and what the model judges cannot drift apart.

#include "bake/texture.h"
#include "core/math.h"
#include "mesh/mesh.h"
#include "render/camera.h"
#include "segment/segment.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rd {

enum class RenderMode : uint8_t {
    Shaded = 0,      // the baked light rig, plus vertex colours
    Regions,         // flat per region colour
    Silhouette,      // white on black, for the outline metric
    Normals,         // world space normals
    Curvature,       // analysis curvature ramp
    Checker,         // uv checker, to spot stretching
    Plain            // untextured clay, the best read of pure form
};

const char* render_mode_name(RenderMode m);

// GPU copy of a mesh. `attribute` carries whatever the current mode needs
// (region colour, curvature, baked vertex colour), uploaded separately so the
// mode can change without re-uploading geometry.
class GpuMesh {
public:
    ~GpuMesh();
    GpuMesh() = default;
    GpuMesh(const GpuMesh&)            = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    void upload(const Mesh& mesh);
    void upload_attribute(const std::vector<Vec4>& per_vertex_color);
    // Convenience: expands per triangle colours (region overlay) onto vertices.
    void upload_face_attribute(const Mesh& mesh, const std::vector<Vec4>& per_face_color);
    void set_texture(const Texture& tex);
    void clear_texture();
    void release();

    bool   valid() const { return vao_ != 0 && index_count_ > 0; }
    size_t index_count() const { return index_count_; }
    Aabb   bounds() const { return bounds_; }
    bool   has_texture() const { return texture_ != 0; }
    // True when the uploaded mesh carried colours that already contain
    // baked lighting, which the shaded view must not light again.
    bool   has_baked_colors() const { return baked_colors_; }
    void   set_baked_colors(bool on) { baked_colors_ = on; }

    void draw() const;
    void draw_wireframe() const;
    void bind_texture(int unit) const;

private:
    void ensure_buffers();

    uint32_t vao_        = 0;
    uint32_t vbo_        = 0;
    uint32_t ebo_        = 0;
    uint32_t texture_    = 0;
    size_t   index_count_ = 0;
    size_t   vertex_count_ = 0;
    bool     baked_colors_ = false;
    Aabb     bounds_;
};

struct RenderOptions {
    int        width      = 768;
    int        height     = 768;
    int        samples    = 4;
    RenderMode mode       = RenderMode::Shaded;
    Vec3       background{0.129f, 0.137f, 0.157f};
    bool       wireframe_overlay = false;
    Vec3       wireframe_color{0.05f, 0.05f, 0.07f};
    float      wireframe_alpha = 0.35f;
    bool       flip_y          = true;   // PNG rows run top down
    // Silhouette mode renders white on black regardless of the background.
    bool       backface_cull   = true;
};

class Renderer {
public:
    ~Renderer();

    bool init(std::string* error = nullptr);
    void shutdown();
    bool ready() const { return program_ != 0; }

    // Renders into an internal framebuffer and reads the result back.
    bool render_to_texture(const GpuMesh& mesh, const ViewCamera& camera,
                           const RenderOptions& opts, Texture& out);

    // Renders into an internal framebuffer and leaves the resolved GL texture
    // for ImGui to display. Returns 0 on failure.
    uint32_t render_to_gl_texture(const GpuMesh& mesh, const ViewCamera& camera,
                                  const RenderOptions& opts);

    // Coverage mask for the silhouette metric: 1 where the model covered a pixel.
    bool render_mask(const GpuMesh& mesh, const ViewCamera& camera, int width, int height,
                     std::vector<uint8_t>& out);

private:
    struct Target {
        uint32_t fbo_ms = 0, color_ms = 0, depth_ms = 0;
        uint32_t fbo    = 0, color    = 0;
        int      width = 0, height = 0, samples = 0;
        void release();
    };

    bool ensure_target(int width, int height, int samples);
    void draw_scene(const GpuMesh& mesh, const ViewCamera& camera, const RenderOptions& opts);

    uint32_t program_ = 0;
    Target   target_;

    int u_view_proj_ = -1, u_model_ = -1, u_mode_ = -1, u_camera_ = -1;
    int u_color_ = -1, u_alpha_ = -1, u_texture_ = -1, u_use_texture_ = -1;
    int u_prelit_ = -1;
    int u_checker_scale_ = -1;
};

// Reads the currently bound draw buffer back into a Texture, flipping it so row
// zero is the top. Used for window screenshots: it reads the back buffer before
// the swap, so it works whether or not the window has focus, and whether or not
// something else is sitting on top of it.
bool capture_framebuffer(int width, int height, Texture& out);

// Convenience wrapper for the pipeline: renders every camera in the rig and
// writes the PNGs, returning the file list in camera order.
struct ViewSet {
    struct Entry {
        std::string           name;
        std::string           description;
        std::filesystem::path file;
        bool                  primary = false;
        float                 weight  = 1.0f;
        std::vector<uint8_t>  mask;      // silhouette coverage, width*height
        int                   mask_width = 0, mask_height = 0;
    };
    std::vector<Entry> entries;
    int  width = 0, height = 0;
    bool ok = false;
    std::string error;
};

ViewSet render_view_set(Renderer& renderer, const Mesh& mesh,
                        const std::vector<ViewCamera>& cameras,
                        const RenderOptions& opts,
                        const std::filesystem::path& directory,
                        const std::string& prefix,
                        const std::vector<Vec4>* per_face_color = nullptr,
                        const Texture* diffuse = nullptr,
                        bool also_capture_masks = true);

// Mean and worst silhouette error between two view sets, expressed as the
// fraction of frame pixels where the two outlines disagree.
struct SilhouetteError {
    float mean  = 0.0f;
    float worst = 0.0f;
    std::string worst_view;
    std::vector<std::pair<std::string, float>> per_view;
};
SilhouetteError compare_silhouettes(const ViewSet& reference, const ViewSet& candidate);

} // namespace rd
