#include "pipeline/director.h"

#include "geom/hard_rules.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>

namespace rd {
namespace {

void attach(LlmRequest& req, const ViewSet& views, const char* prefix, int limit)
{
    int added = 0;
    for (const ViewSet::Entry& e : views.entries) {
        if (limit > 0 && added >= limit) break;
        // Primary cameras first matters: some backends truncate attachments.
        if (limit > 0 && !e.primary && added >= limit / 2 && views.entries.size() > size_t(limit))
            continue;
        LlmImage img;
        img.label = format("%s / %s%s%s", prefix, e.name.c_str(),
                           e.description.empty() ? "" : " - ",
                           e.description.c_str());
        img.path  = e.file;
        req.images.push_back(std::move(img));
        ++added;
    }
}

} // namespace

// ---------------------------------------------------------------------------
std::string director_system_prompt()
{
    return
R"(You are the art director on a retopology pipeline that targets PlayStation 2 class
hardware. You do not touch geometry. You never see vertices, edges or faces as
data. You see rendered images and a small table of measurements, and you fill in
a fixed JSON control panel. Deterministic code turns that panel into a mesh.

What you are good at, and what you are here for:
  - Reading a silhouette and saying which parts carry the character.
  - Deciding where a triangle earns its place and where it is decoration.
  - Knowing that a face seen in a cutscene needs geometry a shoulder pad does not.
  - Judging whether a low poly still reads as the same character.

Rules you must follow:
  - Answer with one JSON object, nothing else. No prose outside it, no markdown fence.
  - Only use the keys the schema defines. Unknown keys are discarded.
  - Budgets are relative shares, not triangle counts. The engine resolves them.
  - Never ask for geometry that the hard rules forbid; they will override you and
    the report will say you were overruled.
  - When you are unsure, say so in the notes field and pick the conservative option.
  - You may argue with the brief, in the one place provided for it. If the budget
    cannot carry this model, saying so is part of the job; quietly dividing an
    impossible budget into impossible pieces is not. You never change the brief
    yourself: a human reads your proposal and decides.
  - Be specific in rationale fields. "Important" is useless. "Read in every
    cutscene close-up" is useful.)";
}

std::string profile_summary_text(const TargetProfile& profile)
{
    std::string out;
    out += "TARGET PROFILE: " + profile.name + "\n";
    if (!profile.description.empty()) out += profile.description + "\n";
    out += format("  triangles      max %d\n", profile.max_triangles);
    out += format("  vertices       max %d\n", profile.max_vertices);
    out += format("  shells         max %s\n",
                  profile.max_shells > 0 ? std::to_string(profile.max_shells).c_str() : "unlimited");
    out += format("  bone influence max %d per vertex\n", profile.max_bone_influences);
    out += format("  symmetry       %s\n", profile.require_symmetry ? "required" : "not required");
    out += format("  strips         %s (average length >= %.1f)\n",
                  profile.require_strips ? "required" : "not required",
                  profile.min_average_strip_len);
    out += format("  texture        %dx%d, %d colour palette, %d page(s)\n",
                  profile.texture.width, profile.texture.height,
                  profile.texture.palette_colors, profile.texture.count);
    out += format("  vertex colours %s\n",
                  profile.require_vertex_colors ? "required" : "optional");
    out += format("  lighting       %s\n",
                  profile.bake_lighting_to_diffuse
                      ? "baked into the diffuse; there is no runtime lighting"
                      : "runtime");
    out += format("  silhouette     target error %.3f, hard limit %.3f\n",
                  profile.target_silhouette_error, profile.max_silhouette_error);
    if (!profile.art_direction.empty()) {
        out += "\nART DIRECTION:\n";
        out += profile.art_direction + "\n";
    }
    return out;
}

std::string knob_panel_summary_text(const KnobPanel& panel, const Segmentation& seg)
{
    std::string out;
    const GlobalKnobs& g = panel.global;
    out += format("global: backend=%s symmetry=%s joint_loops=%.2f merge=%.2f "
                  "curvature=%.2f silhouette=%.2f smoothing=%d quads=%.2f\n",
                  backend_name(g.backend), g.enforce_symmetry ? "on" : "off",
                  g.joint_loop_density, g.merge_aggressiveness, g.curvature_influence,
                  g.silhouette_weight, g.smoothing_iterations, g.quad_dominance);
    out += "id  | name        | share | budget | prio | fidelity | sil | curv\n";
    out += "----+-------------+-------+--------+------+----------+-----+-----\n";
    for (const RegionKnobs& k : panel.regions) {
        (void)seg;
        out += format("%-3u | %-11s | %5.2f | %6d | %.2f | %-8s | %s   | %.2f\n",
                      unsigned(k.id), k.name.substr(0, 11).c_str(), k.share,
                      k.triangle_budget, k.detail_priority, fidelity_name(k.fidelity),
                      k.preserve_silhouette ? "y" : "n", k.curvature_bias);
    }
    return out;
}

std::string metrics_text(const IterationFacts& facts)
{
    std::string out;
    out += format("iteration %d of %d\n", facts.iteration, facts.max_iterations);
    out += format("triangles %zu, vertices %zu\n", facts.triangles, facts.vertices);
    out += format("silhouette error: mean %.4f, worst %.4f",
                  facts.silhouette.mean, facts.silhouette.worst);
    if (!facts.silhouette.worst_view.empty())
        out += " on view '" + facts.silhouette.worst_view + "'";
    out += "\n";
    // The fraction punishes thin shapes for being thin: a figure with arms out
    // cannot reach a sphere's number however well it fits. The offset in pixels
    // is what says whether there is anything left to win, and without it the
    // director kept tearing a good panel apart to chase an unreachable target.
    if (facts.silhouette.outline_px > 0.0f)
        out += format("outline offset: %.2f px on average at the render size. Under about "
                      "1 px the outline is as close as these renders can show, and the error "
                      "fraction that remains is mostly the thin parts' perimeter, not a "
                      "misfit: prefer small, targeted changes over rebalancing the panel.\n",
                      facts.silhouette.outline_px);

    if (!facts.silhouette.per_view.empty()) {
        out += "per view silhouette error (fraction of the reference footprint that "
               "disagrees):\n";
        std::vector<std::pair<std::string, float>> sorted = facts.silhouette.per_view;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < sorted.size() && i < 10; ++i)
            out += format("  %-20s %.4f\n", sorted[i].first.c_str(), sorted[i].second);
    }

