#include "roxy/library.hpp"

#include "roxy/compiler.hpp"
#include "roxy/vm.hpp"

namespace rx {

UniquePtr<Module> Library::s_builtin_module = nullptr;

bool Library::compile_from_dir(std::string directory, std::string& message) {
    std::vector<std::string> files;

    auto dir = fs::path(directory);
    for (const auto& file : fs::recursive_directory_iterator(dir)) {
        if (file.path().extension() != ".roxy") continue;
        files.push_back(file.path().string());
    }

    return compile_from_files(directory, files, message);
}

bool Library::compile_from_files(std::string directory, const std::vector<std::string>& files, std::string& message) {
    m_directory = directory;

    struct ModuleNode {
        Scanner scanner;
        Module* module;
        ModuleStmt* module_stmt;
    };

    Vector<ModuleNode> module_nodes;

    if (s_builtin_module.get() == nullptr) {
        s_builtin_module = UniquePtr<Module>(new Module("builtin", reinterpret_cast<const u8*>(s_builtin_module_src)));
    }

    module_nodes.push_back({Scanner(s_builtin_module->source()), s_builtin_module.get(), nullptr});

    for (const std::string& filename : files) {
        u8* buf;
        if (!read_file_to_buf(filename.c_str(), buf, m_source_allocator)) {
            fmt::print("Error while opening file {}!\n", filename);
            return false;
        }

        std::string relative_path = fs::relative(filename, directory).parent_path().string();
        std::string module_name;
        auto file_path = fs::path(filename);
        if (relative_path.empty()) {
            module_name = file_path.stem().string();
        }
        else {
            module_name = relative_path + '.' + file_path.stem().string();
        }
        for (u64 i = 0; i < module_name.size(); i++) {
            if (module_name[i] == '/' || module_name[i] == '\\')
                module_name[i] = '.';
        }

        m_modules.push_back(rx::make_unique<Module>(module_name, buf));

        Module* module = m_modules.back().get();

        auto scanner = Scanner(module->source());
        module_nodes.push_back({std::move(scanner), module, nullptr});
    }

    tsl::robin_map<std::string_view, ModuleNode*> module_name_map;
    for (auto& module_node : module_nodes) {
        module_name_map.insert({module_node.module->name(), &module_node});
    }

    auto& builtin_module_node = *module_name_map.at("builtin");

    StringInterner string_interner;
    AstAllocator ast_allocator;

    for (auto& [scanner, module, module_stmt] : module_nodes) {
        Parser parser(&scanner, &ast_allocator, &string_interner);

        bool parse_success = parser.parse(module_stmt);

        message += fmt::format("Parsing module {}...\n\n", module->name());
        message += "Parsed output:\n";
        message += AstPrinter(scanner.source()).to_string(*module_stmt);
        message += "\n\n";

        if (!parse_success) return false;
    }

    for (auto& [scanner, module, module_stmt] : module_nodes) {
        SemaAnalyzer sema_analyzer(&ast_allocator, module->source());
        sema_analyzer.scan_dependencies(module->name(), module_stmt);
    }

    for (auto& [scanner, module, module_stmt] : module_nodes) {
        ImportMap import_map;

        std::string parent_module_name = std::string(module->name());
        auto fpos = parent_module_name.rfind('.');
        if (fpos != std::string::npos) {
            parent_module_name = parent_module_name.substr(0, fpos);
        }
        else {
            parent_module_name = "";
        }

        // Import builtins
        if (module != s_builtin_module.get()) {
            for (auto& export_symbol_entry : builtin_module_node.module_stmt->exports) {
                AstFunDecl* export_symbol = export_symbol_entry.get();
                auto export_symbol_name = export_symbol->name.str(builtin_module_node.module->source());
                import_map.insert({export_symbol_name, export_symbol});
            }
        }

        for (auto& import_entry : module_stmt->imports) {
            auto import_stmt = import_entry.get();

            std::string import_name;
            if (!parent_module_name.empty()) {
                import_name = parent_module_name + '.';
            }
            for (u32 i = 0; i < import_stmt->package_path.size(); i++) {
                import_name += import_stmt->package_path[i].str(module->source());
                if (i != import_stmt->package_path.size() - 1) {
                    import_name += '.';
                }
            }

            auto it = module_name_map.find(import_name);
            if (it != module_name_map.end()) {
                ModuleNode* found_module_node = it->second;
                if (import_stmt->is_wildcard()) {
                    for (auto& export_symbol_entry : found_module_node->module_stmt->exports) {
                        AstFunDecl* export_symbol = export_symbol_entry.get();
                        auto export_symbol_name = export_symbol->name.str(found_module_node->module->source());
                        import_map.insert({export_symbol_name, export_symbol});
                    }
                }
                else {
                    for (auto import_symbol : import_stmt->import_symbols) {
                        auto symbol = import_symbol.str(module->source());
                        // TODO: Make this O(1) instead of O(N)
                        AstFunDecl* found_fun_decl = nullptr;
                        for (auto& module_export : found_module_node->module_stmt->exports) {
                            if (module_export->name.str(found_module_node->module->source()) == symbol) {
                                found_fun_decl = module_export.get();
                            }
                        }
                        if (found_fun_decl == nullptr) {
                            message += "Cannot find symbol in module " + import_name + ".\n";
                            return false;
                        }
                        import_map.insert({symbol, found_fun_decl});
                    }
                }
            }
            else {
                message += "Cannot find module " + import_name + ".\n";
                return false;
            }
        }

        message += fmt::format("Analyzing module {}...\n", module->name());

        SemaAnalyzer sema_analyzer(&ast_allocator, module->source());
        Vector<SemaResult> sema_errors = sema_analyzer.typecheck(module_stmt, import_map);

        message += "\nAnalyzed output:\n";
        message += AstPrinter(module->source()).to_string(*module_stmt);
        message += "\n\n";

        if (!sema_errors.empty()) {
            message += "\nSema errors: ";
            message += std::to_string(sema_errors.size());
            message += '\n';

            for (auto err : sema_errors) {
                auto error_msg = err.to_error_msg(module->source());
                auto line = scanner.get_line(error_msg.loc);
                std::string_view str = {reinterpret_cast<const char* const>(scanner.source() + error_msg.loc.source_loc),
                                        (size_t)error_msg.loc.length};
                message += fmt::format("[line {}] Error at '{}': {}\n", line, str, error_msg.message);
            }

            return false;
        }

        for (auto [name, fun_stmt] : import_map) {
            if (fun_stmt->is_native) {
                module->m_native_function_table.push_back({
                    std::string(name),
                    std::string(fun_stmt->module),
                    FunctionTypeData(*fun_stmt->type, module_name_map[fun_stmt->module]->module->source()),
                    NativeFunctionRef {}
                });
            }
            else {
                module->m_function_table.push_back({
                    std::string(name),
                    std::string(fun_stmt->module),
                    FunctionTypeData(*fun_stmt->type, module_name_map[fun_stmt->module]->module->source()),
                    nullptr
                });
            }
        }
    }

    for (auto& [scanner, module, module_stmt] : module_nodes) {
        Compiler compiler(&scanner);
        auto res = compiler.compile(*module_stmt, *module);
        if (res.type != CompileResultType::Ok) {
            message += res.message;
            return false;
        }
        m_module_names.insert({module->name(), module});
    }

    load_builtin_functions();

    for (auto& [scanner, module, module_stmt] : module_nodes) {
        module->m_runtime_function_table.reserve(module->m_function_table.size());
        for (auto& fn_entry : module->m_function_table) {
            Chunk* found_chunk = fn_entry.chunk.get();
            if (found_chunk == nullptr) {
                auto it = module_name_map.find(fn_entry.module);
                if (it != module_name_map.end()) {
                    auto found_module = it->second->module;
                    // TODO: Make this O(1) instead of O(N)
                    for (auto& found_fn_entry : found_module->m_function_table) {
                        if (found_fn_entry.name == fn_entry.name) {
                            found_chunk = found_fn_entry.chunk.get();
                            break;
                        }
                    }
                }
                if (found_chunk == nullptr) {
                    message += fmt::format("In module {}: Cannot find chunk for {} in module {}!",
                                           module->name(), fn_entry.name, fn_entry.module);
                    return false;
                }
            }
            module->m_runtime_function_table.push_back(found_chunk);
        }

        module->m_runtime_native_fun_table.reserve(module->m_native_function_table.size());
        for (auto& fn_entry : module->m_native_function_table) {
            NativeFunctionRef found_fun = fn_entry.fun;
            if (found_fun == nullptr) {
                auto it = module_name_map.find(fn_entry.module);
                if (it != module_name_map.end()) {
                    auto found_module = it->second->module;
                    // TODO: Make this O(1) instead of O(N)
                    for (auto& found_fn_entry : found_module->m_native_function_table) {
                        if (found_fn_entry.name == fn_entry.name) {
                            found_fun = found_fn_entry.fun;
                            break;
                        }
                    }
                }
                if (found_fun == nullptr) {
                    message += fmt::format("In module {}: Cannot find chunk for {} in module {}!",
                                           module->name(), fn_entry.name, fn_entry.module);
                    return false;
                }
            }
            module->m_runtime_native_fun_table.push_back(found_fun);
        }

        module->m_chunk.m_function_table = module->m_runtime_function_table.data();
        module->m_chunk.m_native_function_table = module->m_runtime_native_fun_table.data();
        module->m_chunk.find_ref_local_offsets();
        for (auto& fun_entry : module->m_function_table) {
            if (fun_entry.chunk.get()) {
                fun_entry.chunk->m_function_table = module->m_runtime_function_table.data();
                fun_entry.chunk->m_native_function_table = module->m_runtime_native_fun_table.data();
                fun_entry.chunk->find_ref_local_offsets();
            }
        }
    }

    return true;
}

Module* Library::get_module(std::string_view path) {
    auto it = m_module_names.find(path);
    if (it != m_module_names.end()) {
        return it->second;
    }
    else {
        return nullptr;
    }
}

const char* Library::s_builtin_module_src = R"(
pub native fun print_i32(value: i32);
pub native fun print_i64(value: i64);
pub native fun print_u32(value: u32);
pub native fun print_u64(value: u64);
pub native fun print_f32(value: f32);
pub native fun print_f64(value: f64);
pub native fun print(value: string);
pub native fun concat(a: string, b: string);
pub native fun clock(): f64;
)";

