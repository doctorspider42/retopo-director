#include "segment/sam.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/thread_pool.h"
#include "core/util.h"
#include "llm/process.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace rd {
namespace {

namespace fs = std::filesystem;

constexpr int kProtocolVersion = 1;

// One mask from one view, already projected nowhere: just the pixels it covers.
struct MaskRuns {
    std::vector<uint32_t> runs;     // alternating background / foreground lengths
    float                 score = 0.0f;
    double                area_share = 0.0;
};

struct ViewMasks {
    std::string           name;
    // Which camera rendered it. Views whose PNG never made it to disk are left
    // out of the request, so this cannot be assumed to be the position.
    int                   camera = -1;
    int                   width = 0, height = 0;
    std::vector<MaskRuns> masks;
};

// Basis of the frustum the view was rendered with. Mirrors Mat4::look_at,
// including its fallback for a camera looking straight up the up axis: if the
// two disagree the rays miss the geometry the pixels actually show.
struct CameraBasis {
    Vec3  eye, forward, right, up;
    float tan_half_fov = 0.0f, aspect = 1.0f;

    CameraBasis(const ViewCamera& cam, int width, int height)
    {
        eye     = cam.eye;
        forward = cam.forward();
        Vec3 s  = cross(forward, cam.up);
        if (length2(s) < 1e-12f) s = cross(forward, Vec3{0.0f, 0.0f, 1.0f});
        right = normalize(s);
        up    = cross(right, forward);

        tan_half_fov = std::tan(cam.fov_radians * 0.5f);
        aspect       = float(width) / float(std::max(1, height));
    }

    // Pixel centre, row zero at the top: that is the order the renderer writes
    // its PNGs in (RenderOptions::flip_y), and the PNGs are what SAM saw.
    Vec3 ray(int x, int y, int width, int height) const
    {
        const float ndc_x = 2.0f * (float(x) + 0.5f) / float(width) - 1.0f;
        const float ndc_y = 1.0f - 2.0f * (float(y) + 0.5f) / float(height);
        return forward + right * (ndc_x * tan_half_fov * aspect) +
               up * (ndc_y * tan_half_fov);
    }
};

std::string resolve_python(const std::string& configured)
{
    if (!configured.empty()) return which(configured);
    for (const char* candidate : {"python3", "python", "py"}) {
        const std::string found = which(candidate);
        if (!found.empty()) return found;
    }
    return {};
}

// Splits every label into its connected components over the dual graph. A mask
// that wraps a limb can cover triangles on two different parts of the surface
// that share nothing but a colour, and those must not become one region.
void split_disconnected(const MeshTopology& topo, std::vector<uint16_t>& tri_region,
                        uint32_t& next_label)
{
    const size_t tcount = tri_region.size();
    if (topo.triangle_count() != tcount) return;

    std::vector<uint8_t>           seen(tcount, 0);
    std::vector<uint32_t>          stack, component;
    std::unordered_map<uint16_t, int> components_of;

    for (size_t t = 0; t < tcount; ++t) {
        if (seen[t] || tri_region[t] == kNoRegion) continue;
        const uint16_t label = tri_region[t];

        component.clear();
        stack.assign(1, static_cast<uint32_t>(t));
        seen[t] = 1;
        while (!stack.empty()) {
            const uint32_t cur = stack.back();
            stack.pop_back();
            component.push_back(cur);
            for (int c = 0; c < 3; ++c) {
                const uint32_t n = topo.neighbour(cur, c);
                if (n == kInvalidIndex || n >= tcount) continue;
                if (seen[n] || tri_region[n] != label) continue;
                seen[n] = 1;
                stack.push_back(n);
            }
        }

        // The first component of a label keeps it; every later one is a
        // separate patch that happened to share a mask, and gets a fresh id.
        if (++components_of[label] == 1) continue;
        if (next_label >= kNoRegion) continue;   // out of ids: leave it merged
        const uint16_t fresh = static_cast<uint16_t>(next_label++);
        for (uint32_t tri : component) tri_region[tri] = fresh;
    }
}

} // namespace

