#include "knobs/advice.h"

#include "core/util.h"

#include <algorithm>
#include <cmath>

namespace rd {
namespace {

// Reading and writing a profile field by name. Two small switches beat a
// reflection layer for nine fields, and they keep the whitelist and the
// accessors impossible to get out of step: a name that is not here cannot be
// read, so it cannot be proposed either.
bool read_field(const TargetProfile& p, const std::string& name, double& out)
{
    if      (name == "max_triangles")           out = p.max_triangles;
    else if (name == "max_vertices")            out = p.max_vertices;
    else if (name == "max_bone_influences")     out = p.max_bone_influences;
    else if (name == "max_shells")              out = p.max_shells;
    else if (name == "texture.width")           out = p.texture.width;
    else if (name == "texture.height")          out = p.texture.height;
    else if (name == "texture.count")           out = p.texture.count;
    else if (name == "texture.palette_colors")  out = p.texture.palette_colors;
    else if (name == "target_silhouette_error") out = p.target_silhouette_error;
    else if (name == "max_silhouette_error")    out = p.max_silhouette_error;
    else if (name == "max_iterations")          out = p.max_iterations;
    else return false;
    return true;
}

bool write_field(TargetProfile& p, const std::string& name, double v)
{
    const int i = int(std::lround(v));
    if      (name == "max_triangles")           p.max_triangles = i;
    else if (name == "max_vertices")            p.max_vertices = i;
    else if (name == "max_bone_influences")     p.max_bone_influences = i;
    else if (name == "max_shells")              p.max_shells = i;
    else if (name == "texture.width")           p.texture.width = i;
    else if (name == "texture.height")          p.texture.height = i;
    else if (name == "texture.count")           p.texture.count = i;
    else if (name == "texture.palette_colors")  p.texture.palette_colors = i;
    else if (name == "target_silhouette_error") p.target_silhouette_error = float(v);
    else if (name == "max_silhouette_error")    p.max_silhouette_error = float(v);
    else if (name == "max_iterations")          p.max_iterations = i;
    else return false;
    return true;
}

const ProposableField* find_field(const std::string& name)
{
    for (const ProposableField& f : proposable_profile_fields())
        if (name == f.name) return &f;
    return nullptr;
}

// Models write "triangles", "max triangles" and "profile.max_triangles" for the
// same thing. Normalising costs four lines and saves a proposal that would
// otherwise be shown to a human as unapplicable for no reason.
std::string canonical_field(std::string s)
{
    s = to_lower(trim(s));
    std::replace(s.begin(), s.end(), ' ', '_');
    std::replace(s.begin(), s.end(), '-', '_');
    if (s.rfind("profile.", 0) == 0) s = s.substr(8);
    if (s == "triangles" || s == "triangle_budget") s = "max_triangles";
    if (s == "vertices")                            s = "max_vertices";
    if (s == "texture_width"  || s == "texture.w")  s = "texture.width";
    if (s == "texture_height" || s == "texture.h")  s = "texture.height";
    if (s == "texture_pages"  || s == "pages")      s = "texture.count";
    if (s == "palette_colors" || s == "palette")    s = "texture.palette_colors";
    if (s == "bone_influences")                     s = "max_bone_influences";
    if (s == "shells" || s == "max_pieces")         s = "max_shells";
    return s;
}

std::string number_text(double v, bool integral)
{
    return integral ? format("%d", int(std::lround(v))) : format("%.3f", v);
}

} // namespace

// ---------------------------------------------------------------------------
const char* feasibility_name(Feasibility f)
{
    switch (f) {
        case Feasibility::Comfortable: return "comfortable";
        case Feasibility::Tight:       return "tight";
        case Feasibility::Strained:    return "strained";
        case Feasibility::Impossible:  return "impossible";
        default:                       return "unknown";
    }
}

Feasibility feasibility_from_name(std::string_view s, Feasibility fallback)
{
    const std::string t = to_lower(trim(std::string(s)));
    if (t == "comfortable") return Feasibility::Comfortable;
    if (t == "tight")       return Feasibility::Tight;
    if (t == "strained")    return Feasibility::Strained;
    if (t == "impossible")  return Feasibility::Impossible;
    return fallback;
}

bool feasibility_is_alarming(Feasibility f)
{
    return f == Feasibility::Strained || f == Feasibility::Impossible;
}

std::string ProfileProposal::change_text() const
{
    const ProposableField* f = find_field(field);
    const bool integral = f ? f->integral : true;
    return number_text(current, integral) + " -> " + number_text(proposed, integral);
}

int ProfileAdvice::applicable_count() const
{
    int n = 0;
    for (const ProfileProposal& p : proposals) n += p.applicable ? 1 : 0;
    return n;
}

Json ProfileAdvice::to_json() const
{
    Json j;
    j["feasibility"] = feasibility_name(feasibility);
    j["headline"]    = headline;
    Json arr = Json::array();
    for (const ProfileProposal& p : proposals) {
        Json e;
        e["field"]      = p.field;
        e["current"]    = p.current;
        e["proposed"]   = p.proposed;
        e["reason"]     = p.reason;
        e["applicable"] = p.applicable;
        if (!p.note.empty()) e["note"] = p.note;
        arr.push_back(std::move(e));
    }
    j["proposals"] = std::move(arr);
    return j;
}

// ---------------------------------------------------------------------------
const std::vector<ProposableField>& proposable_profile_fields()
{
    // Ranges are the profile's own clamps, narrowed where a value the profile
    // tolerates would still be absurd coming from a director: it may argue for
    // a bigger budget, not for a budget that stops being a console target.
    static const std::vector<ProposableField> fields = {
        {"max_triangles", "total triangle budget for the asset", 32, 20000, true},
        {"max_vertices", "total vertex budget", 32, 20000, true},
        {"max_bone_influences", "skin weights per vertex", 1, 4, true},
        // Not a taste knob: a source built from separate pieces meets a limit of
        // one by having every piece but the largest deleted, so this is often
        // the only proposal that matters.
        {"max_shells", "connected pieces the asset may keep", 1, 64, true},
        {"texture.width", "atlas width in pixels", 32, 1024, true},
        {"texture.height", "atlas height in pixels", 32, 1024, true},
        {"texture.count", "how many atlas pages the target allows", 1, 4, true},
        {"texture.palette_colors", "CLUT size, 0 for truecolor", 0, 256, true},
        {"target_silhouette_error", "soft quality target", 0.002, 0.2, false},
        {"max_silhouette_error", "hard quality limit", 0.002, 0.3, false},
        {"max_iterations", "how many review passes the run may take", 1, 8, true},
    };
    return fields;
}

std::string proposable_fields_text(const TargetProfile& profile)
{
    std::string out;
    for (const ProposableField& f : proposable_profile_fields()) {
        double cur = 0.0;
        read_field(profile, f.name, cur);
        out += format("  %-24s now %-8s allowed %s..%s   %s\n", f.name,
                      number_text(cur, f.integral).c_str(),
                      number_text(f.min, f.integral).c_str(),
                      number_text(f.max, f.integral).c_str(), f.description);
    }
    return out;
}

// ---------------------------------------------------------------------------
ProfileAdvice parse_profile_advice(const Json& j, const TargetProfile& against)
{
    ProfileAdvice out;
    if (!j.is_object()) return out;

    const Json& node = json_object_or_empty(j, "profile_advice");
    if (node.empty()) return out;

    out.feasibility = feasibility_from_name(json_get<std::string>(node, "feasibility", ""),
                                            Feasibility::Unknown);
    out.headline    = trim(json_get<std::string>(node, "headline", ""));

    for (const Json& e : json_array_or_empty(node, "propose")) {
        if (!e.is_object()) continue;
        ProfileProposal p;
        p.field  = canonical_field(json_get<std::string>(e, "field", ""));
        p.reason = trim(json_get<std::string>(e, "reason", ""));
        if (p.field.empty()) continue;

        // A model that answers with a string ("2400") is being helpful in the
        // wrong type, not being wrong.
        const auto it = e.find("value");
        double value = 0.0;
        bool   have  = false;
        if (it != e.end()) {
            if (it->is_number()) { value = it->get<double>(); have = true; }
            else if (it->is_string()) {
                try { value = std::stod(it->get<std::string>()); have = true; }
                catch (const std::exception&) { have = false; }
            }
        }
        p.proposed = value;

        const ProposableField* f = find_field(p.field);
        if (!f) {
            p.note = "not a field the director may propose";
        } else if (!have || !std::isfinite(value)) {
            read_field(against, p.field, p.current);
            p.note = "no usable number was given";
        } else {
            read_field(against, p.field, p.current);
            const double clamped = std::clamp(value, f->min, f->max);
            if (clamped != value)
                p.note = format("clamped from %s to the allowed range",
                                number_text(value, f->integral).c_str());
            p.proposed   = f->integral ? std::round(clamped) : clamped;
            // A proposal that changes nothing is noise in a list a human reads.
            p.applicable = std::abs(p.proposed - p.current) > 1e-6;
            if (!p.applicable && p.note.empty()) p.note = "already the current value";
        }
        out.proposals.push_back(std::move(p));
    }
    return out;
}

bool apply_proposal(TargetProfile& profile, const ProfileProposal& p)
{
    if (!p.applicable) return false;
    if (!write_field(profile, p.field, p.proposed)) return false;
    profile.clamp();
    return true;
}

void merge_advice(ProfileAdvice& into, const ProfileAdvice& latest)
{
    if (latest.empty()) return;
    if (latest.feasibility != Feasibility::Unknown) into.feasibility = latest.feasibility;
    if (!latest.headline.empty()) into.headline = latest.headline;

    for (const ProfileProposal& p : latest.proposals) {
        auto it = std::find_if(into.proposals.begin(), into.proposals.end(),
                               [&](const ProfileProposal& e) { return e.field == p.field; });
        if (it != into.proposals.end()) *it = p;
        else                            into.proposals.push_back(p);
    }
}

std::string advice_text(const ProfileAdvice& advice)
{
    if (advice.empty()) return {};
    std::string out;
    out += format("feasibility: %s\n", feasibility_name(advice.feasibility));
    if (!advice.headline.empty()) out += advice.headline + "\n";
    for (const ProfileProposal& p : advice.proposals) {
        out += format("  %-24s %-20s %s\n", p.field.c_str(), p.change_text().c_str(),
                      p.reason.c_str());
        if (!p.note.empty()) out += format("  %-24s (%s)\n", "", p.note.c_str());
    }
    return out;
}

// ---------------------------------------------------------------------------
BudgetPressure budget_pressure(size_t source_triangles, int regions,
                               const TargetProfile& profile, bool symmetry_enforced)
{
    BudgetPressure p;
    p.budget           = profile.max_triangles;
    p.source_triangles = source_triangles;
    p.regions          = std::max(regions, 1);
    p.symmetry_halves  = symmetry_enforced;
    p.per_region       = float(p.budget) / float(p.regions);
    p.decimation       = source_triangles > 0
                             ? float(p.budget) / float(source_triangles) : 0.0f;

    // The thresholds are craft numbers, not measurements: below roughly 30
    // triangles a named part is a box with a bevel, and below 12 it is a box.
    // They exist to put a word next to the arithmetic, not to gate anything.
    if      (p.per_region < 12.0f) p.verdict = "below one box per named part";
    else if (p.per_region < 30.0f) p.verdict = "about one box per named part";
    else if (p.per_region < 80.0f) p.verdict = "enough for a simple shape per part";
    else                           p.verdict = "room to model per part";
    return p;
}

std::string budget_pressure_text(const BudgetPressure& p)
{
    std::string out;
    out += format("BUDGET PRESSURE (arithmetic, not opinion)\n");
    out += format("  %d triangles across %d named regions = %.1f per region (%s)\n",
                  p.budget, p.regions, p.per_region, p.verdict.c_str());
    if (p.source_triangles > 0)
        out += format("  the high poly has %zu triangles, so this keeps %.2f%% of them\n",
                      p.source_triangles, p.decimation * 100.0f);
    if (p.symmetry_halves)
        out += "  symmetry is enforced, so the budget is really spent on one half and "
               "mirrored\n";
    return out;
}

} // namespace rd
