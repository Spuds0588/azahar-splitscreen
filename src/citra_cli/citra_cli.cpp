// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#include "citra_cli/citra_cli.h"
#include "citra_cli/compression_cli.h"

namespace CitraCLI {

bool CheckForOptions(const char* optstring, int argc, char* argv[]) {
    // Long options belong to the Qt frontend (--user, --room, --size, ...). GNU
    // getopt would otherwise split "--size" into -s -i -z -e and match the
    // compression CLI's short options.
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            return false;
        }
    }

    const int original_opterr = opterr;
    opterr = 0; // Temporarily suppress invalid option messages

    bool return_value = false;
    int option;
    while ((option = getopt(argc, argv, optstring)) != -1) {
        for (size_t i = 0; optstring[i] != '\0'; ++i) {
            if (optstring[i] == ':') {
                continue;
            }
            if (option == optstring[i]) {
                return_value = true;
                break;
            }
        }
    }

    opterr = original_opterr;
    optind = 1; // Reset getopt so that it can be used again
    return return_value;
}

int ParseCommand(int argc, char* argv[]) {
    if (CheckForOptions(compression_ops_optstring, argc, argv)) {
        return ParseCompressionCommand(argc, argv);
    }
}

} // namespace CitraCLI
