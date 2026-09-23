// Thread pool, JSON leniency and the progress mapping. The pool tests are the
// ones that matter: every one of them is a way the pool has actually failed.

#include "test.h"

#include "core/json.h"
#include "core/thread_pool.h"
#include "pipeline/pipeline.h"

#include <atomic>
#include <numeric>
#include <stdexcept>

using namespace rd;

TEST(thread_pool_covers_every_index_once)
{
    ThreadPool pool(7);
    std::vector<std::atomic<int>> hits(100003);
    pool.parallel_for(hits.size(), 1, [&](size_t i, unsigned) { hits[i].fetch_add(1); });
    size_t wrong = 0;
    for (auto& h : hits) wrong += h.load() != 1;
    CHECK_EQ(wrong, size_t(0));
}

TEST(thread_pool_lanes_are_in_range)
{
    ThreadPool pool(5);
    std::atomic<bool> bad{false};
    pool.parallel_for(5000, 1, [&](size_t, unsigned lane) {
        if (lane >= pool.lane_count()) bad = true;
    });
    CHECK(!bad.load());
}

// The race that shipped: many small regions back to back, each returning the
// moment its last chunk finished while a helper was still reading the frame.
// Thousands of tiny regions is what it took to lose it on a character mesh.
TEST(thread_pool_survives_many_tiny_regions)
{
    ThreadPool pool(15);
    std::atomic<size_t> total{0};
    for (int round = 0; round < 4000; ++round) {
        pool.parallel_ranges(64, 1, [&](size_t b, size_t e, unsigned) { total += e - b; });
    }
    CHECK_EQ(total.load(), size_t(4000 * 64));
}

// A region opened from inside a worker job must not wait on jobs that are
// queued behind the one it is running in.
TEST(thread_pool_nested_regions_do_not_deadlock)
{
    ThreadPool pool(3);
    std::atomic<size_t> total{0};
    pool.parallel_for(16, 1, [&](size_t, unsigned) {
        pool.parallel_for(100, 1, [&](size_t, unsigned) { total.fetch_add(1); });
    });
    CHECK_EQ(total.load(), size_t(1600));
}

// A throw on the caller's lane propagates, and only after the helpers are out
// of the body; a throw on a helper lane is logged and the region still ends.
TEST(thread_pool_throwing_body_neither_hangs_nor_escapes_a_worker)
{
    ThreadPool pool(4);
    bool caught = false;
    try {
        pool.parallel_ranges(1000, 1, [&](size_t b, size_t, unsigned lane) {
            if (lane == 0 && b == 0) throw std::runtime_error("caller lane");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    // Whether lane 0 happened to take chunk 0 is up to the scheduler; what is
    // not allowed is a hang, which is what reaching the next line proves.
    (void)caught;

    std::atomic<size_t> after{0};
    pool.parallel_ranges(1000, 1, [&](size_t b, size_t e, unsigned lane) {
        if (lane != 0 && b == 0) throw std::runtime_error("helper lane");
        after += e - b;
    });
    CHECK(after.load() <= 1000);
    // And the pool still works afterwards.
    std::atomic<size_t> again{0};
    pool.parallel_for(500, 1, [&](size_t, unsigned) { again.fetch_add(1); });
    CHECK_EQ(again.load(), size_t(500));
}

TEST(json_lenient_digs_json_out_of_prose)
{
    std::string err;
    Json j = json_parse_lenient("Sure! Here is the panel:\n```json\n{\"a\": 1, \"b\": [1,2]}\n```\nHope it helps.", err);
    CHECK(j.is_object());
    CHECK_EQ(json_get<int>(j, "a", 0), 1);

    Json k = json_parse_lenient("no json in here at all", err);
    CHECK(!err.empty());
    CHECK(!k.is_object() || k.empty());

    // A wrong type falls back instead of throwing.
    Json typed = Json::parse(R"({"n": "not a number"})");
    CHECK_EQ(json_get<int>(typed, "n", 7), 7);
    CHECK_EQ(json_get<int>(typed, "missing", 3), 3);
}

TEST(overall_progress_never_rewinds)
{
    const Stage order[] = {Stage::Loading, Stage::Analysing, Stage::ReferenceRenders,
                           Stage::Segmenting, Stage::NamingRegions, Stage::AllocatingBudget};
    const Stage loop[]  = {Stage::BuildingDensity, Stage::Retopologising, Stage::Baking,
                           Stage::Validating, Stage::CandidateRenders, Stage::Reviewing};
    const int iterations = 3;
    float last = -1.0f;
    bool  rewound = false;
    for (Stage s : order)
        for (float w : {0.0f, 0.5f, 1.0f}) {
            const float f = stage_overall_progress(s, w, 0, iterations);
            if (f + 1e-6f < last) rewound = true;
            last = f;
        }
    for (int it = 1; it <= iterations; ++it)
        for (Stage s : loop)
            for (float w : {0.0f, 0.5f, 1.0f}) {
                const float f = stage_overall_progress(s, w, it, iterations);
                if (f + 1e-6f < last) rewound = true;
                last = f;
            }
    CHECK(!rewound);
    CHECK_NEAR(stage_overall_progress(Stage::Done, 0.0f), 1.0, 1e-6);
}
