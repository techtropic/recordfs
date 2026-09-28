// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#pragma once

// The build generates version_gen.h from VERSION.txt (both MSBuild and
// CMake). A build without it reports a version the updater refuses to act
// on: comparing releases against a made-up version would reinstall forever.
#if __has_include("version_gen.h")
#include "version_gen.h"
#endif
#ifndef RECORDFS_VERSION
#define RECORDFS_VERSION "0.0.0-dev"
#endif
