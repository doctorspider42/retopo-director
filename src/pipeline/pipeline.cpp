#include "pipeline/pipeline.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"

#include <algorithm>
#include <limits>

namespace rd {
namespace fs = std::filesystem;

// How many times the engine may re-fit its own budget before it gives up and
// keeps the best attempt it has.
constexpr int kBudgetAttempts = 6;
// A run that lands below this share of its binding limit tries again with a
// bigger budget. The backends undershoot on their own - the quad field rarely
// lands within 20% of its target - and the unwrap's seam vertices eat into
// the vertex limit unpredictably, so without a second look a quarter of the
// budget the profile allows is routinely left unspent.
constexpr float kBudgetFillTarget = 0.93f;

const char* stage_name(Stage s)
{
    switch (s) {
    case Stage::Loading:          return "Loading";
    case Stage::Analysing:        return "Analysing";
    case Stage::ReferenceRenders: return "Reference renders";
    case Stage::Segmenting:       return "Segmenting";
    case Stage::NamingRegions:    return "Naming regions";
    case Stage::AllocatingBudget: return "Allocating budget";
    case Stage::BuildingDensity:  return "Density field";
    case Stage::Retopologising:   return "Retopology";
    case Stage::Baking:           return "Baking";
    case Stage::Validating:       return "Validating";
    case Stage::CandidateRenders: return "Candidate renders";
    case Stage::Reviewing:        return "Director review";
    case Stage::Exporting:        return "Exporting";
    case Stage::Reporting:        return "Writing report";
    case Stage::Done:             return "Done";
    case Stage::Failed:           return "Failed";
    case Stage::Cancelled:        return "Cancelled";
    default:                      return "Idle";
    }
}

bool stage_is_terminal(Stage s)
{
    return s == Stage::Done || s == Stage::Failed || s == Stage::Cancelled || s == Stage::Idle;
}

// Where each stage starts and ends in a whole run. The numbers are wall clock
// shares measured on a 300k triangle character with the quad field backend and
// the geometric split; they are rough on purpose, because the point is a bar
// that keeps moving, not a time estimate.
namespace {

struct StageSpan { float begin, end; };

StageSpan span_of(Stage s)
{
    switch (s) {
    case Stage::Loading:          return {0.00f, 0.04f};
    case Stage::Analysing:        return {0.04f, 0.11f};
    case Stage::ReferenceRenders: return {0.11f, 0.18f};
    case Stage::Segmenting:       return {0.18f, 0.28f};
    case Stage::NamingRegions:    return {0.28f, 0.33f};
    case Stage::AllocatingBudget: return {0.33f, 0.36f};
    // The loop, from here to the end of the review.
    case Stage::BuildingDensity:  return {0.36f, 0.44f};
    case Stage::Retopologising:   return {0.44f, 0.62f};
    case Stage::Baking:           return {0.62f, 0.76f};
    case Stage::Validating:       return {0.76f, 0.80f};
    case Stage::CandidateRenders: return {0.80f, 0.86f};
    case Stage::Reviewing:        return {0.86f, 0.92f};
    case Stage::Exporting:        return {0.92f, 0.97f};
    case Stage::Reporting:        return {0.97f, 1.00f};
    case Stage::Done:             return {1.00f, 1.00f};
    default:                      return {0.00f, 0.00f};
    }
}

constexpr float kLoopBegin = 0.36f;
constexpr float kLoopEnd   = 0.92f;

bool stage_in_loop(Stage s)
{
    return s >= Stage::BuildingDensity && s <= Stage::Reviewing;
}

} // namespace

float stage_overall_progress(Stage s, float within, int iteration, int total_iterations)
{
    if (s == Stage::Failed || s == Stage::Cancelled) return 0.0f;
    if (s == Stage::Done) return 1.0f;

    const StageSpan span = span_of(s);
    float f = span.begin + (span.end - span.begin) * clampf(within, 0.0f, 1.0f);

    // Squeeze the loop stages into the slice of the loop window that belongs to
    // this iteration. Without it the bar snaps back to 36% every time the
    // director asks for another pass, which reads as the run starting over.
    if (stage_in_loop(s) && total_iterations > 1 && iteration > 0) {
        const float slice = (kLoopEnd - kLoopBegin) / float(total_iterations);
        const float local = (f - kLoopBegin) / (kLoopEnd - kLoopBegin);
        f = kLoopBegin + slice * (float(std::min(iteration, total_iterations) - 1) + local);
    }
    return clampf(f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
Pipeline::Pipeline() = default;

Pipeline::~Pipeline()
{
    cancel();
    join();
}

void Pipeline::join()
{
    if (worker_.joinable()) worker_.join();
}

void Pipeline::cancel()
{
    if (running_.load()) {
        cancel_.store(true);
        RD_INFO("cancellation requested");
    }
}

void Pipeline::set_stage(Stage s, const std::string& msg)
{
    stage_.store(s);
    progress_.store(0.0f);
    std::lock_guard lock(status_mutex_);
    message_ = msg;
    RD_INFO("stage: %s%s%s", stage_name(s), msg.empty() ? "" : " - ", msg.c_str());
}

void Pipeline::set_progress(float f, const std::string& msg)
{
    progress_.store(clampf(f, 0.0f, 1.0f));
    if (msg.empty()) return;
    std::lock_guard lock(status_mutex_);
    message_ = msg;
}

void Pipeline::fail(const std::string& msg)
{
    stage_.store(Stage::Failed);
    std::lock_guard lock(status_mutex_);
    error_   = msg;
    message_ = msg;
    RD_ERROR("pipeline failed: %s", msg.c_str());
}

std::string Pipeline::message() const
{
    std::lock_guard lock(status_mutex_);
    return message_;
}

std::string Pipeline::error() const
{
    std::lock_guard lock(status_mutex_);
    return error_;
}

void Pipeline::bump() { version_.fetch_add(1, std::memory_order_release); }

void Pipeline::set_panel(const KnobPanel& panel)
{
    if (running_.load()) return;
    {
        std::lock_guard lock(results_mutex_);
        results_.panel = panel;
    }
    bump();
}

bool Pipeline::start(const fs::path& mesh_path, const PipelineSettings& settings)
{
    if (running_.load()) {
        // A preview is a courtesy and a run is not, so the run takes the thread.
        // It only loads a file, so this waits for a fraction of a second. Without
        // it, pressing Run on a mesh picked a moment ago quietly does nothing.
        if (!preview_.load()) return false;
        cancel();
    }
    join();
    launch(Entry::Full, mesh_path, settings);
    return true;
}

bool Pipeline::rebuild_geometry(const PipelineSettings& settings)
{
    if (running_.load()) return false;
    {
        std::lock_guard lock(results_mutex_);
        if (results_.highpoly.empty() || !results_.segmentation.valid()) return false;
    }
    join();
    launch(Entry::GeometryOnly, {}, settings);
    return true;
}

bool Pipeline::preview(const fs::path& mesh_path, const PipelineSettings& settings)
{
    if (running_.load()) return false;
    join();
    launch(Entry::PreviewOnly, mesh_path, settings);
    return true;
}

void Pipeline::launch(Entry entry, const fs::path& mesh_path, const PipelineSettings& settings)
{
    cancel_.store(false);
    running_.store(true);
    {
        std::lock_guard lock(status_mutex_);
        error_.clear();
    }
    worker_ = std::thread(&Pipeline::run, this, entry, mesh_path, settings);
}

// ---------------------------------------------------------------------------
ViewSet Pipeline::render_views(const Mesh& mesh, const RenderOptions& opts,
                               const fs::path& dir, const std::string& prefix,
                               const std::vector<Vec4>* face_colors, const Texture* diffuse,
                               bool masks)
{
    ViewSet set;
    if (!renderer_ || !dispatcher_) {
        set.error = "no renderer available (headless build or GPU init failed)";
        return set;
    }

    std::vector<ViewCamera> cameras;
    {
        std::lock_guard lock(results_mutex_);
        cameras = results_.cameras;
    }

    const bool ran = dispatcher_->run_and_wait([&] {
        set = render_view_set(*renderer_, mesh, cameras, opts, dir, prefix, face_colors,
                              diffuse, masks);
    });
    if (!ran && set.error.empty()) set.error = "the render was never dispatched";
    return set;
}

// Records what the director said about the brief. Nothing is applied: a profile
// is a promise about what the hardware can run, and a model that has seen one
// mesh is not the thing that gets to renegotiate it. The loudest this ever gets
// is a warning in the log and a card in the interface.
void Pipeline::note_advice(const ProfileAdvice& advice, IterationRecord* record)
{
    if (advice.empty()) return;

    if (feasibility_is_alarming(advice.feasibility))
        RD_WARN("the director calls this brief %s: %s",
                feasibility_name(advice.feasibility), advice.headline.c_str());
    else
        RD_INFO("the director calls this brief %s%s%s", feasibility_name(advice.feasibility),
                advice.headline.empty() ? "" : ": ", advice.headline.c_str());

    for (const ProfileProposal& prop : advice.proposals) {
        if (prop.applicable)
            RD_INFO("  proposes %s %s - %s", prop.field.c_str(),
                    prop.change_text().c_str(), prop.reason.c_str());
        else
            RD_DEBUG("  proposal on %s ignored (%s)", prop.field.c_str(), prop.note.c_str());
    }

    if (record) record->advice = advice;
    std::lock_guard lock(results_mutex_);
    merge_advice(results_.advice, advice);
}

LlmResponse Pipeline::ask(const LlmRequest& req, const char* stage_label)
{
    LlmResponse res;
    if (!llm_) {
        res.error = "no LLM backend";
        return res;
    }

    // Persist the exact prompt next to the run so a human can audit it.
    const fs::path dir = paths::reports_dir() / "prompts";
    paths::ensure_dir(dir);
    const std::string base = format("%02d_%s", iteration_.load(), slugify(req.label).c_str());
    paths::write_file(dir / (base + "_prompt.txt"), req.system + "\n\n" + req.user);

    res = llm_->complete(req, &cancel_);

    if (!res.raw.empty()) paths::write_file(dir / (base + "_reply.txt"), res.raw);
    // The reply as the director layer saw it, without the backend's envelope.
    // This is what the Replay backend reads back, so a run can be repeated
    // exactly without the model: --replay <this run>/reports/prompts.
    if (res.ok) paths::write_file(dir / (base + "_text.txt"), res.text);

    LlmExchange exchange;
    exchange.stage  = stage_label;
    exchange.prompt = req.user;
    exchange.reply  = res.text;
    exchange.error  = res.error;
    exchange.ok     = res.ok;
    exchange.seconds = res.seconds;
    exchange.prompt_tokens     = res.prompt_tokens;
    exchange.completion_tokens = res.completion_tokens;
    for (const LlmImage& img : req.images) exchange.image_labels.push_back(img.label);

    {
        std::lock_guard lock(results_mutex_);
        results_.transcript.push_back(std::move(exchange));
    }
    bump();

    if (!res.ok) RD_WARN("%s: %s", stage_label, res.error.c_str());
    return res;
}

// ---------------------------------------------------------------------------
bool Pipeline::stage_load(const fs::path& path, const PipelineSettings& s)
{
    set_stage(Stage::Loading, path.filename().string());

    Mesh mesh;
    const meshio::LoadReport rep = meshio::load(path, mesh, s.loading);
    if (!rep.ok) {
        fail("cannot load " + path.filename().string() + ": " + rep.error);
        return false;
    }

    {
        std::lock_guard lock(results_mutex_);
        results_ = PipelineResults{};
        results_.highpoly    = std::move(mesh);
        results_.load_report = rep;
        results_.source_path = path;
    }
    bump();
    return true;
}

bool Pipeline::stage_analyse(const PipelineSettings& s)
{
    set_stage(Stage::Analysing, "curvature, symmetry, armature");

    Mesh copy;
    {
        std::lock_guard lock(results_mutex_);
        copy = results_.highpoly;
    }

    MeshAnalysis analysis;
    analyse_mesh(copy, analysis, s.analysis,
                 [&](float f, const char* what) { set_progress(f, what); });
    if (cancelled()) return false;
    if (!analysis.valid()) {
        fail("mesh analysis produced nothing usable");
        return false;
    }

    std::vector<ViewCamera> cameras = build_camera_rig(copy, s.profile);
    {
        std::lock_guard lock(results_mutex_);
        results_.analysis = std::move(analysis);
        results_.cameras  = std::move(cameras);
        // The BVH points at the mesh it was built from, so keep that exact copy.
        results_.highpoly = std::move(copy);
    }
    bump();
    return true;
}

bool Pipeline::stage_reference_renders(const PipelineSettings& s)
{
    set_stage(Stage::ReferenceRenders, "rendering the high poly");

    RenderOptions opts;
    opts.width   = s.render_size;
    opts.height  = s.render_size;
    opts.samples = s.render_samples;
    opts.mode    = RenderMode::Shaded;

    Mesh copy;
    {
        std::lock_guard lock(results_mutex_);
        copy = results_.highpoly;
    }

    ViewSet views = render_views(copy, opts, paths::renders_dir(), "highpoly", nullptr,
                                 nullptr, true);
    if (!views.ok) {
        // Renders are how the director sees; without them we can still run the
        // deterministic half of the pipeline, so warn rather than abort.
        RD_WARN("reference renders unavailable: %s", views.error.c_str());
    }

    {
        std::lock_guard lock(results_mutex_);
        results_.reference_views = std::move(views);
    }
    bump();
    return true;
}

bool Pipeline::stage_segment(const PipelineSettings& s)
{
    set_stage(Stage::Segmenting, "splitting into regions");

    Mesh                    copy;
    MeshAnalysis            analysis_copy;
    ViewSet                 views_copy;
    std::vector<ViewCamera> cameras_copy;
    {
        std::lock_guard lock(results_mutex_);
        copy          = results_.highpoly;
        analysis_copy = results_.analysis;
        views_copy    = results_.reference_views;
        cameras_copy  = results_.cameras;
    }
    // The analysis owns a BVH that points at the mesh it was built from; rebuild
    // it against our local copy so the pointers stay valid.
    analysis_copy.bvh.build(copy);

    SegmenterInput input;
    input.mesh     = &copy;
    input.analysis = &analysis_copy;
    input.views    = &views_copy;
    input.cameras  = &cameras_copy;

    auto progress = [&](float f, const char* what) { set_progress(f, what); };

    Segmentation seg;
    bool         done = false;

    // The chosen segmenter first, the geometric split as the floor. A missing
    // checkpoint, a machine with no CUDA device or a sidecar that falls over
    // must cost a warning, not the run.
    if (s.segmenter.kind != SegmenterKind::Geometric) {
        // Auto was told to use whatever is there, so a missing checkpoint is
        // news rather than a problem. Sam was asked for by name.
        const bool asked_for = s.segmenter.kind == SegmenterKind::Sam;

        std::unique_ptr<ISegmenter> chosen = make_segmenter(s.segmenter);
        std::string                 why;
        if (!chosen->available(input, why)) {
            if (asked_for) RD_WARN("%s segmentation unavailable: %s", chosen->name(), why.c_str());
            else           RD_INFO("%s segmentation unavailable: %s", chosen->name(), why.c_str());
        } else {
            set_progress(0.0f, "segmenting with SAM");
            done = chosen->run(input, s.segmentation, seg, why, progress, &cancel_);
            if (cancelled()) return false;
            // A sidecar that answered and then failed is worth a warning either
            // way: something was configured, and it did not work.
            if (!done) RD_WARN("%s segmentation failed: %s", chosen->name(), why.c_str());
        }
        if (!done && asked_for) RD_WARN("falling back to the geometric split");
    }

    if (!done) {
        segment_mesh(copy, analysis_copy, seg, s.segmentation, progress);
        if (cancelled()) return false;
    }
    if (!seg.valid()) {
        fail("segmentation produced no regions");
        return false;
    }

    // Visibility per region, straight off the reference masks when we have them.
    {
        std::lock_guard lock(results_mutex_);
        results_.segmentation = std::move(seg);
        results_.highpoly     = std::move(copy);
        results_.analysis     = std::move(analysis_copy);
    }
    bump();

    // Region overlay renders, for the naming step.
    RenderOptions opts;
    opts.width   = s.render_size;
    opts.height  = s.render_size;
    opts.samples = 0;                     // flat colours, no blending across ids
    opts.mode    = RenderMode::Regions;

    Mesh                mesh_copy;
    std::vector<Vec4>   face_colors;
    {
        std::lock_guard lock(results_mutex_);
        mesh_copy = results_.highpoly;
        region_colors(results_.segmentation, face_colors);
    }
    ViewSet region_views = render_views(mesh_copy, opts, paths::renders_dir(), "regions",
                                        &face_colors, nullptr, false);
    if (!region_views.ok) RD_WARN("region renders unavailable: %s", region_views.error.c_str());

    {
        std::lock_guard lock(results_mutex_);
        results_.region_views = std::move(region_views);
    }
    bump();
    return true;
}

bool Pipeline::stage_name_regions(const PipelineSettings& s)
{
    if (!s.use_llm || !llm_) return true;
    set_stage(Stage::NamingRegions, "asking the director what these parts are");

    Mesh         mesh_copy;
    Segmentation seg_copy;
    ViewSet      regions, shaded;
    {
        std::lock_guard lock(results_mutex_);
        mesh_copy = results_.highpoly;
        seg_copy  = results_.segmentation;
        regions   = results_.region_views;
        shaded    = results_.reference_views;
    }

    const LlmRequest req = build_naming_request(mesh_copy, seg_copy, s.profile, regions, shaded);
    const LlmResponse res = ask(req, "name regions");
    if (cancelled()) return false;

    const RegionNaming naming = parse_naming_response(res, seg_copy);
    if (!naming.ok) {
        RD_WARN("region naming skipped: %s", naming.error.c_str());
        return true;   // automatic names are ugly but perfectly workable
    }

    MeshAnalysis analysis_copy;
    {
        std::lock_guard lock(results_mutex_);
        analysis_copy = results_.analysis;
    }
    analysis_copy.bvh.build(mesh_copy);

    for (const RegionNaming::Named& n : naming.named) {
        if (Region* r = seg_copy.find(n.id)) {
            r->name = n.name;
            if (!n.role.empty()) r->auto_label = n.role;
        }
    }
    if (!naming.merges.empty())
        merge_regions(seg_copy, mesh_copy, analysis_copy, naming.merges);

    {
        std::lock_guard lock(results_mutex_);
        results_.segmentation = std::move(seg_copy);
    }
    bump();

    // The overlay is now stale: ids changed when regions merged.
    RenderOptions opts;
    opts.width   = s.render_size;
    opts.height  = s.render_size;
    opts.samples = 0;
    opts.mode    = RenderMode::Regions;

    std::vector<Vec4> face_colors;
    {
        std::lock_guard lock(results_mutex_);
        region_colors(results_.segmentation, face_colors);
    }
    ViewSet region_views = render_views(mesh_copy, opts, paths::renders_dir(), "regions_named",
                                        &face_colors, nullptr, false);
    if (region_views.ok) {
        std::lock_guard lock(results_mutex_);
        results_.region_views = std::move(region_views);
        bump();
    }
    return true;
}

bool Pipeline::stage_allocate_budget(const PipelineSettings& s)
{
    set_stage(Stage::AllocatingBudget, "seeding the knob panel");

    Segmentation seg_copy;
    {
        std::lock_guard lock(results_mutex_);
        seg_copy = results_.segmentation;
    }

    KnobPanel panel = KnobPanel::seed_from_regions(seg_copy.ids(), seg_copy.names(),
                                                   seg_copy.area_shares());
    panel.resolve_budgets(s.profile.max_triangles);

    if (s.use_llm && llm_) {
        Mesh         mesh_copy;
        MeshAnalysis analysis_copy;
        ViewSet      shaded, regions;
        {
            std::lock_guard lock(results_mutex_);
            mesh_copy     = results_.highpoly;
            analysis_copy = results_.analysis;
            shaded        = results_.reference_views;
            regions       = results_.region_views;
        }

        const LlmRequest req = build_budget_request(mesh_copy, analysis_copy, seg_copy,
                                                    s.profile, panel, shaded, regions);
        const LlmResponse res = ask(req, "allocate budget");
        if (cancelled()) return false;

        if (res.ok && res.json_ok) {
            const KnobPanel::ApplyReport applied = panel.apply_patch(res.json);
            RD_INFO("director set %d fields across %d regions%s",
                    applied.fields_changed, applied.regions_touched,
                    applied.unknown_regions
                        ? format(" (%d unknown regions ignored)", applied.unknown_regions).c_str()
                        : "");
            for (const std::string& k : applied.ignored_keys)
                RD_DEBUG("ignored unknown key %s", k.c_str());
            note_advice(parse_advice_response(res, s.profile), nullptr);
        } else {
            RD_WARN("budget allocation fell back to area proportional defaults: %s",
                    res.error.c_str());
        }
    }

    panel.resolve_budgets(s.profile.max_triangles);
    {
        std::lock_guard lock(results_mutex_);
        results_.panel = std::move(panel);
    }
    bump();
    return true;
}

bool Pipeline::stage_iterate(const PipelineSettings& s)
{
    const int max_iterations = std::max(
        1, s.max_iterations > 0 ? s.max_iterations : s.profile.max_iterations);

    // The best iteration so far, which is what the run hands back. The director
    // steers by taste and its next panel is a guess: a later pass is often worse
    // than an earlier one (a head it re-budgeted into a bucket, arms it bloated),
    // and exporting the last pass regardless threw away the good one.
    struct Kept {
        int              iteration = 0;
        Mesh             lowpoly;
        DensityField     density;
        RetopoResult     retopo;
        BakeResult       bake;
        ValidationReport validation;
        ViewSet          candidate;
        SilhouetteError  silhouette;
        KnobPanel        panel;
    };
    Kept kept;
    // Passing validation first, then the silhouette, then the latest: without
    // renders every silhouette is zero and the director's latest word stands.
    auto better = [](const ValidationReport& v, const SilhouetteError& sil, const Kept& than) {
        if (than.iteration == 0) return true;
        if (v.passed != than.validation.passed) return v.passed;
        return sil.mean <= than.silhouette.mean;
    };

    for (int iteration = 1; iteration <= max_iterations; ++iteration) {
        if (cancelled()) return false;
        iteration_.store(iteration);
        Stopwatch iteration_watch;

        // --- snapshot the inputs -------------------------------------------
        Mesh         source;
        MeshAnalysis analysis;
        Segmentation seg;
        KnobPanel    panel;
        {
            std::lock_guard lock(results_mutex_);
            source   = results_.highpoly;
            analysis = results_.analysis;
            seg      = results_.segmentation;
            panel    = results_.panel;
        }
        analysis.bvh.build(source);

        DensityField     density;
        RetopoResult     retopo;
        BakeResult       bake;
        StripData        strips;
        ValidationReport validation;

        // Deterministic budget fitting. The unwrap duplicates vertices along
        // every uv seam, so a triangle count that fits can still blow the vertex
        // limit, and collapses the link condition refuses leave a few triangles
        // over. Neither is a question of taste and neither is worth a round trip
        // to the model, so the engine shrinks its own budget and tries again
        // before anyone is asked for an opinion.
        int effective_budget = s.profile.max_triangles;

        DensityField     best_density;
        RetopoResult     best_retopo;
        BakeResult       best_bake;
        StripData        best_strips;
        ValidationReport best_validation;
        KnobPanel        best_panel;
        int              best_score = std::numeric_limits<int>::max();
        bool             have_best  = false;
        // Largest budget known to land legal, smallest known to land over.
        int              legal_budget   = -1;
        int              illegal_budget = std::numeric_limits<int>::max();
        // Triangles the previous over-budget attempt came back with, and
        // whether the shell cap has been engaged because shrinking stopped
        // doing anything.
        int              previous_over_tris = -1;
        float            shell_cap_share    = s.retopo.hard_rules.secondary_shell_budget_share;

        for (int attempt = 0; attempt < kBudgetAttempts; ++attempt) {
            panel.resolve_budgets(effective_budget);

            // --- density ----------------------------------------------------
            set_stage(Stage::BuildingDensity, format("iteration %d", iteration));
            density = DensityField{};
            build_density_field(source, analysis, seg, panel, s.profile, density, s.density);
            if (cancelled()) return false;

            // --- retopo -----------------------------------------------------
            set_stage(Stage::Retopologising, format("iteration %d", iteration));
            RetopoOptions ropts = s.retopo;
            ropts.hard_rules.secondary_shell_budget_share = shell_cap_share;
            if (s.force_backend) {
                ropts.forced_backend_valid = true;
                ropts.forced_backend       = s.backend;
            }
            retopo = run_retopo(source, analysis, seg, density, panel, s.profile, ropts,
                                [&](float f, const char* what) { set_progress(f, what); });
            if (cancelled()) return false;
            if (!retopo.ok) {
                fail("retopology failed: " + retopo.error);
                return false;
            }

            // --- bake -------------------------------------------------------
            set_stage(Stage::Baking, format("iteration %d", iteration));
            bake = bake_all(retopo.mesh, source, analysis.bvh, analysis, s.profile,
                            panel.global, s.bake,
                            [&](float f, const char* what) { set_progress(f, what); });
            if (cancelled()) return false;
            if (!bake.ok) RD_WARN("bake failed: %s", bake.error.c_str());

            // Unwrapping rebuilt the vertex list, so the region map must follow.
            transfer_regions(source, seg, analysis.bvh, retopo.mesh);
            retopo.region_triangles.assign(retopo.region_budgets.size(), 0);
            for (uint16_t r : retopo.mesh.tri_region)
                if (r < retopo.region_triangles.size()) ++retopo.region_triangles[r];

            // --- strips, for the validator ----------------------------------
            strips = StripData{};
            if (s.profile.require_strips) {
                Mesh probe = retopo.mesh;
                optimise_for_target(probe, s.profile);
                strips = build_strips(probe, s.profile);
            }

            // --- validate ---------------------------------------------------
            set_stage(Stage::Validating, format("iteration %d", iteration));
            ValidationInput vin;
            vin.mesh             = &retopo.mesh;
            vin.profile          = &s.profile;
            vin.segmentation     = &seg;
            vin.panel            = &panel;
            vin.strips           = s.profile.require_strips ? &strips : nullptr;
            vin.bake             = &bake;
            vin.source_analysis  = &analysis;
            vin.region_triangles = &retopo.region_triangles;
            validation = validate(vin);

            const int tris  = int(retopo.mesh.triangle_count());
            const int verts = int(retopo.mesh.vertex_count());
            const bool over_tri  = tris  > s.profile.max_triangles;
            const bool over_vert = verts > s.profile.max_vertices;

            // Score this attempt so the run can fall back to the best one. The
            // unwrap is not a smooth function of the triangle budget - the seam
            // count jumps around - so the last attempt is often not the best.
            const int overshoot = std::max(0, tris - s.profile.max_triangles) +
                                  std::max(0, verts - s.profile.max_vertices);
            const int score = overshoot * 10000 - tris;   // legal first, then fullest
            if (score < best_score) {
                best_score      = score;
                best_density    = density;
                best_retopo     = retopo;
                best_bake       = bake;
                best_strips     = strips;
                best_validation = validation;
                best_panel      = panel;
                have_best       = true;
            }

            // How full the tighter of the two limits is. Whichever binds decides.
            const float fill = std::max(float(tris) / float(std::max(1, s.profile.max_triangles)),
                                        float(verts) / float(std::max(1, s.profile.max_vertices)));
            const bool legal = !over_tri && !over_vert;
            if (legal) legal_budget   = std::max(legal_budget, effective_budget);
            else       illegal_budget = std::min(illegal_budget, effective_budget);

            // A smaller budget that barely moved the triangle count means the
            // count has a floor: dozens of separate pieces, each already as
            // small as a closed piece can be. Shrinking further only starves
            // the body, so the next attempt caps what the pieces may hold.
            if (!legal && previous_over_tris > 0 && shell_cap_share <= 0.0f &&
                tris * 10 > previous_over_tris * 9) {
                shell_cap_share = 0.4f;
                RD_INFO("budget re-fit stalled at %d triangles; capping the loose pieces at "
                        "%.0f%% of the budget", tris, shell_cap_share * 100.0f);
                // What was learnt about budgets no longer holds with fewer
                // pieces, so the bracket starts over at the profile's own.
                legal_budget       = -1;
                illegal_budget     = std::numeric_limits<int>::max();
                previous_over_tris = -1;
                effective_budget   = s.profile.max_triangles;
                if (attempt + 1 < kBudgetAttempts) continue;
            }
            if (!legal) previous_over_tris = tris;

            if (legal && fill >= kBudgetFillTarget) break;

            if (attempt + 1 >= kBudgetAttempts) {
                if (!legal || !have_best)
                    RD_WARN("still over budget after %d attempts (%d tri, %d vtx); keeping the "
                            "closest attempt and letting validation say so",
                            kBudgetAttempts, tris, verts);
                break;
            }

            // Both directions are a proportional step on the binding limit,
            // bracketed by what has already been tried: never back up to a
            // budget known to be over, never down to one known to be legal.
            // Aim a little short, because the seam count does not scale
            // linearly with the triangle count and aiming at the limit
            // overshoots it again.
            const float scale = std::min(float(s.profile.max_triangles) / float(std::max(1, tris)),
                                         float(s.profile.max_vertices) / float(std::max(1, verts)));
            int next = legal ? int(float(effective_budget) * std::min(scale * 0.97f, 2.0f))
                             : int(float(effective_budget) * scale * 0.92f);
            if (next >= illegal_budget) next = (std::max(legal_budget, effective_budget) + illegal_budget) / 2;
            if (next <= legal_budget)   next = (legal_budget + std::min(illegal_budget, effective_budget)) / 2;
            next = std::max(16, next);
            // Less than two percent apart is noise in the backend, not a step.
            if (std::abs(next - effective_budget) * 50 < effective_budget) break;

            RD_INFO("budget re-fit: %d tri / %d vtx against limits %d / %d (%.0f%% full), "
                    "retrying with a %d triangle budget", tris, verts, s.profile.max_triangles,
                    s.profile.max_vertices, fill * 100.0f, next);
            effective_budget = next;
        }

        if (have_best) {
            density    = std::move(best_density);
            retopo     = std::move(best_retopo);
            bake       = std::move(best_bake);
            strips     = std::move(best_strips);
            validation = std::move(best_validation);
            panel      = std::move(best_panel);
        }

        // --- candidate renders -----------------------------------------------
        set_stage(Stage::CandidateRenders, format("iteration %d", iteration));
        RenderOptions opts;
        opts.width   = s.render_size;
        opts.height  = s.render_size;
        opts.samples = s.render_samples;
        opts.mode    = RenderMode::Shaded;
        opts.wireframe_overlay = true;

        const fs::path iter_dir = paths::iteration_dir(iteration);
        ViewSet candidate = render_views(retopo.mesh, opts, iter_dir, "lowpoly", nullptr,
                                         bake.ok ? &bake.diffuse : nullptr, true);
        if (!candidate.ok) RD_WARN("candidate renders unavailable: %s", candidate.error.c_str());

        SilhouetteError silhouette;
        {
            std::lock_guard lock(results_mutex_);
            silhouette = compare_silhouettes(results_.reference_views, candidate);
        }

        if (bake.ok && !bake.diffuse.empty())
            bake.diffuse.save_png(iter_dir / "diffuse.png");
        {
            std::string err;
            json_save_file((iter_dir / "knobs.json").string(), panel.to_json(), err);
            json_save_file((iter_dir / "validation.json").string(), validation.to_json(), err);
        }

        // --- record ----------------------------------------------------------
        IterationRecord record;
        record.index      = iteration;
        record.panel      = panel;
        record.triangles  = retopo.mesh.triangle_count();
        record.vertices   = retopo.mesh.vertex_count();
        record.silhouette = silhouette;
        record.validation_passed = validation.passed;
        record.errors     = validation.errors;
        record.warnings   = validation.warnings;
        record.backend    = retopo.backend_used;
        record.seconds    = iteration_watch.seconds();
        for (const ViewSet::Entry& e : candidate.entries) record.renders.push_back(e.file);

        {
            std::lock_guard lock(results_mutex_);
            results_.lowpoly         = retopo.mesh;
            results_.density         = std::move(density);
            results_.retopo          = retopo;
            results_.bake            = bake;
            results_.validation      = validation;
            results_.candidate_views = candidate;
            results_.silhouette      = silhouette;
            results_.panel           = panel;
            if (better(validation, silhouette, kept)) {
                kept.iteration  = iteration;
                kept.lowpoly    = results_.lowpoly;
                kept.density    = results_.density;
                kept.retopo     = retopo;
                kept.bake       = bake;
                kept.validation = validation;
                kept.candidate  = candidate;
                kept.silhouette = silhouette;
                kept.panel      = panel;
            }
        }
        bump();

        RD_INFO("iteration %d: %zu tri, silhouette mean %.4f worst %.4f, validation %s",
                iteration, record.triangles, silhouette.mean, silhouette.worst,
                validation.passed ? "passed" : "FAILED");

        const bool last_iteration = iteration >= max_iterations;
        bool stop = last_iteration;

        // --- director review --------------------------------------------------
        if (s.use_llm && llm_ && !cancelled()) {
            set_stage(Stage::Reviewing, format("iteration %d", iteration));

            IterationFacts facts;
            facts.iteration      = iteration;
            facts.max_iterations = max_iterations;
            facts.triangles      = record.triangles;
            facts.vertices       = record.vertices;
            facts.silhouette     = silhouette;
            facts.retopo         = &retopo;
            facts.validation     = &validation;
            facts.bake           = &bake;

            ViewSet reference;
            {
                std::lock_guard lock(results_mutex_);
                reference = results_.reference_views;
            }

            const LlmRequest req =
                validation.passed
                    ? build_review_request(source, seg, s.profile, panel, facts, reference,
                                           candidate)
                    : build_repair_request(seg, s.profile, panel, validation, retopo);

            const LlmResponse res = ask(req, validation.passed ? "review" : "repair");
            if (cancelled()) return false;

            const ReviewOutcome outcome = parse_review_response(res);
            note_advice(parse_advice_response(res, s.profile), &record);
            if (outcome.ok) {
                record.verdict  = outcome.verdict;
                record.critique = outcome.critique;

                KnobPanel next = panel;
                const KnobPanel::ApplyReport applied = next.apply_patch(outcome.patch);
                next.resolve_budgets(s.profile.max_triangles);
                {
                    std::lock_guard lock(results_mutex_);
                    results_.panel = next;
                }
                bump();

                RD_INFO("director verdict '%s': %s (%d fields changed)",
                        outcome.verdict.c_str(), outcome.critique.c_str(),
                        applied.fields_changed);

                const bool accepted = outcome.verdict == "accept" && validation.passed;
                const bool no_change = applied.fields_changed == 0;
                stop = last_iteration || accepted ||
                       (!outcome.wants_another_pass && validation.passed) || no_change;
                if (no_change && !accepted)
                    RD_INFO("the patch changed nothing, so another pass would repeat itself");
            } else {
                RD_WARN("review skipped: %s", outcome.error.c_str());
                stop = true;
            }
        } else if (!s.use_llm) {
            stop = true;
        }

        {
            std::lock_guard lock(results_mutex_);
            results_.iterations.push_back(std::move(record));
        }
        bump();

        if (stop) break;
    }

    {
        std::lock_guard lock(results_mutex_);
        const int last = results_.iterations.empty() ? 0 : results_.iterations.back().index;
        results_.kept_iteration = kept.iteration;
        if (kept.iteration != 0 && kept.iteration != last) {
            RD_INFO("keeping iteration %d (silhouette %.4f) over the last one, %d (%.4f)",
                    kept.iteration, kept.silhouette.mean, last, results_.silhouette.mean);
            results_.lowpoly         = std::move(kept.lowpoly);
            results_.density         = std::move(kept.density);
            results_.retopo          = std::move(kept.retopo);
            results_.bake            = std::move(kept.bake);
            results_.validation      = std::move(kept.validation);
            results_.candidate_views = std::move(kept.candidate);
            results_.silhouette      = kept.silhouette;
            // The panel that built it, so Rebuild reproduces what is on screen
            // rather than the director's untried next guess.
            results_.panel           = std::move(kept.panel);
        }
    }
    bump();
    return true;
}

bool Pipeline::stage_export(const PipelineSettings& s)
{
    if (!s.auto_export) return true;
    set_stage(Stage::Exporting, "writing the asset");

    Mesh    mesh;
    Texture diffuse;
    Palette palette;
    {
        std::lock_guard lock(results_mutex_);
        mesh    = results_.lowpoly;
        diffuse = results_.bake.diffuse;
        palette = results_.bake.palette;
    }
    if (mesh.empty()) {
        RD_WARN("nothing to export");
        return true;
    }

    ExportOptions opts = s.exporting;
    if (opts.base_name.empty() || opts.base_name == "lowpoly") {
        std::lock_guard lock(results_mutex_);
        if (!results_.source_path.empty())
            opts.base_name = slugify(results_.source_path.stem().string()) + "_lowpoly";
    }

    ExportResult exported = export_asset(mesh, diffuse, palette, s.profile,
                                         paths::export_dir(), opts);
    if (!exported.ok) RD_WARN("export incomplete: %s", exported.error.c_str());

    {
        std::lock_guard lock(results_mutex_);
        results_.exported = std::move(exported);
        // The exporter reordered the mesh in place; keep the shipped version.
        results_.lowpoly = std::move(mesh);
    }
    bump();
    return true;
}

bool Pipeline::stage_report(const PipelineSettings& s)
{
    if (!s.write_report) return true;
    set_stage(Stage::Reporting, "assembling the report");

    std::lock_guard lock(results_mutex_);
    const PipelineResults& r = results_;

    // --- machine readable ---------------------------------------------------
    Json j;
    j["source"]  = r.source_path.string();
    j["profile"] = s.profile.to_json();
    j["panel"]   = r.panel.to_json();
    j["validation"] = r.validation.to_json();
    j["silhouette"] = Json{{"mean", r.silhouette.mean},
                           {"worst", r.silhouette.worst},
                           {"outline_px", r.silhouette.outline_px},
                           {"worst_view", r.silhouette.worst_view}};
    // One flat block with the numbers a run is judged by, so tools/bench.py can
    // compare two runs without knowing the shape of the rest of this file.
    {
        int budget_sum = 0, budget_used = 0;
        for (const RegionKnobs& k : r.panel.regions) {
            budget_sum += k.triangle_budget;
            if (k.id < r.retopo.region_triangles.size())
                budget_used += r.retopo.region_triangles[k.id];
        }
        j["summary"] = Json{
            {"highpoly_triangles", r.highpoly.triangle_count()},
            {"highpoly_vertices", r.highpoly.vertex_count()},
            {"lowpoly_triangles", r.lowpoly.triangle_count()},
            {"lowpoly_vertices", r.lowpoly.vertex_count()},
            {"max_triangles", s.profile.max_triangles},
            {"max_vertices", s.profile.max_vertices},
            {"regions", r.panel.regions.size()},
            {"region_budget_sum", budget_sum},
            {"region_triangles_sum", budget_used},
            {"silhouette_mean", r.silhouette.mean},
            {"silhouette_worst", r.silhouette.worst},
            {"outline_px", r.silhouette.outline_px},
            {"validation_passed", r.validation.passed},
            {"errors", r.validation.errors},
            {"warnings", r.validation.warnings},
            {"iterations", r.iterations.size()},
            {"kept_iteration", r.kept_iteration},
        };
    }

    Json iterations = Json::array();
    for (const IterationRecord& it : r.iterations) {
        Json e;
        e["index"]      = it.index;
        e["triangles"]  = it.triangles;
        e["vertices"]   = it.vertices;
        e["backend"]    = backend_name(it.backend);
        e["silhouette_mean"]  = it.silhouette.mean;
        e["silhouette_worst"] = it.silhouette.worst;
        e["outline_px"]       = it.silhouette.outline_px;
        e["validation_passed"] = it.validation_passed;
        e["errors"]     = it.errors;
        e["warnings"]   = it.warnings;
        e["verdict"]    = it.verdict;
        e["critique"]   = it.critique;
        e["seconds"]    = it.seconds;
        iterations.push_back(e);
    }
    j["iterations"] = iterations;
    if (!r.advice.empty()) j["profile_advice"] = r.advice.to_json();

    Json files = Json::array();
    for (const fs::path& f : r.exported.files) files.push_back(f.string());
    j["exported"] = files;

    std::string err;
    json_save_file((paths::reports_dir() / "report.json").string(), j, err);

    // --- for a human --------------------------------------------------------
    std::string md;
    md += "# Retopo Director report\n\n";
    md += format("- source: `%s`\n", r.source_path.filename().string().c_str());
    md += format("- profile: **%s** (%d triangles, %d vertices)\n", s.profile.name.c_str(),
                 s.profile.max_triangles, s.profile.max_vertices);
    md += format("- high poly: %zu triangles\n", r.highpoly.triangle_count());
    md += format("- low poly: %zu triangles, %zu vertices\n", r.lowpoly.triangle_count(),
                 r.lowpoly.vertex_count());
    md += format("- silhouette error: mean %.4f, worst %.4f%s%s; outline off by %.2f px\n",
                 r.silhouette.mean, r.silhouette.worst,
                 r.silhouette.worst_view.empty() ? "" : " on ", r.silhouette.worst_view.c_str(),
                 r.silhouette.outline_px);
    if (r.iterations.size() > 1)
        md += format("- kept iteration %d of %zu\n", r.kept_iteration, r.iterations.size());
    md += format("- validation: **%s** (%d errors, %d warnings)\n\n",
                 r.validation.passed ? "passed" : "failed", r.validation.errors,
                 r.validation.warnings);

    md += "## Regions\n\n";
    md += "| region | budget | actual | fidelity | rationale |\n";
    md += "|---|---:|---:|---|---|\n";
    for (const RegionKnobs& k : r.panel.regions) {
        const int actual = k.id < r.retopo.region_triangles.size()
                               ? r.retopo.region_triangles[k.id] : 0;
        md += format("| %s | %d | %d | %s | %s |\n", k.name.c_str(), k.triangle_budget,
                     actual, fidelity_name(k.fidelity),
                     k.rationale.empty() ? "-" : k.rationale.c_str());
    }

    md += "\n## Iterations\n\n";
    md += "| # | triangles | silhouette mean | validation | verdict |\n";
    md += "|---:|---:|---:|---|---|\n";
    for (const IterationRecord& it : r.iterations)
        md += format("| %d | %zu | %.4f | %s | %s |\n", it.index, it.triangles,
                     it.silhouette.mean, it.validation_passed ? "passed" : "failed",
                     it.verdict.empty() ? "-" : it.verdict.c_str());

    for (const IterationRecord& it : r.iterations) {
        if (it.critique.empty()) continue;
        md += format("\n**Iteration %d critique.** %s\n", it.index, it.critique.c_str());
    }

    if (!r.advice.empty()) {
        md += "\n## What the director thinks of the brief\n\n";
        md += format("Feasibility: **%s**. %s\n\n",
                     feasibility_name(r.advice.feasibility), r.advice.headline.c_str());
        if (!r.advice.proposals.empty()) {
            md += "These are proposals, not changes. Nothing below was applied to "
                  "this run.\n\n";
            md += "| field | change | reason |\n|---|---|---|\n";
            for (const ProfileProposal& prop : r.advice.proposals)
                md += format("| `%s` | %s | %s%s |\n", prop.field.c_str(),
                             prop.applicable ? prop.change_text().c_str() : "-",
                             prop.reason.c_str(),
                             prop.note.empty() ? ""
                                               : format(" _(%s)_", prop.note.c_str()).c_str());
        }
    }

    md += "\n## Validation detail\n\n```\n";
    md += r.validation.full_text();
    md += "```\n";

    if (!r.exported.files.empty()) {
        md += "\n## Exported files\n\n";
        for (const fs::path& f : r.exported.files)
            md += format("- `%s`\n", f.filename().string().c_str());
    }

    paths::write_file(paths::reports_dir() / "report.md", md);
    return true;
}

// ---------------------------------------------------------------------------
void Pipeline::run(Entry entry, fs::path mesh_path, PipelineSettings settings)
{
    Stopwatch watch;

    // A preview is not a run: no model, no project folders, no report. It exists
    // so that picking a file puts something on screen straight away.
    if (entry == Entry::PreviewOnly) {
        preview_.store(true);
        const bool loaded = stage_load(mesh_path, settings);
        if (loaded) set_stage(Stage::Idle, mesh_path.filename().string());
        progress_.store(0.0f);
        bump();
        preview_.store(false);
        running_.store(false);
        return;
    }

    llm_ = settings.use_llm ? make_llm_backend(settings.llm) : nullptr;

    if (llm_) {
        std::string reason;
        if (!llm_->available(&reason)) {
            RD_WARN("%s is unavailable (%s); running without the director",
                    llm_->name(), reason.c_str());
            llm_.reset();
            settings.use_llm = false;
        } else {
            RD_INFO("director backend: %s  [%s]", llm_->name(), llm_->describe().c_str());
        }
    }

    paths::ensure_dir(paths::renders_dir());
    paths::ensure_dir(paths::reports_dir());

    // The engine path returns result structs rather than throwing, but it calls
    // into third party loaders and the standard library, and an exception that
    // reaches the top of this thread is std::terminate - the window vanishes
    // with nothing on screen and nothing in the log. Turn it into the failure it
    // is, so the message lands in the Pipeline panel like any other.
    bool ok = true;
    try {
        if (entry == Entry::Full) {
            ok = stage_load(mesh_path, settings) &&
                 stage_analyse(settings) &&
                 stage_reference_renders(settings) &&
                 stage_segment(settings) &&
                 stage_name_regions(settings) &&
                 stage_allocate_budget(settings);
        }

        if (ok && !cancelled()) ok = stage_iterate(settings);
        if (ok && !cancelled()) ok = stage_export(settings);
        if (ok && !cancelled()) ok = stage_report(settings);
    } catch (const std::exception& e) {
        fail(std::string("the run hit an unexpected error: ") + e.what());
        ok = false;
    } catch (...) {
        fail("the run hit an unexpected error of unknown type");
        ok = false;
    }

    {
        std::lock_guard lock(results_mutex_);
        results_.total_seconds = watch.seconds();
    }

    if (cancelled())      set_stage(Stage::Cancelled, "cancelled by the user");
    else if (!ok)         { if (stage_.load() != Stage::Failed) fail("the run stopped early"); }
    else                  set_stage(Stage::Done, format("finished in %s",
                                                        format_duration(watch.seconds()).c_str()));

    progress_.store(1.0f);
    bump();
    llm_.reset();
    running_.store(false);
}

} // namespace rd