    if (facts.retopo) {
        out += format("backend: %s", backend_name(facts.retopo->backend_used));
        if (facts.retopo->quads > 0)
            out += format(", %.0f%% of triangles came from quads",
                          facts.retopo->quad_ratio * 100.0f);
        out += "\n";
        for (const std::string& m : facts.retopo->hard_rules.messages)
            out += "hard rule: " + m + "\n";
    }
    if (facts.bake && facts.bake->ok) {
        out += format("bake: %d charts, %.1f%% atlas used, worst uv stretch %.2f\n",
                      facts.bake->charts, facts.bake->uv_utilisation * 100.0f,
                      facts.bake->uv_max_stretch);
    }
    return out;
}

std::string profile_advice_prompt_text(const TargetProfile& profile)
{
    std::string out;
    out += R"(
THE BRIEF ITSELF

Everything above is what a human decided before seeing this model. They may have
been right. They may have picked round numbers for a placeholder and never come
back to them. You are the first thing in this pipeline that has looked at the
model and the brief at the same time, so you are the only one who can tell.

If, and only if, the brief is the problem, fill in "profile_advice". These are
proposals to a human, not settings: nothing you put here changes this run. They
cost somebody money and memory on a real console, so propose the smallest change
that makes the asset work and say what it buys.

  "profile_advice": {
    "feasibility": "comfortable" | "tight" | "strained" | "impossible",
    "headline": "one sentence a human reads before deciding whether to care",
    "propose": [
      { "field": "max_triangles", "value": 2400,
        "reason": "what specifically fails at the current value" }
    ]
  }

What "feasibility" means:
  comfortable  the budget has room; you could spend less than you were given.
  tight        achievable, but something visible has to be given up. Normal.
  strained     you can produce it and it will read poorly, and the budget is why.
  impossible   no way of spending this budget yields a usable asset.

Fields you may propose, with their current values and the range you may ask for:
)";
    out += proposable_fields_text(profile);
    out += R"(
Rules for this block:
  - Leave "propose" empty when the brief is fine. "tight" with nothing proposed
    is a perfectly good answer, and the most common correct one.
  - One field per entry, a number in "value", no ranges and no prose in there.
  - Never propose a change to buy yourself an easier job. Propose it when the
    asset is worse than the target hardware could carry.
  - "reason" names what breaks: which part, seen from where, at what distance.
    "more triangles would look better" is always true and therefore useless.
  - Proposing nothing is not modesty, and proposing everything is not thorough.
)";
    return out;
}

