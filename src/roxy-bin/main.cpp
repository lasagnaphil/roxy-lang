#include <roxy/vm.hpp>
#include <roxy/library.hpp>

#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    using namespace rx;

    if (argc == 2) {
        Library library;

        std::string msg;
        auto filename = std::string(argv[1]);
        auto parent_path = fs::path(filename).parent_path().string();
        auto module_name = fs::path(filename).stem().string();
        bool compile_success = library.compile_from_files(parent_path, {filename}, msg);
        printf("%s\n", msg.c_str());
        fflush(stdout);
        if (!compile_success) return 9;

        auto module = library.get_module(module_name);
        if (module == nullptr) {
            printf("Cannot find module %s!\n", module_name.c_str());
            return 0;
        }

        module->print_disassembly();

        VM vm;
        if (vm.run_module(*module) != InterpretResult::Ok) {
            printf("Error while running module %s!\n", module_name.c_str());
        }
        return 0;
    }
    if (argc == 3) {
        Library library;

        auto path = std::string(argv[1]);
        auto init_module = std::string(argv[2]);

        std::string msg;
        bool compile_success = library.compile_from_dir(path, msg);
        printf("%s\n", msg.c_str());
        fflush(stdout);
        if (!compile_success) return 0;

        auto module = library.get_module(init_module);
        if (module == nullptr) {
            printf("Cannot find module %s!\n", init_module.c_str());
            return 0;
        }

        module->print_disassembly();

        VM vm;
        if (vm.run_module(*module) != InterpretResult::Ok) {
            printf("Error while running module %s!\n", init_module.c_str());
        }

        return 0;
    }
    else if (argc == 1) {
        fmt::print("Usage: roxy <path> <module>\n");
        return 0; // TODO: repl
    }
    else {
        fmt::print("Usage: roxy <path> <module>\n");
        return 0;
    }
}
