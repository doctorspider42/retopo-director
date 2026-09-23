#pragma once

// The one place where the model is allowed to disagree with the brief.
//
// The knob panel lets the director spend a budget; it does not let it say the
// budget is wrong. That gap is real: 100 triangles for a humanoid is not a
// tight brief, it is an impossible one, and a director that can only redistribute
// will redistribute right up to the point where the asset is worthless and never
// mention it. Advice is the channel for that sentence.
//
// Nothing in here is ever applied by the pipeline. A proposal is resolved
// against the current profile, clamped to the range the profile itself accepts,
// and handed to a human to accept or ignore. `TargetProfile` stays what its
// header says it is: authored by a human.

#include "core/json.h"
#include "knobs/profile.h"

#include <string>
#include <vector>

namespace rd {

// How the director rates the brief against the model it was handed.
enum class Feasibility : uint8_t {
    Unknown = 0,   // never asked, or the reply did not say
    Comfortable,   // the budget has room to spare
    Tight,         // achievable, but something has to be given up
    Strained,      // the result will read poorly; the brief is the reason
    Impossible     // no allocation of this budget produces a usable asset
};

const char* feasibility_name(Feasibility f);
Feasibility feasibility_from_name(std::string_view s, Feasibility fallback = Feasibility::Unknown);
// True once the director is saying the brief, not the allocation, is the problem.
bool        feasibility_is_alarming(Feasibility f);

// One proposed change to one profile field.
struct ProfileProposal {
    std::string field;       // canonical name, e.g. "max_triangles", "texture.width"
    double      proposed = 0.0;
    double      current  = 0.0;   // read off the profile at parse time
    std::string reason;

    // False when the field is not proposable or the value was nonsense. Kept in
    // the list rather than dropped so the report can show that the director
    // asked for something it is not allowed to ask for.
    bool        applicable = false;
    std::string note;        // why it is not applicable, or what it was clamped to

    // Human readable "1200 -> 2400".
    std::string change_text() const;
};

struct ProfileAdvice {
    Feasibility                  feasibility = Feasibility::Unknown;
    std::string                  headline;    // one sentence, shown in the UI
    std::vector<ProfileProposal> proposals;

    bool empty() const { return feasibility == Feasibility::Unknown && headline.empty() &&
                                proposals.empty(); }
    int  applicable_count() const;

    Json to_json() const;
};

// The fields the director may propose, with the range it may propose inside.
// Everything here is a scalar the profile already clamps; nothing structural,
// nothing that would change what the validator is checking against.
struct ProposableField {
    const char* name;
    const char* description;
    double      min;
    double      max;
    bool        integral;
};
const std::vector<ProposableField>& proposable_profile_fields();

// The catalogue as prompt text, so the model is told the exact names and ranges
// instead of guessing them.
std::string proposable_fields_text(const TargetProfile& profile);

// Reads an "advice" object out of a model reply. Never throws; an unknown field
// comes back as a proposal with `applicable` false rather than being dropped,
// because "the director wanted 8 bone influences" is worth seeing.
ProfileAdvice parse_profile_advice(const Json& j, const TargetProfile& against);

// Writes one proposal into a profile. Returns false when the proposal is not
// applicable. The profile is clamped afterwards, so this cannot wedge a run.
bool apply_proposal(TargetProfile& profile, const ProfileProposal& p);

// Folds a later opinion into an earlier one. The headline and the verdict are
// replaced, because a pass that has seen the render knows better than one that
// has not; the proposals are merged by field, because it usually does not
// mention the atlas again once it has started worrying about the silhouette,
// and dropping that earlier proposal would lose a note a human still wants.
void merge_advice(ProfileAdvice& into, const ProfileAdvice& latest);

// For the log and the report.
std::string advice_text(const ProfileAdvice& advice);

// --- the deterministic half -------------------------------------------------
// What the engine can say about the brief without asking anybody. This goes
// into the prompt so the director is reacting to arithmetic rather than to a
// vibe: a model that is told "24 triangles per region" objects far more
// reliably than one left to infer it from a total.
struct BudgetPressure {
    int    budget          = 0;
    size_t source_triangles = 0;
    int    regions         = 0;
    float  per_region      = 0.0f;   // triangles per named region, after symmetry
    float  decimation      = 0.0f;   // budget / source, 0..1
    bool   symmetry_halves = false;  // budget effectively covers one side
    std::string verdict;             // one line of plain arithmetic, no opinion
};
BudgetPressure budget_pressure(size_t source_triangles, int regions,
                               const TargetProfile& profile, bool symmetry_enforced);
std::string    budget_pressure_text(const BudgetPressure& p);

} // namespace rd
