# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@llvm-project//mlir:tblgen.bzl", "gentbl_filegroup")

def iree_py_extension(name, srcs = [], deps = [], copts = [], **kwargs):
    """Compiles a C++ Python extension module with RTTI/Exceptions enabled.

    Handles Windows (.pyd) vs. Linux/macOS (.so) naming.
    """
    native.cc_binary(
        name = name + ".so",
        srcs = srcs,
        copts = copts + [
            "-fexceptions",
            "-frtti",
        ],
        features = ["exceptions"],
        linkshared = True,
        linkstatic = True,
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        deps = deps + [
            "@nanobind",
            "@rules_python//python/cc:current_py_cc_headers",
        ],
        **kwargs
    )

    native.cc_binary(
        name = name + ".pyd",
        srcs = srcs,
        copts = copts + ["/EHsc", "/GR"],
        features = ["exceptions"],
        linkshared = True,
        linkstatic = True,
        target_compatible_with = ["@platforms//os:windows"],
        deps = deps + [
            "@nanobind",
            "@rules_python//python/cc:current_py_cc_headers",
        ],
        **kwargs
    )

    native.filegroup(
        name = name,
        srcs = select({
            "@platforms//os:windows": [":" + name + ".pyd"],
            "//conditions:default": [":" + name + ".so"],
        }),
        visibility = ["//visibility:public"],
    )

def iree_py_dialect_bindings(name, td_file, dialect_name, gen_enums = True, output_name = None, deps = [], **kwargs):
    """Generates Python bindings for an IREE dialect from TableGen."""
    out_base = output_name if output_name else dialect_name
    tbl_outs = {
        "iree/compiler/dialects/_" + out_base + "_ops_gen.py": [
            "-gen-python-op-bindings",
            "-bind-dialect=" + dialect_name,
        ],
    }
    if gen_enums:
        tbl_outs["iree/compiler/dialects/_" + out_base + "_enum_gen.py"] = [
            "-gen-python-enum-bindings",
            "-bind-dialect=" + dialect_name,
        ]

    gentbl_filegroup(
        name = name,
        tbl_outs = tbl_outs,
        tblgen = "@llvm-project//mlir:mlir-tblgen",
        td_file = td_file,
        deps = deps,
        **kwargs
    )
