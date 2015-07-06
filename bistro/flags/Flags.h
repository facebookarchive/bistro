#pragma once

#include <gflags/gflags.h>

/**
 * Define flags shared by different modules here, so unit tests won't always
 * link to the module where the flag is defined
 */

DECLARE_bool(log_performance);
