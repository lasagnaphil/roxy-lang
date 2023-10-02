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

        BlockStmt* block_stmt = parser.parse();

        fmt::print("Parsed output:\n");
        fmt::print("{}\n", AstPrinter().to_string(*block_stmt));

        SemaAnalyzer sema_analyzer(parser.get_ast_allocator());

        auto sema_errors = sema_analyzer.check(block_stmt);

        fmt::print("Sema errors: {}\n", sema_errors.empty()? "none" : std::to_string(sema_errors.size()));

        for (auto err : sema_errors) {
            switch (err.res_type) {
                case SemaResultType::UndefinedVar:
                    fmt::print("- Undefined variable.\n");
                    break;
                case SemaResultType::WrongType:
                    fmt::print("- Wrong type.\n");
                    break;
                case SemaResultType::InvalidInitializerType:
                    fmt::print("- Invalid initializer type.\n");
                    break;
                case SemaResultType::InvalidAssignedType:
                    fmt::print("- Invalid assignment type.\n");
                    break;
                case SemaResultType::IncompatibleTypes:
                    fmt::print("- Incompatible types.\n");
                    break;
                case SemaResultType::CannotInferType:
                    fmt::print("- Cannot infer kind.\n");
                    break;
                case SemaResultType::Misc:
                    fmt::print("- Misc.\n");
                    break;
                default:
                    break;
            }
        }

        fmt::print("\nAfter semantic analysis:\n");
        fmt::print("{}\n", AstPrinter().to_string(*block_stmt));

        return 0;
    }
}
