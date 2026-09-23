#include "render/renderer.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "render/gl.h"

#include <algorithm>
#include <cstring>

namespace rd {
namespace {

struct GpuVertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b, a;
    float u, v;
};

const char* kVertexShader = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUv;

uniform mat4 uViewProj;
uniform mat4 uModel;

out vec3 vWorld;
out vec3 vNormal;
out vec4 vColor;
out vec2 vUv;

void main()
{
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld  = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vColor  = aColor;
    vUv     = aUv;
    gl_Position = uViewProj * world;
}
)GLSL";

// The shaded mode mirrors bake/bake.cpp's light rig on purpose: what the
// viewport shows is what the baker will burn into the diffuse.
const char* kFragmentShader = R"GLSL(
#version 330 core
in vec3 vWorld;
in vec3 vNormal;
in vec4 vColor;
in vec2 vUv;

uniform int   uMode;
uniform vec3  uCamera;
uniform vec4  uColor;
uniform float uAlpha;
uniform sampler2D uTexture;
uniform int   uUseTexture;
uniform int   uPrelit;        // 1 when the colours already carry baked light
uniform float uCheckerScale;

out vec4 fragColor;

vec3 lightRig(vec3 n)
{
    vec3 key  = normalize(vec3(-0.45, 0.75, 0.50));
    vec3 fill = normalize(vec3( 0.70, 0.20, 0.35));
    vec3 rim  = normalize(vec3( 0.05,-0.80,-0.40));

    vec3 c = vec3(0.0);
    c += vec3(1.00, 0.97, 0.90) * 0.62 * max(dot(n, key),  0.0);
    c += vec3(0.62, 0.70, 0.85) * 0.24 * max(dot(n, fill), 0.0);
    c += vec3(0.85, 0.75, 0.62) * 0.14 * max(dot(n, rim),  0.0);
    c += vec3(0.17, 0.19, 0.23);
    return c;
}

vec3 curvatureRamp(float t)
{
    // blue (flat) -> green -> yellow -> red (creased)
    vec3 a = vec3(0.16, 0.34, 0.68);
    vec3 b = vec3(0.20, 0.70, 0.45);
    vec3 c = vec3(0.95, 0.80, 0.25);
    vec3 d = vec3(0.90, 0.25, 0.20);
    if (t < 0.33) return mix(a, b, t / 0.33);
    if (t < 0.66) return mix(b, c, (t - 0.33) / 0.33);
    return mix(c, d, clamp((t - 0.66) / 0.34, 0.0, 1.0));
}

void main()
{
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;

    if (uMode == 2) {                       // silhouette
        fragColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    if (uMode == 6) {                       // flat wire colour
        fragColor = vec4(uColor.rgb, uAlpha);
        return;
    }
    if (uMode == 1) {                       // regions
        vec3 shade = vec3(0.45) + 0.55 * max(dot(n, normalize(vec3(-0.3, 0.8, 0.5))), 0.0);
        fragColor = vec4(vColor.rgb * shade, 1.0);
        return;
    }
    if (uMode == 3) {                       // normals
        fragColor = vec4(n * 0.5 + 0.5, 1.0);
        return;
    }
    if (uMode == 4) {                       // curvature
        fragColor = vec4(curvatureRamp(clamp(vColor.r, 0.0, 1.0)), 1.0);
        return;
    }
    if (uMode == 5) {                       // uv checker
        vec2 t = vUv * uCheckerScale;
        float c = mod(floor(t.x) + floor(t.y), 2.0);
        vec3 base = mix(vec3(0.20, 0.22, 0.26), vec3(0.82, 0.84, 0.88), c);
        fragColor = vec4(base * lightRig(n) * 0.8, 1.0);
        return;
    }

    // Shaded mode shows what the console would actually put on screen. Once the
    // bake has run, the lighting is already in the pixels; running the rig over
    // it a second time just clips everything to white.
    if (uMode == 0) {
        if (uUseTexture == 1) { fragColor = vec4(texture(uTexture, vUv).rgb, 1.0); return; }
        if (uPrelit == 1)     { fragColor = vec4(vColor.rgb, 1.0); return; }
        fragColor = vec4(vColor.rgb * lightRig(n), 1.0);
        return;
    }
    fragColor = vec4(vec3(0.78) * lightRig(n), 1.0);
}
)GLSL";

