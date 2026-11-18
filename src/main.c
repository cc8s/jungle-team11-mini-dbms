#include <string.h>

#include "cli.h"
#include "http_server.h"

/* ./minidb [옵션] [query.sql]   → CLI (Phase 1·2)
 * ./minidb --server [옵션]      → HTTP API 서버 (Phase 3) */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--server") == 0) {
        return server_main(argc, argv);
    }
    return cli_main(argc, argv);
}