ProfileAdvice parse_advice_response(const LlmResponse& res, const TargetProfile& profile)
{
    if (!res.ok || !res.json_ok) return {};
    return parse_profile_advice(res.json, profile);
}

// ---------------------------------------------------------------------------
LlmRequest build_naming_request(const Mesh& mesh, const Segmentation& seg,
                                const TargetProfile& profile, const ViewSet& region_views,
                                const ViewSet& shaded_views)
{
    LlmRequest req;
    req.label  = "name_regions";
    req.system = director_system_prompt();

    std::string u;
    u += "STEP 1 OF 3 - NAME THE REGIONS\n\n";
    u += "An automatic segmenter has split the high poly into patches. It followed "
         "curvature, hard creases and bone boundaries, so the split is geometrically "
         "sound but anatomically dumb: it does not know that three of these patches "
         "are one hand.\n\n";
    u += "You are looking at two sets of renders of the SAME model from the same "
         "cameras. One set is shaded so you can read the form. The other paints each "
         "patch in a flat colour with its id. Match them up.\n\n";
    u += profile_summary_text(profile);
    u += "\nREGION TABLE\n";
    u += region_table_text(seg, mesh);
    u += "\nshape 'tube N:1' is measured, not guessed: the region is the tip of a long thin\n"
         "tube N times as long as it is wide - a tail, an antenna, a horn, a whip. The\n"
         "tube runs on from the tip into its neighbours, so the tip region itself can\n"
         "be small. Name it for the tube it ends; it is not an ear, a nose or a neck.\n";

    if (!mesh.armature.empty()) {
        u += "\nARMATURE (joint names, which usually tell you what a region is):\n";
        for (size_t i = 0; i < mesh.armature.size() && i < 64; ++i)
            u += format("  %2zu %s\n", i, mesh.armature.joints[i].name.c_str());
    }

    u += R"(
Produce this JSON:

{
  "regions": [
    { "id": 0, "name": "short_lowercase_name", "role": "what this part does for the read of the model" }
  ],
  "merge": [
    { "name": "left_hand", "role": "grips equipment, seen at mid distance", "members": [4, 9, 17] }
  ],
  "notes": "anything you noticed that the next step should know"
}

Rules for this step:
  - Name every id in the table. Use short, lowercase, underscore separated names.
  - Put a group in "merge" only when the patches genuinely belong to one part.
    Merging is destructive: the merged region gets one budget for all of it.
  - Do not merge a part that deforms differently from its neighbour. A forearm and
    a hand are separate even though they touch.
  - Do not merge across the symmetry plane. Left and right stay separate.
  - If a patch is a segmentation artefact (a stray sliver), merge it into the
    neighbour it belongs to rather than naming it.
)";

    req.user = u;
    attach(req, shaded_views, "shaded", 6);
    attach(req, region_views, "regions", 6);
    return req;
}

RegionNaming parse_naming_response(const LlmResponse& res, const Segmentation& seg)
{
    RegionNaming out;
    if (!res.ok) { out.error = res.error; return out; }
    if (!res.json_ok || !res.json.is_object()) {
        out.error = "the reply was not a JSON object";
        return out;
    }

    for (const Json& e : json_array_or_empty(res.json, "regions")) {
        if (!e.is_object()) continue;
        RegionNaming::Named n;
        n.id   = uint16_t(json_get<int>(e, "id", -1));
        n.name = trim(json_get<std::string>(e, "name", ""));
        n.role = trim(json_get<std::string>(e, "role", ""));
        if (n.name.empty() || !seg.find(n.id)) continue;
        n.name = slugify(n.name);
        out.named.push_back(std::move(n));
    }

    for (const Json& e : json_array_or_empty(res.json, "merge")) {
        if (!e.is_object()) continue;
        MergeGroup g;
        g.name = slugify(trim(json_get<std::string>(e, "name", "")));
        g.role = trim(json_get<std::string>(e, "role", ""));
        for (const Json& m : json_array_or_empty(e, "members")) {
            if (!m.is_number_integer()) continue;
            const uint16_t id = uint16_t(m.get<int>());
            if (seg.find(id)) g.members.push_back(id);
        }
        if (g.members.size() >= 2) out.merges.push_back(std::move(g));
    }

    out.notes = trim(json_get<std::string>(res.json, "notes", ""));
    out.ok    = !out.named.empty() || !out.merges.empty();
    if (!out.ok) out.error = "the reply named no regions";
    return out;
}

