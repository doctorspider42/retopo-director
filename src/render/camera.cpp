#include "render/camera.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>

namespace rd {
namespace {

Vec3 orbit_direction(float yaw_degrees, float pitch_degrees)
{
    const float yaw   = yaw_degrees * kDeg2Rad;
    const float pitch = pitch_degrees * kDeg2Rad;
    // Yaw 0 puts the camera on -Z looking at the origin, which lines up with
    // the glTF convention of a model facing +Z.
    const float cp = std::cos(pitch);
    return normalize(Vec3{std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp});
}

} // namespace

ViewCamera fit_camera(const Aabb& bounds, float yaw_degrees, float pitch_degrees,
                      float fov_degrees, float fill)
{
    ViewCamera cam;
    cam.fov_radians = clampf(fov_degrees, 5.0f, 150.0f) * kDeg2Rad;
    cam.target      = bounds.valid() ? bounds.center() : Vec3{};

    const float radius = bounds.valid() ? std::max(0.5f * bounds.diagonal(), kEps) : 1.0f;
    const float safe_fill = clampf(fill, 0.1f, 1.0f);
    const float distance  = radius / std::max(std::sin(cam.fov_radians * 0.5f) * safe_fill, kEps);

    cam.eye   = cam.target + orbit_direction(yaw_degrees, pitch_degrees) * distance;
    cam.znear = std::max(distance - radius * 2.0f, radius * 1e-3f);
    cam.zfar  = distance + radius * 3.0f;
    return cam;
}

std::vector<ViewCamera> build_camera_rig(const Mesh& mesh, const TargetProfile& profile)
{
    std::vector<ViewCamera> out;

    const Aabb bounds = mesh.bounds();
    if (!bounds.valid()) return out;

    const Vec3  extent  = bounds.extent();
    const float height  = std::max(extent.y, kEps);
    const float radius  = std::max(0.5f * bounds.diagonal(), kEps);
    // Everything in the profile is authored in metres; the mesh has been
    // normalised, so convert once here.
    const float units_per_m = profile.units_per_metre(height);
    const Vec3  feet        = {bounds.center().x, bounds.lo.y, bounds.center().z};

    for (const ProfileCamera& pc : profile.cameras) {
        ViewCamera cam;
        cam.name        = pc.name;
        cam.description = pc.description;
        cam.primary     = pc.primary;
        cam.weight      = pc.primary ? 2.0f : 1.0f;
        cam.fov_radians = clampf(pc.fov_degrees, 5.0f, 150.0f) * kDeg2Rad;

        if (pc.distance_m > 0.0f) {
            cam.target = feet + Vec3{0.0f, pc.height_m * units_per_m, 0.0f};
            const float distance = pc.distance_m * units_per_m;
            cam.eye   = cam.target + orbit_direction(pc.yaw_degrees, pc.pitch_degrees) * distance;
            cam.znear = std::max(distance * 0.02f, radius * 1e-3f);
            cam.zfar  = distance + radius * 4.0f;
        } else {
            cam = fit_camera(bounds, pc.yaw_degrees, pc.pitch_degrees, pc.fov_degrees,
                             pc.fit_fraction > 0.0f ? pc.fit_fraction : 0.9f);
            cam.name        = pc.name;
            cam.description = pc.description;
            cam.primary     = pc.primary;
            cam.weight      = pc.primary ? 2.0f : 1.0f;
        }
        out.push_back(std::move(cam));
    }

    const int turns = std::max(0, profile.turntable_views);
    for (int i = 0; i < turns; ++i) {
        const float yaw = 360.0f * float(i) / float(turns);
        ViewCamera cam = fit_camera(bounds, yaw, 12.0f, 40.0f, 0.88f);
        cam.name        = format("turntable_%02d", i);
        cam.description = format("Orbit view at %.0f degrees", yaw);
        cam.weight      = 1.0f;
        out.push_back(std::move(cam));
    }

    if (out.empty()) {
        ViewCamera cam = fit_camera(bounds, 180.0f, 10.0f);
        cam.name    = "default";
        cam.primary = true;
        cam.weight  = 2.0f;
        out.push_back(std::move(cam));
    }

    return out;
}

} // namespace rd
