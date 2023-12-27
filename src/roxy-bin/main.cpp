#include <roxy/fmt/core.h>
#include <roxy/core/file.hpp>
#include <roxy/compiler.hpp>
#include <roxy/vm.hpp>
#include <roxy/module.hpp>

int main(int argc, char** argv) {
    using namespace rx;

    if (argc == 1) {
        fmt::print("Usage: roxy <filename>\n");
        return 0; // TDO: repl
    }
    else if (argc == 2) {
        Vector<u8> buf;
        if (!read_file_to_buf(argv[1], buf)) {
            fmt::print("Error while opening file {}!\n", argv[1]);
            return 1;
        }

        Module module("test", buf.data());

        Compiler compiler;
        auto res = compiler.compile(module);
        fmt::print("{}\n", res.message);
        if (res.type != CompileResultType::Ok) {
            fmt::print("Error during compilation; exiting!");
            return 0;
        }

        module.print_disassembly();

        fmt::print("\n");

        VM vm;
        auto interpret_res = vm.run_module(module);
        if (interpret_res != InterpretResult::Ok) {
            fmt::print("Error during execution!\n");
            return 0;
        }

        return 0;
    }
}
