#pragma once

// A test harness small enough to read in one go. TEST registers a function,
// CHECK records a failure and carries on, REQUIRE records one and returns.
// No exceptions, no dependencies: the engine path does not throw, and a test
// framework that needs them would be the only thing in the build that does.

#include "core/math.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace rdtest {

struct Case {
    const char*           name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
void                fail(const char* file, int line, const std::string& what);

template <typename T>
std::string to_text(const T& v)
{
    if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);
    else return std::string(v);
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

// --- fixtures -----------------------------------------------------------------
// Unit icosphere subdivided `levels` times: 20 * 4^levels triangles, closed,
// outward facing, one shell. The shape every stage has to get right.
rd::Mesh icosphere(int levels, float radius = 1.0f);
// Genus one torus in the XZ plane, outward facing.
rd::Mesh torus(int nu, int nv, float major = 1.0f, float minor = 0.35f);
// Signed volume; positive for a closed shell that faces outward.
double signed_volume(const rd::Mesh& m);

} // namespace rdtest

#define RD_CAT2(a, b) a##b
#define RD_CAT(a, b)  RD_CAT2(a, b)

#define TEST(name)                                                              \
    static void RD_CAT(test_fn_, name)();                                       \
    static ::rdtest::Registrar RD_CAT(test_reg_, name)(#name, &RD_CAT(test_fn_, name)); \
    static void RD_CAT(test_fn_, name)()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) ::rdtest::fail(__FILE__, __LINE__, #cond);                 \
    } while (0)

#define REQUIRE(cond)                                                           \
    do {                                                                        \
        if (!(cond)) { ::rdtest::fail(__FILE__, __LINE__, #cond); return; }     \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        const auto va_ = (a);                                                   \
        const auto vb_ = (b);                                                   \
        if (!(va_ == vb_))                                                      \
            ::rdtest::fail(__FILE__, __LINE__,                                  \
                           std::string(#a " == " #b "  (") + ::rdtest::to_text(va_) + \
                               " vs " + ::rdtest::to_text(vb_) + ")");             \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                   \
    do {                                                                        \
        const double va_ = double(a), vb_ = double(b);                          \
        if (!(std::fabs(va_ - vb_) <= double(tol)))                             \
            ::rdtest::fail(__FILE__, __LINE__,                                  \
                           std::string(#a " ~ " #b "  (") + ::rdtest::to_text(va_) + \
                               " vs " + ::rdtest::to_text(vb_) + ")");             \
    } while (0)
