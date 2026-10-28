// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan upscalers' library loading (`upscale_vk.h`): the viewer's own
// directory, and LoadLibrary / dlopen from there and nowhere else. The SDKs'
// libraries are never taken from the search path.
#include "upscale_vk.h"

#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  include <climits>
#endif

namespace omk::vkup {

std::string moduleDir() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    const auto cut = s.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : s.substr(0, cut + 1);
#else
    char path[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return std::string();
    std::string s(path, static_cast<std::size_t>(n));
    const auto cut = s.find_last_of('/');
    return cut == std::string::npos ? std::string() : s.substr(0, cut + 1);
#endif
}

void* openLibrary(const std::string& file) {
    const std::string full = moduleDir() + file;
#ifdef _WIN32
    // its OWN directory first, for the libraries it loads beside it
    return reinterpret_cast<void*>(LoadLibraryExA(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
#else
    return dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* lib, const char* name) {
    if (!lib) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void closeLibrary(void* lib) {
    if (!lib) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

}  // namespace omk::vkup
