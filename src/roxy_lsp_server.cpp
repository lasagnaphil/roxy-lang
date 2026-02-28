#include "roxy/lsp/server.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main() {
#ifdef _WIN32
    // Set binary mode on stdin/stdout to prevent \r\n translation
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    rx::LspServer server;
    server.run();
    return 0;
}
