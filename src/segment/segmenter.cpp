#include "segment/segmenter.h"

#include "core/log.h"
#include "segment/sam.h"

#include <algorithm>
#include <cctype>

namespace rd {
namespace {

// The geometric split, wrapped so the pipeline only ever talks to one type.
class GeometricSegmenter final : public ISegmenter {
public:
    const char* name() const override { return "geometric"; }

    bool available(const SegmenterInput& in, std::string& why_not) const override
    {
        if (!in.mesh || !in.analysis || in.mesh->triangle_count() == 0) {
            why_not = "no mesh to segment";
            return false;
        }
        return true;
    }

    bool run(const SegmenterInput& in, const SegmentationOptions& opts, Segmentation& out,
             std::string& error, const std::function<void(float, const char*)>& progress,
             const std::atomic<bool>* cancel) override
    {
        if (!available(in, error)) return false;
        segment_mesh(*in.mesh, *in.analysis, out, opts, progress);
        if (cancel && cancel->load()) {
            error = "cancelled";
            return false;
        }
        if (!out.valid()) {
            error = "the split produced no regions";
            return false;
        }
        return true;
    }
};

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
const char* segmenter_kind_name(SegmenterKind k)
{
    switch (k) {
    case SegmenterKind::Geometric: return "geometric";
    case SegmenterKind::Sam:       return "sam";
    case SegmenterKind::Auto:      return "auto";
    }
    return "geometric";
}

bool parse_segmenter_kind(const std::string& text, SegmenterKind& out)
{
    const std::string t = lower(text);
    if (t == "geometric" || t == "geom") { out = SegmenterKind::Geometric; return true; }
    if (t == "sam")                      { out = SegmenterKind::Sam;       return true; }
    if (t == "auto")                     { out = SegmenterKind::Auto;      return true; }
    return false;
}

Json SegmenterOptions::to_json() const
{
    Json j;
    j["kind"] = segmenter_kind_name(kind);

    Json s;
    s["python"]          = sam.python;
    s["script"]          = sam.script;
    s["checkpoint"]      = sam.checkpoint;
    s["model_type"]      = sam.model_type;
    s["device"]          = sam.device;
    s["points_per_side"] = sam.points_per_side;
    s["min_mask_area"]   = sam.min_mask_area;
    s["max_mask_area"]   = sam.max_mask_area;
    s["pixel_stride"]    = sam.pixel_stride;
    s["min_votes"]       = sam.min_votes;
    s["timeout_seconds"] = sam.timeout_seconds;
    j["sam"] = s;
    return j;
}

SegmenterOptions SegmenterOptions::from_json(const Json& j)
{
    SegmenterOptions o;
    parse_segmenter_kind(json_get<std::string>(j, "kind", "geometric"), o.kind);

    const Json& s = json_object_or_empty(j, "sam");
    o.sam.python          = json_get<std::string>(s, "python", o.sam.python);
    o.sam.script          = json_get<std::string>(s, "script", o.sam.script);
    o.sam.checkpoint      = json_get<std::string>(s, "checkpoint", o.sam.checkpoint);
    o.sam.model_type      = json_get<std::string>(s, "model_type", o.sam.model_type);
    o.sam.device          = json_get<std::string>(s, "device", o.sam.device);
    o.sam.points_per_side = std::clamp(json_get<int>(s, "points_per_side", o.sam.points_per_side), 4, 64);
    o.sam.min_mask_area   = std::clamp(json_get<float>(s, "min_mask_area", o.sam.min_mask_area), 0.0f, 1.0f);
    o.sam.max_mask_area   = std::clamp(json_get<float>(s, "max_mask_area", o.sam.max_mask_area), 0.01f, 1.0f);
    o.sam.pixel_stride    = std::clamp(json_get<int>(s, "pixel_stride", o.sam.pixel_stride), 1, 16);
    o.sam.min_votes       = std::clamp(json_get<int>(s, "min_votes", o.sam.min_votes), 1, 64);
    o.sam.timeout_seconds = std::clamp(json_get<int>(s, "timeout_seconds", o.sam.timeout_seconds), 10, 7200);
    return o;
}

std::unique_ptr<ISegmenter> make_segmenter(const SegmenterOptions& opts)
{
    switch (opts.kind) {
    case SegmenterKind::Sam:
    case SegmenterKind::Auto:
        return make_sam_segmenter(opts.sam);
    case SegmenterKind::Geometric:
        break;
    }
    return std::make_unique<GeometricSegmenter>();
}

} // namespace rd