// ---------------------------------------------------------------------------
LlmRequest build_budget_request(const Mesh& mesh, const MeshAnalysis& analysis,
                                const Segmentation& seg, const TargetProfile& profile,
                                const KnobPanel& current, const ViewSet& shaded_views,
                                const ViewSet& region_views)
{
    LlmRequest req;
    req.label  = "allocate_budget";
    req.system = director_system_prompt();

    std::string u;
    u += "STEP 2 OF 3 - SPEND THE BUDGET\n\n";
    u += format("You have %d triangles for the whole asset. Decide how to spend them.\n\n",
                profile.max_triangles);
    u += profile_summary_text(profile);

    u += "\nWHAT THE MEASUREMENTS MEAN\n";
    u += "  area%       share of total surface area. A fair starting point, nothing more.\n";
    u += "  curv        mean curvature, 0 = flat plate, 1 = covered in detail.\n";
    u += "  ambient     how exposed the surface is. Low means it sits in a crevice\n";
    u += "              nobody will ever see.\n";
    u += "  visibility  how much of the rendered frame this region occupies across\n";
    u += "              the profile cameras, primary cameras weighted double.\n";
    u += "  shape       'tube N:1': the tip of a thin tube N times as long as it is wide\n";
    u += "              (a tail). It is built as a prism of three to six sides with rings\n";
    u += "              along it; roughly six triangles a ring, so budget it by the rings\n";
    u += "              you want to see along it, not by its tiny area.\n";
    u += "  joint       the bone that drives this region, when there is a skeleton.\n";

    u += "\nREGION TABLE\n";
    u += region_table_text(seg, mesh);

    u += format("\nThe high poly has %zu triangles and %s symmetry across %s "
                "(score %.2f).\n",
                mesh.triangle_count(),
                analysis.symmetry.accepted ? "usable" : "no usable",
                analysis.symmetry.axis_name(), analysis.symmetry.score);

    u += format("It is built from %zu connected piece%s and has %zu open boundary "
                "edge%s, so it is %s. The profile allows %s piece%s to survive.\n",
                analysis.stats.shells, analysis.stats.shells == 1 ? "" : "s",
                analysis.stats.boundary_edges, analysis.stats.boundary_edges == 1 ? "" : "s",
                analysis.stats.closed ? "watertight"
                                      : "not watertight, which is normal for an asset "
                                        "assembled from separate costume and prop pieces",
                profile.max_shells > 0 ? std::to_string(profile.max_shells).c_str()
                                       : "any number of",
                profile.max_shells == 1 ? "" : "s");
    // The pieces the low poly will actually have. Eyeballs, brows and straps
    // lying on the skin are dropped by the hard rules and painted by the bake,
    // and until the director was told so it read "7 pieces" as an assembled
    // costume and forced the quadric onto a clean body.
    {
        const HardRuleOptions rules;
        size_t kept = 0, dropped = 0;
        for (uint32_t s = 0; s < analysis.shell_area_share.size(); ++s) {
            if (source_shell_is_droppable(analysis, s, rules)) ++dropped;
            else ++kept;
        }
        if (dropped > 0)
            u += format("Of those pieces, %zu are small and either hidden (eyeballs behind "
                        "lids) or lying flat on the surface (brows, straps, patches): the "
                        "engine drops them from the low poly and the bake paints them, so the "
                        "surface to retopologise is %zu piece%s.\n",
                        dropped, kept, kept == 1 ? "" : "s");
    }
    if (profile.require_symmetry && analysis.symmetry.accepted)
        u += "Symmetry will be enforced, so half the budget effectively covers both "
             "sides. Do not try to spend differently on left and right.\n";

    u += "\n";
    u += budget_pressure_text(budget_pressure(mesh.triangle_count(), int(seg.ids().size()),
                                              profile,
                                              profile.require_symmetry &&
                                                  analysis.symmetry.accepted));

    u += "\nCURRENT PANEL (defaults, area proportional)\n";
    u += knob_panel_summary_text(current, seg);

    u += "\nSCHEMA\n";
    u += KnobPanel::json_schema_text();
    u += profile_advice_prompt_text(profile);

    u += R"(
Rules for this step:
  - Every region in the table needs an entry. Missing regions keep their defaults.
  - "share" is relative. If the head gets 4.0 and a boot gets 0.5, the head gets
    eight times the boot's triangles.
  - Area is a starting point, not an answer. A large flat back deserves less than
    its area share; a small face deserves far more.
  - "fidelity": "geometry" means the shape must exist in the silhouette;
    "texture" means flatten it and let the bake carry the detail. On this
    hardware the bake is genuinely good, so "texture" is not a defeat.
  - Set "preserve_silhouette" false only where the outline genuinely does not
    matter, for example a surface that is always against the body.
  - Pick "backend". The axis is not organic versus hard surface, whatever that
    would mean for a hooded archer. It is whether the source is one closed
    surface or an assembly of pieces:

      "quad_field"  walks an isotropic field over the surface and pairs the
                    result into quads. It needs a single closed watertight
                    shell to walk over. Given one, it beats quadric on evenness
                    and deforms better. Given an assembly with open borders it
                    starves: regions come back with a fraction of the triangles
                    they were allocated, and the budget goes unspent.
      "quadric"     collapses edges by quadric error, per region, and does not
                    care how many pieces there are or whether they are closed.
                    Slightly less even topology on a clean shell.

    Measured across four sources, quad_field won only on the one that was a
    single closed shell; on a costumed character, a creature and a building it
    lost, twice by enough to fail validation. Use the topology lines above rather
    than the word "organic": count the pieces that will survive, not the ones
    that will be dropped, and a few open edges round an eye socket do not make
    a body an assembly. "auto" applies exactly that rule, so leave it on "auto"
    unless you can see something in the renders it cannot.
  - Write one sentence of "rationale" per region. It goes in the report a human
    will read when they wonder why the elbow looks like that.
  - Allocate the budget you were given, in full, whatever you think of it. If you
    also think it is wrong, say so in "profile_advice" and allocate anyway: this
    run produces the best asset the current brief allows, and a human decides
    afterwards whether to change the brief and run it again.
)";

    req.user = u;
    attach(req, shaded_views, "high poly", 8);
    attach(req, region_views, "regions", 4);
    return req;
}

