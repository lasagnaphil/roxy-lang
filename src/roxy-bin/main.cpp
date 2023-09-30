#include <roxy/scanner.hpp>
#include <roxy/parser.hpp>
#include <roxy/ast_printer.hpp>
#include <roxy/fmt/core.h>
#include <roxy/core/file.hpp>

#include <cstdio>

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

        Scanner scanner(buf.data());
        StringInterner string_interner;
        Parser parser(&scanner, &string_interner);

        Vector<Stmt*> statements;
        parser.parse(statements);

        fmt::print("{}\n", AstPrinter().to_string(statements));
        return 0;
    }
}
