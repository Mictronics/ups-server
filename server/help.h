// Part of UPS Server.
// A websocket server reading data from a Bicker PSZ-1063 uExtension module
// in combination with a Bicker UPS.
//
// Copyright (c) 2023 Michael Wolf <michael@mictronics.de>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef HELP_H
#define HELP_H

#include <argp.h>
const char *argp_program_bug_address = "Michael Wolf <michael@mictronics.de>";
static error_t parse_opt(int key, char *arg, struct argp_state *state);

static struct argp_option options[] =
    {
        {0, 0, 0, 0, "Options:", 1},
        {"config", 'c', "configuration file", OPTION_ARG_OPTIONAL, "Configuration file path and name [default: /etc/default/ups-server.cfg]", 1},
        {0}};

#endif /* HELP_H */