// ---------------------------------------------------------------------------
std::string find_sam_script()
{
    std::error_code ec;
    const fs::path candidates[] = {
        paths::exe_dir() / "tools" / "sam_server.py",
        paths::exe_dir() / ".." / "tools" / "sam_server.py",
        // A build directory run: <repo>/build/<preset>/bin/retopo-director.exe
        paths::exe_dir() / ".." / ".." / ".." / ".." / "tools" / "sam_server.py",
    };
    for (const fs::path& p : candidates)
        if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec).string();
    return {};
}

namespace {

class SamSegmenter final : public ISegmenter {
public:
    explicit SamSegmenter(const SamOptions& opts) : opts_(opts) {}

    const char* name() const override { return "sam"; }

    bool available(const SegmenterInput& in, std::string& why_not) const override
    {
        if (!in.mesh || !in.analysis || in.mesh->triangle_count() == 0) {
            why_not = "no mesh to segment";
            return false;
        }
        if (in.analysis->bvh.empty()) {
            why_not = "the analysis has no BVH to project the masks through";
            return false;
        }
        if (!in.views || !in.cameras || in.views->entries.empty()) {
            why_not = "no reference renders to segment (a --no-gpu run has none)";
            return false;
        }
        if (in.cameras->size() < in.views->entries.size()) {
            why_not = "the view set and the camera rig disagree";
            return false;
        }
        if (opts_.checkpoint.empty()) {
            why_not = "no SAM checkpoint configured";
            return false;
        }
        std::error_code ec;
        if (!fs::exists(opts_.checkpoint, ec)) {
            why_not = "the SAM checkpoint '" + opts_.checkpoint + "' is not there";
            return false;
        }
        if (resolve_python(opts_.python).empty()) {
            why_not = "no Python interpreter on PATH";
            return false;
        }
        if (script_path().empty()) {
            why_not = "tools/sam_server.py not found; set the script path";
            return false;
        }
        return true;
    }

    bool run(const SegmenterInput& in, const SegmentationOptions& opts, Segmentation& out,
             std::string& error, const std::function<void(float, const char*)>& progress,
             const std::atomic<bool>* cancel) override
    {
        if (!available(in, error)) return false;

        Stopwatch watch;
        auto report = [&](float f, const char* what) { if (progress) progress(f, what); };

        std::vector<ViewMasks> views;
        report(0.05f, "asking the SAM sidecar");
        if (!call_sidecar(*in.views, views, error, cancel)) return false;
        if (cancel && cancel->load()) { error = "cancelled"; return false; }

        size_t mask_total = 0;
        for (const ViewMasks& v : views) mask_total += v.masks.size();
        if (mask_total == 0) {
            error = "the sidecar returned no usable masks";
            return false;
        }
        RD_INFO("sam: %zu masks over %zu views", mask_total, views.size());

        report(0.45f, "projecting masks onto the mesh");
        std::vector<uint16_t> tri_region;
        uint32_t              labels = 0;
        if (!project(in, views, tri_region, labels, error, progress, cancel))
            return false;
        if (cancel && cancel->load()) { error = "cancelled"; return false; }

        report(0.85f, "filling and merging");
        split_disconnected(in.analysis->topology, tri_region, labels);
        grow_unassigned_regions(*in.mesh, *in.analysis, opts, tri_region);

        out.clear();
        out.tri_region = std::move(tri_region);
        finalize_segmentation(*in.mesh, *in.analysis, opts, out, "part");
        reduce_to_target(*in.mesh, *in.analysis, opts, out);

        if (!out.valid()) {
            error = "mask projection left no regions";
            return false;
        }
        for (Region& r : out.regions)
            if (r.auto_label.empty()) r.auto_label = "sam patch";

        out.seconds = watch.seconds();
        report(1.0f, "done");
        RD_INFO("sam segmentation: %zu regions over %zu triangles in %s",
                out.regions.size(), out.tri_region.size(),
                format_duration(out.seconds).c_str());
        return true;
    }

private:
    std::string script_path() const
    {
        if (!opts_.script.empty()) {
            std::error_code ec;
            if (fs::exists(opts_.script, ec)) return opts_.script;
            return {};
        }
        return find_sam_script();
    }