uint32_t compile(uint32_t type, const char* source, std::string* error)
{
    const uint32_t shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(std::max(len, 1)), '\0');
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        if (error) *error = log;
        RD_ERROR("shader compile failed: %s", log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int mode_index(RenderMode m)
{
    switch (m) {
    case RenderMode::Regions:    return 1;
    case RenderMode::Silhouette: return 2;
    case RenderMode::Normals:    return 3;
    case RenderMode::Curvature:  return 4;
    case RenderMode::Checker:    return 5;
    case RenderMode::Plain:      return 7;
    default:                     return 0;
    }
}

} // namespace

const char* render_mode_name(RenderMode m)
{
    switch (m) {
    case RenderMode::Regions:    return "Regions";
    case RenderMode::Silhouette: return "Silhouette";
    case RenderMode::Normals:    return "Normals";
    case RenderMode::Curvature:  return "Curvature";
    case RenderMode::Checker:    return "UV checker";
    case RenderMode::Plain:      return "Clay";
    default:                     return "Shaded";
    }
}

// ---------------------------------------------------------------------------
// GpuMesh
// ---------------------------------------------------------------------------
GpuMesh::~GpuMesh() { release(); }

void GpuMesh::release()
{
    if (!gl::loaded()) { vao_ = vbo_ = ebo_ = texture_ = 0; return; }
    if (vao_)     glDeleteVertexArrays(1, &vao_);
    if (vbo_)     glDeleteBuffers(1, &vbo_);
    if (ebo_)     glDeleteBuffers(1, &ebo_);
    if (texture_) glDeleteTextures(1, &texture_);
    vao_ = vbo_ = ebo_ = texture_ = 0;
    index_count_ = vertex_count_ = 0;
    baked_colors_ = false;
}

void GpuMesh::ensure_buffers()
{
    if (vao_) return;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
}

void GpuMesh::upload(const Mesh& mesh)
{
    if (!gl::loaded() || mesh.empty()) return;
    ensure_buffers();

    std::vector<GpuVertex> verts(mesh.vertex_count());
    for (size_t i = 0; i < verts.size(); ++i) {
        GpuVertex& v = verts[i];
        v.px = mesh.positions[i].x; v.py = mesh.positions[i].y; v.pz = mesh.positions[i].z;
        if (mesh.has_normals()) { v.nx = mesh.normals[i].x; v.ny = mesh.normals[i].y; v.nz = mesh.normals[i].z; }
        else                    { v.nx = 0; v.ny = 1; v.nz = 0; }
        if (mesh.has_colors()) { v.r = mesh.colors[i].x; v.g = mesh.colors[i].y;
                                 v.b = mesh.colors[i].z; v.a = mesh.colors[i].w; }
        else                   { v.r = v.g = v.b = 0.78f; v.a = 1.0f; }
        if (mesh.has_uvs()) { v.u = mesh.uvs[i].x; v.v = mesh.uvs[i].y; }
        else                { v.u = v.v = 0.0f; }
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(GpuVertex)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(mesh.indices.size() * sizeof(uint32_t)),
                 mesh.indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = GLsizei(sizeof(GpuVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, px));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, nx));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, u));
    glBindVertexArray(0);

    index_count_  = mesh.indices.size();
    vertex_count_ = verts.size();
    bounds_       = mesh.bounds();
    baked_colors_ = mesh.has_colors() && mesh.colors_prelit;
}

