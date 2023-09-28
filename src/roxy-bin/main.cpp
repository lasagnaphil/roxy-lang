#include <roxy/scanner.hpp>
#include <roxy/parser.hpp>
#include <roxy/ast_printer.hpp>
#include <roxy/fmt/core.h>

int main() {
    using namespace rx;

    const char* src = "-123 + (45.67) * \"asdf\"";
    Scanner scanner(reinterpret_cast<const u8*>(src));
    StringInterner string_interner;
    Parser parser(&scanner, &string_interner);

    auto expr = parser.parse();
    fmt::print("{}\n", AstPrinter::to_string(*expr));
    return 0;
}