    // --- the sidecar --------------------------------------------------------
    bool call_sidecar(const ViewSet& set, std::vector<ViewMasks>& out, std::string& error,
                      const std::atomic<bool>* cancel) const
    {
        Json req;
        req["protocol"]        = kProtocolVersion;
        req["checkpoint"]      = opts_.checkpoint;
        req["model_type"]      = opts_.model_type;
        req["device"]          = opts_.device;
        req["points_per_side"] = opts_.points_per_side;
        req["min_mask_area"]   = opts_.min_mask_area;
        req["max_mask_area"]   = opts_.max_mask_area;

        Json views = Json::array();
        std::error_code ec;
        for (size_t i = 0; i < set.entries.size(); ++i) {
            const ViewSet::Entry& e = set.entries[i];
            if (e.file.empty() || !fs::exists(e.file, ec)) continue;
            Json v;
            v["name"]  = e.name;
            v["index"] = int(i);           // echoed back, so the rays use the right camera
            v["file"]  = fs::absolute(e.file, ec).string();
            views.push_back(std::move(v));
        }
        if (views.empty()) {
            error = "none of the reference renders made it to disk";
            return false;
        }
        req["views"] = std::move(views);

        ProcessRequest proc;
        proc.executable      = resolve_python(opts_.python);
        proc.arguments       = {script_path()};
        proc.stdin_data      = json_dump(req, 0);
        proc.timeout_seconds = opts_.timeout_seconds;
        proc.cancel          = cancel;

        RD_INFO("sam: %s", format_command(proc.executable, proc.arguments).c_str());
        const ProcessResult res = run_process(proc);

        if (!res.started)  { error = "cannot start the sidecar: " + res.error; return false; }
        if (res.cancelled) { error = "cancelled"; return false; }
        if (res.timed_out) {
            error = format("the sidecar timed out after %d s", opts_.timeout_seconds);
            return false;
        }
        // Torch is chatty on stderr even when it works, so it is only
        // interesting once something has gone wrong.
        if (res.exit_code != 0) {
            error = format("the sidecar exited with %d: %s", res.exit_code,
                           trim(res.err).substr(0, 400).c_str());
            return false;
        }

        std::string parse_error;
        const Json reply = json_parse_lenient(res.out, parse_error);
        if (reply.is_null()) {
            error = "cannot read the sidecar reply: " + parse_error;
            return false;
        }
        if (!json_get<bool>(reply, "ok", false)) {
            error = json_get<std::string>(reply, "error", "the sidecar reported a failure");
            return false;
        }

        const std::string device = json_get<std::string>(reply, "device", "?");
        RD_INFO("sam: model on %s, %.1f s", device.c_str(),
                json_get<double>(reply, "seconds", 0.0));
        if (device == "cpu")
            RD_WARN("sam: running on the CPU, which is roughly fifteen seconds a view");

        for (const Json& jv : json_array_or_empty(reply, "views")) {
            if (!jv.is_object()) continue;
            ViewMasks vm;
            vm.name   = json_get<std::string>(jv, "name", "");
            vm.camera = json_get<int>(jv, "index", -1);
            vm.width  = json_get<int>(jv, "width", 0);
            vm.height = json_get<int>(jv, "height", 0);
            if (vm.width <= 0 || vm.height <= 0) continue;
            if (vm.camera < 0) {
                // An older sidecar that does not echo the index: fall back to
                // the name, which the renderer takes from the camera.
                for (size_t i = 0; i < set.entries.size(); ++i)
                    if (set.entries[i].name == vm.name) { vm.camera = int(i); break; }
            }
            if (vm.camera < 0 || vm.camera >= int(set.entries.size())) {
                RD_WARN("sam: reply mentions view '%s', which was never sent", vm.name.c_str());
                continue;
            }

            for (const Json& jm : json_array_or_empty(jv, "masks")) {
                if (!jm.is_object()) continue;
                MaskRuns m;
                m.score      = json_get<float>(jm, "score", 0.0f);
                m.area_share = json_get<double>(jm, "area_share", 0.0);
                const Json& runs = json_array_or_empty(jm, "rle");
                m.runs.reserve(runs.size());
                for (const Json& r : runs) {
                    if (!r.is_number_unsigned() && !r.is_number_integer()) { m.runs.clear(); break; }
                    const long long v = r.get<long long>();
                    if (v < 0) { m.runs.clear(); break; }
                    m.runs.push_back(static_cast<uint32_t>(v));
                }
                if (!m.runs.empty()) vm.masks.push_back(std::move(m));
            }
            out.push_back(std::move(vm));
        }
        return true;
    }

