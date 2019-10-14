/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>

/**
 * Define flags shared by different modules here, so unit tests won't always
 * link to the module where the flag is defined
 */

DECLARE_bool(log_performance);