// ---------------------------------------------------------------------------
LlmRequest build_review_request(const Mesh& source, const Segmentation& seg,
                                const TargetProfile& profile, const KnobPanel& panel,
                                const IterationFacts& facts, const ViewSet& reference_views,
                                const ViewSet& candidate_views)
{
    LlmRequest req;
    req.label  = "review";
    req.system = director_system_prompt();

    std::string u;
    u += "STEP 3 OF 3 - REVIEW AND CORRECT\n\n";
    u += "The engine built a low poly from your panel. You are looking at the high "
         "poly and the low poly rendered from the same cameras, in that order. "
         "Compare them the way you would compare a sculpt to its game model: does "
         "it read as the same character at the distance the profile says it will "
         "be seen from?\n\n";
    u += profile_summary_text(profile);
    u += "\nMEASUREMENTS\n";
    u += metrics_text(facts);

    if (facts.validation) {
        u += "\nVALIDATION\n";
        if (facts.validation->passed) {
            u += "All hard checks passed.\n";
            if (facts.validation->warnings > 0) {
                u += "Warnings:\n";
                u += facts.validation->failure_text();
            }
        } else {
            u += "The asset FAILED validation:\n";
            u += facts.validation->failure_text();
        }
    }

    if (facts.retopo) {
        const auto misses = facts.retopo->budget_misses(seg, 0.25f);
        if (!misses.empty()) {
            u += "\nREGIONS THAT MISSED THEIR BUDGET\n";
            for (const auto& m : misses)
                u += format("  %-14s allocated %4d, got %4d (%+d)\n", m.name.c_str(),
                            m.budget, m.actual, m.actual - m.budget);
            u += "A region that undershoots is usually one where the geometry ran out "
                 "of detail to keep; move those triangles somewhere they show.\n";
        }
    }

    u += "\nCURRENT PANEL\n";
    u += knob_panel_summary_text(panel, seg);
    (void)source;

    u += format(R"(
Produce this JSON:

{
  "verdict": "accept" or "revise",
  "critique": "two or three sentences on what is wrong and what is fine",
  "patch": {
    "global":  { "only the keys you want to change": 0 },
    "regions": [ { "id": 3, "share": 2.5, "rationale": "why" } ]
  },
  "another_pass": true or false,
  "profile_advice": { ... only when the brief, not the panel, is the problem ... }
}

Rules for this step:
  - "patch" is a partial update. Leave out anything you are happy with. Do not
    restate the whole panel.
  - You are on iteration %d of at most %d. If the result is good enough for the
    distance it will be seen from, say "accept" and stop. Chasing the last
    percent of silhouette error costs a human their afternoon.
  - If validation failed, fixing that comes first; taste is secondary.
  - If regions came back far under the budget they were allocated and the total
    is well short of the profile, look at the backend before the shares. A
    starving quad_field on an assembled source produces exactly that pattern,
    and "backend": "quadric" in the global patch is one key rather than twenty.
    If the total is close to the budget, it is not starving: quad_field treats
    region budgets as a density, not a quota, and moves triangles between
    neighbours. Switching backend then trades even, clean topology for the
    fans and slivers the quadric leaves on limbs and hands.
  - Small pieces that are mostly hidden or lie flat on the surface - eyeballs,
    brows, straps - are removed from the low poly on purpose and painted by the
    bake (a "hard rule" line says so). Judge how they read in the texture; do
    not treat their missing geometry as a failure or spend shares on them.
  - Dark specks and blotches inside the silhouette are texture, not holes,
    unless the validation reports open boundary edges.
  - "patch" cannot raise the total. The budget is fixed for this run: inside it
    you move triangles, you do not add them. If moving them is no longer enough
    and the renders show you why, that belongs in "profile_advice", where a human
    will read it. You have something now that you did not have at step 2. You
    have seen the result.
)", facts.iteration, facts.max_iterations);

    u += profile_advice_prompt_text(profile);

    req.user = u;
    attach(req, reference_views, "HIGH POLY", 6);
    attach(req, candidate_views, "LOW POLY", 6);
    return req;
}