void GpuMesh::upload_attribute(const std::vector<Vec4>& colors)
{
    if (!gl::loaded() || !vbo_ || colors.size() != vertex_count_) return;

    std::vector<GpuVertex> scratch(vertex_count_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // Patching colour in place avoids re-uploading positions every time the
    // viewport switches mode, which happens constantly while browsing regions.
    for (size_t i = 0; i < colors.size(); ++i) {
        const float rgba[4] = {colors[i].x, colors[i].y, colors[i].z, colors[i].w};
        glBufferSubData(GL_ARRAY_BUFFER,
                        GLintptr(i * sizeof(GpuVertex) + offsetof(GpuVertex, r)),
                        GLsizeiptr(sizeof(rgba)), rgba);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GpuMesh::upload_face_attribute(const Mesh& mesh, const std::vector<Vec4>& face_color)
{
    if (mesh.empty() || face_color.size() != mesh.triangle_count()) return;

    // Vertices shared between regions get the colour of the last face that
    // claims them; the overlay is a guide, not a precise readout.
    std::vector<Vec4> per_vertex(mesh.vertex_count(), Vec4{0.5f, 0.5f, 0.5f, 1.0f});
    for (size_t t = 0; t < mesh.triangle_count(); ++t)
        for (int c = 0; c < 3; ++c)
            per_vertex[mesh.indices[t * 3 + c]] = face_color[t];
    upload_attribute(per_vertex);
}

void GpuMesh::set_texture(const Texture& tex)
{
    if (!gl::loaded() || tex.empty()) return;
    if (!texture_) glGenTextures(1, &texture_);

    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLenum format = tex.channels == 4 ? GL_RGBA : (tex.channels == 3 ? GL_RGB : GL_LUMINANCE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex.width, tex.height, 0, format,
                 GL_UNSIGNED_BYTE, tex.pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GpuMesh::clear_texture()
{
    if (texture_ && gl::loaded()) glDeleteTextures(1, &texture_);
    texture_ = 0;
}

void GpuMesh::bind_texture(int unit) const
{
    if (!texture_) return;
    glActiveTexture(GL_TEXTURE0 + GLenum(unit));
    glBindTexture(GL_TEXTURE_2D, texture_);
}

void GpuMesh::draw() const
{
    if (!valid()) return;
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, GLsizei(index_count_), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void GpuMesh::draw_wireframe() const
{
    if (!valid()) return;
    glBindVertexArray(vao_);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glDrawElements(GL_TRIANGLES, GLsizei(index_count_), GL_UNSIGNED_INT, nullptr);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
Renderer::~Renderer() { shutdown(); }

void Renderer::Target::release()
{
    if (!gl::loaded()) { fbo_ms = color_ms = depth_ms = fbo = color = 0; return; }
    if (fbo_ms)   glDeleteFramebuffers(1, &fbo_ms);
    if (color_ms) glDeleteRenderbuffers(1, &color_ms);
    if (depth_ms) glDeleteRenderbuffers(1, &depth_ms);
    if (fbo)      glDeleteFramebuffers(1, &fbo);
    if (color)    glDeleteTextures(1, &color);
    fbo_ms = color_ms = depth_ms = fbo = color = 0;
    width = height = samples = 0;
}

bool Renderer::init(std::string* error)
{
    if (!gl::loaded() && !gl::load()) {
        if (error) *error = gl::last_load_error();
        return false;
    }
    if (program_) return true;

    std::string log;
    const uint32_t vs = compile(GL_VERTEX_SHADER, kVertexShader, &log);
    if (!vs) { if (error) *error = "vertex shader: " + log; return false; }
    const uint32_t fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, &log);
    if (!fs) { glDeleteShader(vs); if (error) *error = "fragment shader: " + log; return false; }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &len);
        std::string link_log(size_t(std::max(len, 1)), '\0');
        glGetProgramInfoLog(program_, len, nullptr, link_log.data());
        if (error) *error = "link: " + link_log;
        RD_ERROR("program link failed: %s", link_log.c_str());
        glDeleteProgram(program_);
        program_ = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!program_) return false;

    u_view_proj_     = glGetUniformLocation(program_, "uViewProj");
    u_model_         = glGetUniformLocation(program_, "uModel");
    u_mode_          = glGetUniformLocation(program_, "uMode");
    u_camera_        = glGetUniformLocation(program_, "uCamera");
    u_color_         = glGetUniformLocation(program_, "uColor");
    u_alpha_         = glGetUniformLocation(program_, "uAlpha");
    u_texture_       = glGetUniformLocation(program_, "uTexture");
    u_use_texture_   = glGetUniformLocation(program_, "uUseTexture");
    u_prelit_        = glGetUniformLocation(program_, "uPrelit");
    u_checker_scale_ = glGetUniformLocation(program_, "uCheckerScale");
    return true;
}

void Renderer::shutdown()
{
    target_.release();
    if (program_ && gl::loaded()) glDeleteProgram(program_);
    program_ = 0;
}

bool Renderer::ensure_target(int width, int height, int samples)
{
    width  = std::clamp(width, 8, 8192);
    height = std::clamp(height, 8, 8192);
    samples = std::clamp(samples, 0, 8);

    if (target_.width == width && target_.height == height && target_.samples == samples)
        return true;
    target_.release();

    // Resolve target: a plain texture we can read back or hand to ImGui.
    glGenTextures(1, &target_.color);
    glBindTexture(GL_TEXTURE_2D, target_.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &target_.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target_.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_.color, 0);

    if (samples > 0) {
        glGenFramebuffers(1, &target_.fbo_ms);
        glBindFramebuffer(GL_FRAMEBUFFER, target_.fbo_ms);

        glGenRenderbuffers(1, &target_.color_ms);
        glBindRenderbuffer(GL_RENDERBUFFER, target_.color_ms);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  target_.color_ms);

        glGenRenderbuffers(1, &target_.depth_ms);
        glBindRenderbuffer(GL_RENDERBUFFER, target_.depth_ms);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8,
                                         width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  target_.depth_ms);
    } else {
        glGenRenderbuffers(1, &target_.depth_ms);
        glBindRenderbuffer(GL_RENDERBUFFER, target_.depth_ms);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, target_.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  target_.depth_ms);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        RD_ERROR("framebuffer incomplete (0x%04x) at %dx%d, %d samples",
                 unsigned(status), width, height, samples);
        target_.release();
        return false;
    }

    target_.width   = width;
    target_.height  = height;
    target_.samples = samples;
    return true;
}

void Renderer::draw_scene(const GpuMesh& mesh, const ViewCamera& camera,
                          const RenderOptions& opts)
{
    const float aspect = float(target_.width) / float(std::max(1, target_.height));
    const Mat4  vp     = camera.view_proj(aspect);
    const Mat4  model  = Mat4::identity();

    const bool mask_mode = opts.mode == RenderMode::Silhouette;
    const Vec3 bg = mask_mode ? Vec3{0.0f, 0.0f, 0.0f} : opts.background;

    glViewport(0, 0, target_.width, target_.height);
    glClearColor(bg.x, bg.y, bg.z, mask_mode ? 0.0f : 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (opts.backface_cull && !mask_mode) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (target_.samples > 0) glEnable(GL_MULTISAMPLE);

    glUseProgram(program_);
    glUniformMatrix4fv(u_view_proj_, 1, GL_FALSE, vp.m);
    glUniformMatrix4fv(u_model_, 1, GL_FALSE, model.m);
    glUniform3fv(u_camera_, 1, &camera.eye.x);
    glUniform1i(u_mode_, mode_index(opts.mode));
    glUniform1f(u_alpha_, 1.0f);
    glUniform1f(u_checker_scale_, 24.0f);

    const bool use_texture = opts.mode == RenderMode::Shaded && mesh.has_texture();
    glUniform1i(u_use_texture_, use_texture ? 1 : 0);
    glUniform1i(u_prelit_, mesh.has_baked_colors() ? 1 : 0);
    if (use_texture) {
        mesh.bind_texture(0);
        glUniform1i(u_texture_, 0);
    }

    const Vec4 white{1, 1, 1, 1};
    glUniform4fv(u_color_, 1, &white.x);
    mesh.draw();

    if (opts.wireframe_overlay && !mask_mode) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0f, -1.0f);
        glUniform1i(u_mode_, 6);
        const Vec4 wire{opts.wireframe_color.x, opts.wireframe_color.y,
                        opts.wireframe_color.z, 1.0f};
        glUniform4fv(u_color_, 1, &wire.x);
        glUniform1f(u_alpha_, opts.wireframe_alpha);
        mesh.draw_wireframe();
        glDisable(GL_POLYGON_OFFSET_LINE);
        glDisable(GL_BLEND);
    }

    glUseProgram(0);
}

