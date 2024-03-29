#!/usr/bin/env python
# Copyright (c) Facebook, Inc. and its affiliates.

import specs.fizz as fizz
import specs.fmt as fmt
import specs.folly as folly
import specs.gmock as gmock
import specs.mvfst as mvfst
import specs.sodium as sodium
import specs.wangle as wangle
import specs.zstd as zstd


def fbcode_builder_spec(builder):
    # Projects that **depend** on proxygen should don't need to build tests
    # or QUIC support.
    builder.add_option(
        "proxygen/proxygen:cmake_defines",
        {
            # These 2 are set to ON in `proxygen_quic.py`
            "BUILD_QUIC": "OFF",
            "BUILD_TESTS": "OFF",
            # NB May no longer be needed now that Bistro is gone.
            "BUILD_SHARED_LIBS": "OFF",
        },
    )

    return {
        "depends_on": [gmock, fmt, folly, wangle, fizz, sodium, zstd, mvfst],
        "steps": [builder.fb_github_cmake_install("proxygen/proxygen", "..")],
    }