    // --- masks back onto the mesh -------------------------------------------
    bool project(const SegmenterInput& in, const std::vector<ViewMasks>& views,
                 std::vector<uint16_t>& tri_region,
                 uint32_t& label_count, std::string& error,
                 const std::function<void(float, const char*)>& progress,
                 const std::atomic<bool>* cancel) const
    {
        const Mesh&   mesh   = *in.mesh;
        const Bvh&    bvh    = in.analysis->bvh;
        const size_t  tcount = mesh.triangle_count();
        const int     stride = std::max(1, opts_.pixel_stride);

        std::vector<uint32_t> best_label(tcount, kNoRegion);
        std::vector<uint32_t> best_votes(tcount, 0);
        uint32_t next_label = 0;

        // Every label has to survive a round trip through uint16_t, and
        // finalize_segmentation reserves 0xFFFF for "no region".
        const uint32_t max_labels = kNoRegion - 1;

        ThreadPool& pool = ThreadPool::shared();

        for (size_t vi = 0; vi < views.size(); ++vi) {
            if (cancel && cancel->load()) { error = "cancelled"; return false; }
            const ViewMasks& vm = views[vi];
            if (vm.camera < 0 || size_t(vm.camera) >= in.cameras->size()) continue;

            const int w = vm.width, h = vm.height;
            const int sw = (w + stride - 1) / stride;
            const int sh = (h + stride - 1) / stride;
            if (sw <= 0 || sh <= 0) continue;

            // One ray per sampled pixel, shared by every mask in this view:
            // SAM masks overlap heavily, so casting per mask would pay for the
            // same pixel several times over.
            const CameraBasis basis((*in.cameras)[size_t(vm.camera)], w, h);
            std::vector<uint32_t> hits(size_t(sw) * size_t(sh), kInvalidIndex);
            pool.parallel_ranges(size_t(sh), 8, [&](size_t begin, size_t end, unsigned) {
                for (size_t sy = begin; sy < end; ++sy) {
                    for (int sx = 0; sx < sw; ++sx) {
                        const Vec3 dir = basis.ray(sx * stride, int(sy) * stride, w, h);
                        const RayHit hit = bvh.intersect(basis.eye, dir);
                        if (hit.hit()) hits[sy * size_t(sw) + size_t(sx)] = hit.triangle;
                    }
                }
            });

            // One label per mask, votes tallied per triangle.
            std::unordered_map<uint32_t, uint32_t> votes;
            for (const MaskRuns& m : vm.masks) {
                if (next_label >= max_labels) {
                    RD_WARN("sam: ran out of region ids after %u masks", next_label);
                    break;
                }
                votes.clear();

                size_t pixel = 0;
                bool   foreground = false;    // runs start on background
                for (uint32_t run : m.runs) {
                    if (foreground) {
                        for (uint32_t k = 0; k < run; ++k) {
                            const size_t p = pixel + k;
                            const int x = int(p % size_t(w));
                            const int y = int(p / size_t(w));
                            if (y >= h) break;
                            if ((x % stride) || (y % stride)) continue;
                            const uint32_t tri = hits[size_t(y / stride) * size_t(sw) + size_t(x / stride)];
                            if (tri != kInvalidIndex) ++votes[tri];
                        }
                    }
                    pixel += run;
                    foreground = !foreground;
                }
                if (votes.empty()) continue;

                const uint32_t label = next_label++;
                for (const auto& [tri, count] : votes) {
                    if (tri >= tcount) continue;
                    if (int(count) < std::max(1, opts_.min_votes)) continue;
                    if (count > best_votes[tri]) {
                        best_votes[tri] = count;
                        best_label[tri] = label;
                    }
                }
            }

            if (progress) {
                progress(0.45f + 0.40f * float(vi + 1) / float(views.size()),
                         "projecting masks onto the mesh");
            }
        }

        if (next_label == 0) {
            error = "no mask covered a single triangle";
            return false;
        }

        tri_region.assign(tcount, kNoRegion);
        size_t claimed = 0;
        for (size_t t = 0; t < tcount; ++t) {
            if (best_label[t] == kNoRegion) continue;
            tri_region[t] = static_cast<uint16_t>(best_label[t]);
            ++claimed;
        }
        RD_INFO("sam: %zu of %zu triangles claimed directly (%.0f%%), %u labels",
                claimed, tcount, tcount ? 100.0 * double(claimed) / double(tcount) : 0.0,
                next_label);

        label_count = next_label;
        return claimed > 0;
    }

