#pragma once

// Command lines of the tools, through the nx argument parser: declare the options on a nx::cli::Parser, then
//
//     if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;

#include <cstdio>

#include "core/foundation/serialization/arguments.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore {

// Parses argv. --help prints the help (0), errors print the message and the help (2); -1 when parsed, with the
// chosen subcommand in `command` (empty: the root command).
inline int parse_command_line(nx::cli::Parser& cli, int argc, char** argv, nx::string* command = nullptr) {
    const nx::cli::ParseResult result = cli.parse(argc, argv);
    if (result.status == nx::cli::ParseStatus::Help) {
        std::fputs(cli.help(result.command).c_str(), stdout);
        return 0;
    }
    if (!result.parsed()) {
        std::fprintf(stderr, "%s\n\n%s", result.message.c_str(), cli.help(result.command).c_str());
        return 2;
    }
    if (command) *command = result.command;
    return -1;
}

}  // namespace psprecore
