#pragma once

// The orchestrator. Owns every intermediate product of a run, drives the stages
// on a worker thread, and borrows the main thread whenever it needs the GPU.

#include "bake/bake.h"
#include "core/dispatcher.h"
#include "export/exporter.h"
#include "geom/retopo.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "llm/backend.h"
#include "mesh/analysis.h"
#include "mesh/io.h"
#include "pipeline/director.h"
#include "render/renderer.h"
#include "segment/segment.h"
#include "segment/segmenter.h"
#include "validate/validator.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rd {

enum class Stage : uint8_t {
    Idle = 0,
    Loading,
    Analysing,
    ReferenceRenders,
    Segmenting,
    NamingRegions,
    AllocatingBudget,
    BuildingDensity,
    Retopologising,
    Baking,
    Validating,
    CandidateRenders,
    Reviewing,
    Exporting,
    Reporting,
    Done,
    Failed,
    Cancelled
};

const char* stage_name(Stage s);
bool        stage_is_terminal(Stage s);

// Where a stage sits in a whole run, 0..1, so one bar can stand for the entire
// thing instead of one bar per stage. `within` is the stage's own progress.
//
// When the iteration count is known the loop stages are compressed into the
// slice belonging to the current iteration, which keeps the bar moving forward
// instead of rewinding every time the director asks for another pass.
float stage_overall_progress(Stage s, float within, int iteration = 0,
                             int total_iterations = 0);

struct PipelineSettings {
    TargetProfile       profile;
    LlmConfig           llm;
    AnalysisOptions     analysis;
    SegmentationOptions segmentation;
    SegmenterOptions    segmenter;
    DensityOptions      density;
    RetopoOptions       retopo;
    BakeOptions         bake;
    ExportOptions       exporting;
    meshio::LoadOptions loading;

    // Renders handed to the model. Bigger is not better: the model reads a 640px
    // silhouette as well as a 2048px one, and the upload costs real time.
    int  render_size   = 640;
    int  render_samples = 4;

    bool use_llm      = true;
    bool auto_export  = true;
    bool write_report = true;
    // 0 takes the profile value.
    int  max_iterations = 0;

    // When set, overrides whatever backend the panel asks for. Used by the
    // command line and by anyone comparing the two paths on one mesh.
    bool          force_backend = false;
    RetopoBackend backend       = RetopoBackend::Auto;
};

struct IterationRecord {
    int        index = 0;
    KnobPanel  panel;
    size_t     triangles = 0;
    size_t     vertices  = 0;
    SilhouetteError silhouette;
    bool       validation_passed = false;
    int        errors = 0, warnings = 0;
    std::string verdict;
    std::string critique;
    ProfileAdvice advice;
    std::vector<std::filesystem::path> renders;
    double     seconds = 0.0;
    RetopoBackend backend = RetopoBackend::Auto;
};

struct PipelineResults {
    Mesh             highpoly;
    Mesh             lowpoly;
    MeshAnalysis     analysis;
    Segmentation     segmentation;
    KnobPanel        panel;
    // The director's standing objection to the brief, latest wins. Advisory:
    // the pipeline reads it only to write it down.
    ProfileAdvice    advice;
    DensityField     density;
    RetopoResult     retopo;
    BakeResult       bake;
    ValidationReport validation;
    ExportResult     exported;

    std::vector<ViewCamera> cameras;
    ViewSet                 reference_views;
    ViewSet                 region_views;
    ViewSet                 candidate_views;
    SilhouetteError         silhouette;

    std::vector<LlmExchange>     transcript;
    std::vector<IterationRecord> iterations;

    std::filesystem::path source_path;
    meshio::LoadReport    load_report;
    double                total_seconds = 0.0;
};

class Pipeline {
public:
    Pipeline();
    ~Pipeline();

    void set_dispatcher(MainThreadDispatcher* d) { dispatcher_ = d; }
    void set_renderer(Renderer* r) { renderer_ = r; }

    // Starts a full run. Returns false if one is already going.
    bool start(const std::filesystem::path& mesh_path, const PipelineSettings& settings);
    // Re-runs from the density stage using the current panel, without the model.
    bool rebuild_geometry(const PipelineSettings& settings);
    // Loads the mesh and nothing else, so the viewport has something to show
    // the moment a file is picked. Leaves the stage on Idle: this is not a run.
    bool preview(const std::filesystem::path& mesh_path, const PipelineSettings& settings);

    void cancel();
    void join();

    bool  running()  const { return running_.load(); }
    Stage stage()    const { return stage_.load(); }
    float progress() const { return progress_.load(); }
    int   iteration() const { return iteration_.load(); }
    std::string message() const;
    std::string error() const;
    uint64_t    version() const { return version_.load(); }

    // Runs `fn` against the results under the lock. Keep it short.
    template <typename Fn>
    void with_results(Fn&& fn) const
    {
        std::lock_guard lock(results_mutex_);
        fn(results_);
    }

    // Replaces the knob panel from the UI between runs.
    void set_panel(const KnobPanel& panel);

private:
    enum class Entry { Full, GeometryOnly, PreviewOnly };

    void launch(Entry entry, const std::filesystem::path& mesh_path,
                const PipelineSettings& settings);
    void run(Entry entry, std::filesystem::path mesh_path, PipelineSettings settings);

    void set_stage(Stage s, const std::string& message);
    void set_progress(float f, const std::string& message);
    void fail(const std::string& message);
    bool cancelled() const { return cancel_.load(); }
    void bump();

    // Stage helpers. Each returns false when the run should stop.
    bool stage_load(const std::filesystem::path& path, const PipelineSettings& s);
    bool stage_analyse(const PipelineSettings& s);
    bool stage_reference_renders(const PipelineSettings& s);
    bool stage_segment(const PipelineSettings& s);
    bool stage_name_regions(const PipelineSettings& s);
    bool stage_allocate_budget(const PipelineSettings& s);
    bool stage_iterate(const PipelineSettings& s);
    bool stage_export(const PipelineSettings& s);
    bool stage_report(const PipelineSettings& s);

    // Renders on the main thread, blocking the worker until it is done.
    ViewSet render_views(const Mesh& mesh, const RenderOptions& opts,
                         const std::filesystem::path& dir, const std::string& prefix,
                         const std::vector<Vec4>* face_colors, const Texture* diffuse,
                         bool masks);

    LlmResponse ask(const LlmRequest& req, const char* stage_label);
    void        note_advice(const ProfileAdvice& advice, IterationRecord* record);

    MainThreadDispatcher* dispatcher_ = nullptr;
    Renderer*             renderer_   = nullptr;

    std::thread                  worker_;
    std::atomic<bool>            running_{false};
    std::atomic<bool>            cancel_{false};
    // True while the worker is only loading a mesh for the viewport, which a
    // real run is allowed to interrupt.
    std::atomic<bool>            preview_{false};
    std::atomic<Stage>           stage_{Stage::Idle};
    std::atomic<float>           progress_{0.0f};
    std::atomic<int>             iteration_{0};
    std::atomic<uint64_t>        version_{0};

    mutable std::mutex           status_mutex_;
    std::string                  message_;
    std::string                  error_;

    mutable std::mutex           results_mutex_;
    PipelineResults              results_;

    std::unique_ptr<ILlmBackend> llm_;
};

} // namespace rd