uint32_t Renderer::render_to_gl_texture(const GpuMesh& mesh, const ViewCamera& camera,
                                        const RenderOptions& opts)
{
    if (!ready() || !mesh.valid()) return 0;
    if (!ensure_target(opts.width, opts.height, opts.samples)) return 0;

    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);

    const uint32_t draw_fbo = target_.samples > 0 ? target_.fbo_ms : target_.fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    draw_scene(mesh, camera, opts);

    if (target_.samples > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target_.fbo_ms);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_.fbo);
        glBlitFramebuffer(0, 0, target_.width, target_.height,
                          0, 0, target_.width, target_.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous));
    return target_.color;
}

bool Renderer::render_to_texture(const GpuMesh& mesh, const ViewCamera& camera,
                                 const RenderOptions& opts, Texture& out)
{
    if (!render_to_gl_texture(mesh, camera, opts)) return false;

    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_FRAMEBUFFER, target_.fbo);

    out.resize(target_.width, target_.height, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, target_.width, target_.height, GL_RGBA, GL_UNSIGNED_BYTE,
                 out.pixels.data());

    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous));

    if (opts.flip_y) {
        const size_t row = size_t(out.width) * 4;
        std::vector<uint8_t> scratch(row);
        for (int y = 0; y < out.height / 2; ++y) {
            uint8_t* a = out.pixels.data() + size_t(y) * row;
            uint8_t* b = out.pixels.data() + size_t(out.height - 1 - y) * row;
            std::memcpy(scratch.data(), a, row);
            std::memcpy(a, b, row);
            std::memcpy(b, scratch.data(), row);
        }
    }
    return true;
}

