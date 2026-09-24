#include "test.h"

#include "core/log.h"

#include <cstdlib>
#include <cstring>
#include <map>

namespace rdtest {

namespace {
int         g_failures = 0;
const char* g_current  = "";
} // namespace

std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& what)
{
    ++g_failures;
    std::fprintf(stderr, "  FAIL %s  %s:%d  %s\n", g_current, file, line, what.c_str());
}

rd::Mesh icosphere(int levels, float radius)
{
    using rd::Vec3;
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    rd::Mesh m;
    m.positions = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                   {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    m.indices = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4,
                 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8,
                 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (Vec3& p : m.positions) p = rd::normalize(p);

    for (int l = 0; l < levels; ++l) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> mid;
        auto midpoint = [&](uint32_t a, uint32_t b) {
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            m.positions.push_back(rd::normalize((m.positions[a] + m.positions[b]) * 0.5f));
            const uint32_t idx = uint32_t(m.positions.size() - 1);
            mid.emplace(key, idx);
            return idx;
        };
        std::vector<uint32_t> out;
        for (size_t f = 0; f < m.indices.size(); f += 3) {
            const uint32_t a = m.indices[f], b = m.indices[f + 1], c = m.indices[f + 2];
            const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            out.insert(out.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        m.indices = std::move(out);
    }
    for (Vec3& p : m.positions) p = p * radius;
    return m;
}

rd::Mesh torus(int nu, int nv, float major, float minor)
{
    rd::Mesh m;
    const float two_pi = 6.28318530718f;
    for (int i = 0; i < nu; ++i) {
        const float u = two_pi * float(i) / float(nu);
        for (int j = 0; j < nv; ++j) {
            const float w = two_pi * float(j) / float(nv);
            m.positions.push_back({(major + minor * std::cos(w)) * std::cos(u), minor * std::sin(w),
                                   (major + minor * std::cos(w)) * std::sin(u)});
        }
    }
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            const uint32_t a = uint32_t(i * nv + j);
            const uint32_t b = uint32_t(((i + 1) % nu) * nv + j);
            const uint32_t c = uint32_t(((i + 1) % nu) * nv + (j + 1) % nv);
            const uint32_t d = uint32_t(i * nv + (j + 1) % nv);
            m.indices.insert(m.indices.end(), {a, d, c, a, c, b});
        }
    return m;
}

double signed_volume(const rd::Mesh& m)
{
    double v = 0.0;
    for (size_t t = 0; t < m.triangle_count(); ++t) {
        rd::Vec3 a, b, c;
        m.tri_positions(t, a, b, c);
        v += double(rd::dot(a, rd::cross(b, c)));
    }
    return v / 6.0;
}

} // namespace rdtest

int main(int argc, char** argv)
{
    rd::log::init();
    // The engine logs a lot; it is only worth reading when a test fails.
    if (std::getenv("RD_TEST_VERBOSE")) rd::log::set_echo_stderr(true);
    const char* filter = argc > 1 ? argv[1] : nullptr;

    int run = 0, failed_cases = 0;
    for (const rdtest::Case& c : rdtest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        rdtest::g_current = c.name;
        const int before  = rdtest::g_failures;
        c.fn();
        ++run;
        const bool ok = rdtest::g_failures == before;
        if (!ok) ++failed_cases;
        std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", c.name);
        std::fflush(stdout);
    }
    std::printf("\n%d of %d tests passed\n", run - failed_cases, run);
    rd::log::shutdown();
    return failed_cases == 0 && run > 0 ? 0 : 1;
}
