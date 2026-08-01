// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include "config.h"

namespace rfs {

// Exercise the full protocol stack without mounting: login, walk the
// namespace, fetch one blob direct-with-pin, verify its hash. Returns a
// process exit code.
int run_probe(const Options& o);

}  // namespace rfs