ReviewOutcome parse_review_response(const LlmResponse& res)
{
    ReviewOutcome out;
    if (!res.ok) { out.error = res.error; return out; }
    if (!res.json_ok || !res.json.is_object()) {
        out.error = "the reply was not a JSON object";
        return out;
    }

    out.verdict  = to_lower(trim(json_get<std::string>(res.json, "verdict", "revise")));
    out.critique = trim(json_get<std::string>(res.json, "critique", ""));
    out.wants_another_pass = json_get<bool>(res.json, "another_pass", out.verdict != "accept");

    const auto it = res.json.find("patch");
    if (it != res.json.end() && it->is_object()) out.patch = *it;
    else                                          out.patch = Json::object();

    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
LlmRequest build_repair_request(const Segmentation& seg, const TargetProfile& profile,
                                const KnobPanel& panel, const ValidationReport& report,
                                const RetopoResult& retopo)
{
    LlmRequest req;
    req.label  = "repair";
    req.system = director_system_prompt();

    std::string u;
    u += "VALIDATION FAILED - FIX THE PANEL\n\n";
    u += "The asset did not pass the hard checks, so there is nothing worth looking "
         "at yet. No renders this round: fix the numbers first.\n\n";
    u += profile_summary_text(profile);

    u += "\nFAILURES\n";
    u += report.failure_text();

    const auto misses = retopo.budget_misses(seg, 0.15f);
    if (!misses.empty()) {
        u += "\nREGION BUDGETS\n";
        for (const auto& m : misses)
            u += format("  %-14s allocated %4d, got %4d (%+d)\n", m.name.c_str(),
                        m.budget, m.actual, m.actual - m.budget);
    }

    u += "\nCURRENT PANEL\n";
    u += knob_panel_summary_text(panel, seg);

    u += R"(
Produce this JSON:

{
  "verdict": "revise",
  "critique": "what you think went wrong",
  "patch": { "global": {}, "regions": [] },
  "another_pass": true
}

Practical fixes, in rough order of how often they work:
  - Over the triangle budget: lower the shares of the regions that overshot, or
    raise "merge_aggressiveness".
  - Strips too short: raise "quad_dominance" and "smoothing_iterations"; long
    strips need regular topology.
  - Too many shells: something detached. Raise "merge_aggressiveness" and set
    "preserve_boundary" false on the region that fragmented.
  - Symmetry failed: make sure "enforce_symmetry" is true and raise
    "symmetry_lock" on the regions that drifted.
  - Atlas needs more pages: lower "uv_padding_texels", or move budget away from
    regions with large surface area that do not need the resolution.

If you have been round this loop before and the same check keeps failing, the
panel is probably not what is wrong. Say so in "profile_advice" rather than
moving the same triangles a fourth time.
)";
    u += profile_advice_prompt_text(profile);

    req.user = u;
    return req;
}

} // namespace rd