bool Renderer::render_mask(const GpuMesh& mesh, const ViewCamera& camera, int width,
                           int height, std::vector<uint8_t>& out)
{
    RenderOptions opts;
    opts.width   = width;
    opts.height  = height;
    opts.samples = 0;            // a hard edge is what the metric wants
    opts.mode    = RenderMode::Silhouette;
    opts.wireframe_overlay = false;
    opts.backface_cull     = false;
    opts.flip_y            = false;

    Texture tex;
    if (!render_to_texture(mesh, camera, opts, tex)) return false;

    out.assign(size_t(width) * size_t(height), 0);
    for (int y = 0; y < tex.height; ++y)
        for (int x = 0; x < tex.width; ++x)
            out[size_t(y) * width + x] = tex.at(x, y)[0] > 127 ? 1 : 0;
    return true;
}

// ---------------------------------------------------------------------------
bool capture_framebuffer(int width, int height, Texture& out)
{
    if (!gl::loaded() || width <= 0 || height <= 0) return false;

    out.resize(width, height, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out.pixels.data());

    // OpenGL hands back rows bottom up; PNG wants them the other way round.
    const size_t row = size_t(width) * 4;
    std::vector<uint8_t> scratch(row);
    for (int y = 0; y < height / 2; ++y) {
        uint8_t* a = out.pixels.data() + size_t(y) * row;
        uint8_t* b = out.pixels.data() + size_t(height - 1 - y) * row;
        std::memcpy(scratch.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, scratch.data(), row);
    }

    // The window has no meaningful alpha; force it opaque so the png is not
    // silently transparent in whatever views it later.
    for (size_t i = 3; i < out.pixels.size(); i += 4) out.pixels[i] = 255;
    return true;
}