void Library::load_builtin_functions() {

#define ADD_NATIVE_PRINT_FUN(Type, FormatStr) \
    s_builtin_module->add_native_function("print_" #Type, [](ArgStack* args) { \
        Type value = args->pop_##Type(); \
        printf(FormatStr "\n", value); \
    });

    ADD_NATIVE_PRINT_FUN(i32, "%d")
    ADD_NATIVE_PRINT_FUN(i64, "%lld")
    ADD_NATIVE_PRINT_FUN(u32, "%u")
    ADD_NATIVE_PRINT_FUN(u64, "%llu")
    ADD_NATIVE_PRINT_FUN(f32, "%f")
    ADD_NATIVE_PRINT_FUN(f64, "%f")

    s_builtin_module->add_native_function("print", [](ArgStack* args) {
        ObjString* str= reinterpret_cast<ObjString*>(args->pop_ref());
        puts(str->chars());
        str->obj().decref();
    });

    s_builtin_module->add_native_function("concat", [](ArgStack* args) {
        ObjString* b = reinterpret_cast<ObjString*>(args->pop_ref());
        ObjString* a = reinterpret_cast<ObjString*>(args->pop_ref());
        ObjString* res = ObjString::concat(a, b);
        args->push_ref(reinterpret_cast<Obj*>(res));
        a->obj().decref();
        b->obj().decref();
    });

    s_builtin_module->add_native_function("clock", [](ArgStack* args) {
        args->push_f64((f64)clock() / CLOCKS_PER_SEC);
    });

#undef ADD_NATIVE_PRINT_FUN

}

}