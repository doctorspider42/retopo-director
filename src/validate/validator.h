#pragma once

// The gate between the geometry engine and the director.
//
// If a hard check fails, the model is not shown renders and asked for an
// opinion; it is handed a specific, numeric complaint ("region head is 340
// triangles over budget") and asked to fix the knobs. Taste only gets a vote
// once the asset is legal.

#include "bake/bake.h"
#include "export/exporter.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "segment/segment.h"

#include <string>
#include <vector>

namespace rd {

enum class Severity : uint8_t { Info = 0, Warning, Error };
const char* severity_name(Severity s);

struct Check {
    std::string id;        // stable identifier, e.g. "geometry.triangle_count"
    std::string title;
    std::string detail;    // human readable, also what the model is shown
    Severity    severity = Severity::Info;
    bool        passed    = true;
    double      value     = 0.0;
    double      limit     = 0.0;
    std::string scope;     // empty for global, otherwise the region name
};

struct ValidationReport {
    std::vector<Check> checks;
    int  errors   = 0;
    int  warnings = 0;
    bool passed   = false;    // no errors

    // Mirror score measured on the low poly itself, not inherited.
    float symmetry_score = 0.0f;

    void add(Check c);

    // Only the failures, formatted as a numbered list. This is exactly what the
    // director receives when the result is rejected.
    std::string failure_text() const;
    // Everything, for the UI and the written report.
    std::string full_text() const;
    Json        to_json() const;
};

struct ValidationInput {
    const Mesh*          mesh       = nullptr;   // the finished low poly
    const TargetProfile* profile    = nullptr;
    const Segmentation*  segmentation = nullptr; // optional
    const KnobPanel*     panel      = nullptr;   // optional, for per region budgets
    const StripData*     strips     = nullptr;   // optional
    const BakeResult*    bake       = nullptr;   // optional
    const MeshAnalysis*  source_analysis = nullptr; // optional, for symmetry reference
    // Region triangle counts from the retopo result, indexed by region id.
    const std::vector<int>* region_triangles = nullptr;
    // Tolerance before a region budget miss is reported, as a fraction.
    float budget_tolerance = 0.20f;
};

ValidationReport validate(const ValidationInput& in);

} // namespace rd
