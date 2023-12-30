#pragma once

#include "roxy/module.hpp"
#include "roxy/core/file.hpp"
#include "roxy/tsl/robin_map.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace rx {

class Library {
public:
    Library() : m_source_allocator(65536) {}

    bool compile_from_dir(std::string directory, std::string& message);

    bool compile_from_files(std::string directory, const std::vector<std::string>& files, std::string& message);

    Module* get_module(std::string_view path);

private:

    void load_builtin_functions();

    std::string m_directory;
    Vector<UniquePtr<Module>> m_modules;
    BumpAllocator m_source_allocator;
    tsl::robin_map<std::string_view, Module*> m_module_names;

    static UniquePtr<Module> s_builtin_module;
    static const char* s_builtin_module_src;
};

}