    // SAM is generous: thirty masks a view is normal, and the naming step stops
    // being useful past about sixty regions. Merge the smallest into the
    // neighbour they agree with best until the count is back inside the budget.
    static void reduce_to_target(const Mesh& mesh, const MeshAnalysis& analysis,
                                 const SegmentationOptions& opts, Segmentation& seg)
    {
        const size_t target = size_t(std::clamp(opts.target_regions, 2, 512));
        int guard = 0;
        while (seg.regions.size() > target && guard++ < 4096) {
            size_t smallest = 0;
            for (size_t i = 1; i < seg.regions.size(); ++i)
                if (seg.regions[i].area_share < seg.regions[smallest].area_share) smallest = i;

            const Region& r = seg.regions[smallest];
            if (r.neighbours.empty()) break;

            uint16_t best       = r.neighbours.front();
            float    best_score = -2.0f;
            for (uint16_t n : r.neighbours) {
                const Region* nr = seg.find(n);
                if (!nr) continue;
                const float score = nr->area_share -
                                    std::fabs(nr->mean_curvature - r.mean_curvature);
                if (score > best_score) { best_score = score; best = n; }
            }

            const uint16_t victim = r.id;
            for (uint16_t& v : seg.tri_region)
                if (v == victim) v = best;
            seg.regions.erase(seg.regions.begin() + static_cast<long>(smallest));
            seg.refresh_statistics(mesh, analysis);
        }

        // Back to a dense 0..n-1 range. refresh_statistics keys off the ids, so
        // the triangles have to be renumbered in the same pass as the regions.
        std::unordered_map<uint16_t, uint16_t> dense;
        for (size_t i = 0; i < seg.regions.size(); ++i)
            dense[seg.regions[i].id] = static_cast<uint16_t>(i);
        for (uint16_t& v : seg.tri_region) {
            const auto it = dense.find(v);
            v = it == dense.end() ? kNoRegion : it->second;
        }
        for (size_t i = 0; i < seg.regions.size(); ++i) {
            seg.regions[i].id   = static_cast<uint16_t>(i);
            seg.regions[i].name = format("part_%02zu", i);
        }
        seg.refresh_statistics(mesh, analysis);
    }

    SamOptions opts_;
};

} // namespace

std::unique_ptr<ISegmenter> make_sam_segmenter(const SamOptions& opts)
{
    return std::make_unique<SamSegmenter>(opts);
}

} // namespace rd
