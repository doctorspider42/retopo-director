#pragma once

// Turns the framing section of a target profile into concrete cameras for the
// mesh that is actually loaded. Both the offscreen renderer and the density
// field use these, so a change to the profile moves the pictures the director
// sees and the geometry it protects at the same time.

#include "core/math.h"
#include "knobs/profile.h"
#include "mesh/mesh.h"

#include <string>
#include <vector>

namespace rd {

struct ViewCamera {
    std::string name;
    std::string description;
    Vec3        eye;
    Vec3        target;
    Vec3        up{0.0f, 1.0f, 0.0f};
    float       fov_radians = 40.0f * kDeg2Rad;
    float       znear = 0.01f;
    float       zfar  = 100.0f;
    bool        primary = false;
    // Relative weight in the silhouette score. Primary cameras count double.
    float       weight = 1.0f;

    Vec3 forward() const { return normalize(target - eye); }
    Mat4 view() const { return Mat4::look_at(eye, target, up); }
    Mat4 proj(float aspect) const { return Mat4::perspective(fov_radians, aspect, znear, zfar); }
    Mat4 view_proj(float aspect) const { return proj(aspect) * view(); }
};

// Builds the profile cameras plus `turntable_views` evenly spaced orbit shots.
// The mesh is assumed Y up; `mesh` only supplies its bounding box.
std::vector<ViewCamera> build_camera_rig(const Mesh& mesh, const TargetProfile& profile);

// A single auto framed camera at the given yaw/pitch, fitting the whole mesh.
ViewCamera fit_camera(const Aabb& bounds, float yaw_degrees, float pitch_degrees,
                      float fov_degrees = 40.0f, float fill = 0.9f);

} // namespace rd
