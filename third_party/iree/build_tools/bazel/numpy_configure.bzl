# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

def _numpy_configure_impl(repository_ctx):
    # Run python to get the numpy include path.
    result = repository_ctx.execute(["python3", "-c", "import numpy; print(numpy.get_include())"])
    if result.return_code != 0:
        fail("Failed to get numpy include path: %s" % result.stderr)

    numpy_include = result.stdout.strip()
    repository_ctx.symlink(numpy_include, "include")

    repository_ctx.file("BUILD", """
cc_library(
    name = "numpy_headers",
    hdrs = glob(["include/numpy/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
""")

numpy_configure = repository_rule(
    implementation = _numpy_configure_impl,
    local = True,
)
