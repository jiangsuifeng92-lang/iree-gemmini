# Copyright 2020 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Generates FlatBuffer source files with flatcc."""

def iree_flatbuffer_c_library(
        name,
        srcs,
        flatcc_args = ["--common", "--reader"],
        includes = [],
        testonly = False,
        **kwargs):
    flatcc = "@com_github_dvidelabs_flatcc//:flatcc"

    # Extract the path from the //runtime/src:fbs_files label.
    runtime_src_label = Label("//runtime/src:fbs_files")
    runtime_src_path = runtime_src_label.package
    if runtime_src_label.workspace_root:
        runtime_src_path = runtime_src_label.workspace_root + "/" + runtime_src_path

    flags = [
        "-o$(RULEDIR)",
        "-I " + runtime_src_path,
    ] + flatcc_args

    out_stem = "%s" % (srcs[0].replace(".fbs", ""))

    outs = []
    for arg in flags:
        if arg == "--reader":
            outs.append("%s_reader.h" % (out_stem))
        if arg == "--builder":
            outs.append("%s_builder.h" % (out_stem))
        if arg == "--verifier":
            outs.append("%s_verifier.h" % (out_stem))
        if arg == "--json":
            outs.append("%s_json_parser.h" % (out_stem))
            outs.append("%s_json_printer.h" % (out_stem))

    native.genrule(
        name = name + "_gen",
        srcs = srcs + includes + ["//runtime/src:fbs_files"],
        outs = outs,
        tools = [flatcc],
        cmd = "$(location %s) %s %s" % (flatcc, " ".join(flags), " ".join(["$(location {})".format(src) for src in srcs])),
        testonly = testonly,
    )
    native.cc_library(
        name = name,
        hdrs = outs,
        testonly = testonly,
        **kwargs
    )
