#pragma once

// Everything the language model is ever told, and everything we are willing to
// believe back. Prompt text lives here rather than being scattered through the
// pipeline so it can be read, reviewed and diffed as one artefact.

#include "geom/retopo.h"
#include "knobs/advice.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "llm/backend.h"
#include "mesh/analysis.h"
#include "render/renderer.h"
#include "segment/segment.h"
#include "validate/validator.h"

#include <string>
#include <vector>

namespace rd {

// --- role description, shared by every request ------------------------------
std::string director_system_prompt();

// --- stage 1: name and merge the automatic patches --------------------------
struct RegionNaming {
    struct Named {
        uint16_t    id = 0;
        std::string name;
        std::string role;
    };
    std::vector<Named>      named;
    std::vector<MergeGroup> merges;
    std::string             notes;
    bool                    ok = false;
    std::string             error;
};

LlmRequest  build_naming_request(const Mesh& mesh, const Segmentation& seg,
                                 const TargetProfile& profile, const ViewSet& region_views,
                                 const ViewSet& shaded_views);
RegionNaming parse_naming_response(const LlmResponse& res, const Segmentation& seg);

// --- stage 2: allocate the budget ------------------------------------------
LlmRequest build_budget_request(const Mesh& mesh, const MeshAnalysis& analysis,
                                const Segmentation& seg, const TargetProfile& profile,
                                const KnobPanel& current, const ViewSet& shaded_views,
                                const ViewSet& region_views);

// Every reply may carry an "profile_advice" block arguing that the brief itself
// is wrong. Reading it is separate from reading the knobs because the two go to
// different places: knobs to the engine, advice to a human.
ProfileAdvice parse_advice_response(const LlmResponse& res, const TargetProfile& profile);

// --- stage 3: review an iteration ------------------------------------------
struct ReviewOutcome {
    Json        patch;              // knob panel patch, applied through apply_patch
    std::string verdict;            // "accept" | "revise"
    std::string critique;
    bool        wants_another_pass = false;
    bool        ok = false;
    std::string error;
};

struct IterationFacts {
    int              iteration = 0;
    int              max_iterations = 4;
    size_t           triangles = 0;
    size_t           vertices  = 0;
    int              max_triangles = 0;   // the profile's limits, to say which one binds
    int              max_vertices  = 0;
    SilhouetteError  silhouette;
    const RetopoResult*     retopo = nullptr;
    const ValidationReport* validation = nullptr;
    const BakeResult*       bake = nullptr;
};

LlmRequest    build_review_request(const Mesh& source, const Segmentation& seg,
                                   const TargetProfile& profile, const KnobPanel& panel,
                                   const IterationFacts& facts,
                                   const ViewSet& reference_views,
                                   const ViewSet& candidate_views);
ReviewOutcome parse_review_response(const LlmResponse& res);

// --- stage 2b: the model is shown a validation failure instead of renders ---
LlmRequest build_repair_request(const Segmentation& seg, const TargetProfile& profile,
                                const KnobPanel& panel, const ValidationReport& report,
                                const RetopoResult& retopo);

// Shared helpers, exposed for the prompt preview panel in the UI.
std::string profile_summary_text(const TargetProfile& profile);
std::string knob_panel_summary_text(const KnobPanel& panel, const Segmentation& seg);
std::string metrics_text(const IterationFacts& facts);
// The advice section of the prompt: the field catalogue and the rules for using
// it. Exposed so the prompt preview panel shows the same text the model gets.
std::string profile_advice_prompt_text(const TargetProfile& profile);

} // namespace rd
