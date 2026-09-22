#pragma once

// Which thing decides where the region boundaries go.
//
// The geometric split in segment.h is the default and the fallback: it needs no
// model, no download and no GPU. Behind this interface sits the other kind of
// answer, the one a person would give - a painted belt is not the torso under
// it, a strap across a chest is one object - which curvature cannot see and a
// vision model can. Both produce the same `Segmentation`, so nothing
// downstream knows or cares which one ran.
//
// The seam is deliberately narrow: a segmenter gets the mesh, its analysis and
// the renders the director is already looking at, and hands back region ids per
// triangle. Everything after that - naming, budget, density - is unchanged.

#include "core/json.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "render/camera.h"
#include "render/renderer.h"
#include "segment/segment.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rd {

enum class SegmenterKind : uint8_t {
    Geometric = 0,   // the deterministic dual graph split
    Sam,             // Segment Anything through the sidecar, geometric on failure
    Auto             // Sam when the sidecar is configured and answers, else Geometric
};

const char*   segmenter_kind_name(SegmenterKind k);
bool          parse_segmenter_kind(const std::string& text, SegmenterKind& out);

// Everything the sidecar needs to know. The checkpoint is the only value with
// no sensible default: it is a 350 MB (ViT-B) to 2.4 GB (ViT-H) file that we
// will not download on anybody's behalf.
struct SamOptions {
    std::string python;        // empty: python3 then python, off PATH
    std::string script;        // empty: tools/sam_server.py next to the exe
    std::string checkpoint;    // .pth for the chosen model type
    std::string model_type = "vit_b";
    std::string device     = "auto";   // auto | cuda | cpu | mps

    // Sample grid the automatic mask generator starts from. 32 is the upstream
    // default and roughly four times the work of 16; 16 is enough here because
    // the masks are only a starting point that the director then merges.
    int   points_per_side = 16;
    // Masks outside this band of the frame are dropped: the tiny ones are
    // speckle, the huge ones are the whole silhouette in one piece.
    float min_mask_area = 0.0015f;
    float max_mask_area = 0.5f;
    // One ray per Nth pixel in both axes when projecting a mask back onto the
    // mesh. 2 quarters the ray count and costs nothing: the masks are blobs,
    // not hairlines, and the Dijkstra fill cleans up the edges anyway.
    int   pixel_stride = 2;
    // A triangle needs this many votes before a mask can claim it, which keeps
    // a single grazing pixel on a silhouette edge from stealing a triangle.
    int   min_votes = 2;
    int   timeout_seconds = 900;
};

struct SegmenterOptions {
    SegmenterKind kind = SegmenterKind::Geometric;
    SamOptions    sam;

    Json to_json() const;
    static SegmenterOptions from_json(const Json& j);
};

// What a segmenter gets to look at. `views` and `cameras` are parallel: entry i
// of the view set was rendered with camera i. Both may be empty, which is what
// a --no-gpu run looks like, and a segmenter that needs pictures must say so
// through available() rather than failing later.
struct SegmenterInput {
    const Mesh*                    mesh     = nullptr;
    const MeshAnalysis*            analysis = nullptr;
    const ViewSet*                 views    = nullptr;
    const std::vector<ViewCamera>* cameras  = nullptr;
};

class ISegmenter {
public:
    virtual ~ISegmenter() = default;

    virtual const char* name() const = 0;

    // Checked before the run so the caller can fall back without having paid
    // for a model load. `why_not` is written only when this returns false.
    virtual bool available(const SegmenterInput& in, std::string& why_not) const = 0;

    // False means the caller should fall back; `error` says why.
    virtual bool run(const SegmenterInput& in, const SegmentationOptions& opts,
                     Segmentation& out, std::string& error,
                     const std::function<void(float, const char*)>& progress = nullptr,
                     const std::atomic<bool>* cancel = nullptr) = 0;
};

// Never null. Kind::Auto and Kind::Sam both return the SAM segmenter; the
// caller is expected to fall back to make_segmenter({Geometric}) when
// available() or run() says no.
std::unique_ptr<ISegmenter> make_segmenter(const SegmenterOptions& opts);

} // namespace rd
