#include "render/gl.h"

#include "core/log.h"
#include "core/util.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <string>

#define RD_GL_DEFINE(ret, name, params) ret (APIENTRY* rd_##name) params = nullptr;
RD_GL_FUNCTIONS(RD_GL_DEFINE)
#undef RD_GL_DEFINE

namespace rd::gl {
namespace {

bool        g_loaded = false;
std::string g_error;

template <typename Fn>
bool fetch(Fn& slot, const char* name)
{
    slot = reinterpret_cast<Fn>(glfwGetProcAddress(name));
    if (!slot) {
        if (g_error.empty()) g_error = std::string("missing entry point: ") + name;
        return false;
    }
    return true;
}

} // namespace

bool load()
{
    if (g_loaded) return true;
    g_error.clear();

    bool ok = true;
#define RD_GL_FETCH(ret, name, params) ok = fetch(rd_##name, #name) && ok;
    RD_GL_FUNCTIONS(RD_GL_FETCH)
#undef RD_GL_FETCH

    g_loaded = ok;
    if (ok) {
        const char* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        RD_INFO("opengl %s on %s", version ? version : "?", renderer ? renderer : "?");
    } else {
        RD_ERROR("opengl loader failed: %s", g_error.c_str());
    }
    return ok;
}

bool        loaded()          { return g_loaded; }
const char* last_load_error() { return g_error.c_str(); }

void check_error(const char* where)
{
    GLenum err;
    int    guard = 0;
    while ((err = glGetError()) != GL_NO_ERROR && guard++ < 8) {
        const char* text = "unknown";
        switch (err) {
        case GL_INVALID_ENUM:      text = "invalid enum"; break;
        case GL_INVALID_VALUE:     text = "invalid value"; break;
        case GL_INVALID_OPERATION: text = "invalid operation"; break;
        case GL_OUT_OF_MEMORY:     text = "out of memory"; break;
        case GL_STACK_OVERFLOW:    text = "stack overflow"; break;
        case GL_STACK_UNDERFLOW:   text = "stack underflow"; break;
        default: break;
        }
        RD_WARN("gl error at %s: %s (0x%04x)", where, text, unsigned(err));
    }
}

} // namespace rd::gl