// ---------------------------------------------------------------------------
ViewSet render_view_set(Renderer& renderer, const Mesh& mesh,
                        const std::vector<ViewCamera>& cameras, const RenderOptions& opts,
                        const std::filesystem::path& directory, const std::string& prefix,
                        const std::vector<Vec4>* per_face_color, const Texture* diffuse,
                        bool also_capture_masks)
{
    ViewSet set;
    set.width  = opts.width;
    set.height = opts.height;

    if (mesh.empty()) { set.error = "no mesh"; return set; }
    if (!renderer.ready()) { set.error = "renderer is not initialised"; return set; }
    if (!paths::ensure_dir(directory)) { set.error = "cannot create " + directory.string(); return set; }

    GpuMesh gpu;
    gpu.upload(mesh);
    if (!gpu.valid()) { set.error = "mesh upload failed"; return set; }
    if (per_face_color) gpu.upload_face_attribute(mesh, *per_face_color);
    if (diffuse && !diffuse->empty()) gpu.set_texture(*diffuse);

    for (const ViewCamera& cam : cameras) {
        ViewSet::Entry entry;
        entry.name        = cam.name;
        entry.description = cam.description;
        entry.primary     = cam.primary;
        entry.weight      = cam.weight;

        Texture image;
        if (!renderer.render_to_texture(gpu, cam, opts, image)) {
            set.error = "render failed for view " + cam.name;
            return set;
        }
        entry.file = directory / (prefix + "_" + slugify(cam.name) + ".png");
        if (!image.save_png(entry.file)) {
            set.error = "cannot write " + entry.file.string();
            return set;
        }

        if (also_capture_masks) {
            renderer.render_mask(gpu, cam, opts.width, opts.height, entry.mask);
            entry.mask_width  = opts.width;
            entry.mask_height = opts.height;
        }
        set.entries.push_back(std::move(entry));
    }

    set.ok = true;
    return set;
}

SilhouetteError compare_silhouettes(const ViewSet& reference, const ViewSet& candidate)
{
    SilhouetteError err;
    if (reference.entries.empty() || candidate.entries.empty()) return err;

    double weighted_sum = 0.0, weight_total = 0.0, outline_sum = 0.0;

    for (const ViewSet::Entry& ref : reference.entries) {
        const ViewSet::Entry* cand = nullptr;
        for (const ViewSet::Entry& c : candidate.entries)
            if (c.name == ref.name) { cand = &c; break; }
        if (!cand || ref.mask.empty() || cand->mask.empty()) continue;
        if (ref.mask.size() != cand->mask.size()) continue;

        size_t disagree = 0, covered = 0;
        for (size_t i = 0; i < ref.mask.size(); ++i) {
            if (ref.mask[i]) ++covered;
            if (ref.mask[i] != cand->mask[i]) ++disagree;
        }
        // Normalising by the reference footprint rather than the whole frame
        // keeps the number meaningful when the model is small in view.
        const float denom = float(std::max<size_t>(covered, 1));
        const float e = float(disagree) / denom;

        size_t perimeter = 0;
        const int w = ref.mask_width, h = ref.mask_height;
        if (w > 1 && h > 1 && size_t(w) * size_t(h) == ref.mask.size()) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const bool here = ref.mask[size_t(y) * w + x] != 0;
                    if (x + 1 < w && here != (ref.mask[size_t(y) * w + x + 1] != 0)) ++perimeter;
                    if (y + 1 < h && here != (ref.mask[size_t(y + 1) * w + x] != 0)) ++perimeter;
                }
        }
        const float px = float(disagree) / float(std::max<size_t>(perimeter, 1));
        outline_sum += double(px) * double(ref.weight);

        err.per_view.emplace_back(ref.name, e);
        weighted_sum += double(e) * double(ref.weight);
        weight_total += double(ref.weight);
        if (e > err.worst) { err.worst = e; err.worst_view = ref.name; }
    }

    err.mean       = weight_total > 0.0 ? float(weighted_sum / weight_total) : 0.0f;
    err.outline_px = weight_total > 0.0 ? float(outline_sum / weight_total) : 0.0f;
    return err;
}

} // namespace rd
