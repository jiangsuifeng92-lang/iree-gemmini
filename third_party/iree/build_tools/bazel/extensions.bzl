# Copyright 2024 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Bzlmod extension for IREE repository rules."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:local.bzl", "local_repository", "new_local_repository")
load("//build_tools/bazel:workspace.bzl", "cuda_auto_configure")
load("//build_tools/bazel:numpy_configure.bzl", "numpy_configure")

def _iree_external_plugins_impl(repository_ctx):
    handle_plugin_ids = "".join(["HANDLE_PLUGIN_ID(%s)\\n" % plugin_id for plugin_id in repository_ctx.attr.plugins])

    cc_library_deps = ", ".join(["\"%s\"" % target for target in repository_ctx.attr.plugins.values()])

    repository_ctx.file("BUILD.bazel", """
load("@rules_cc//cc:defs.bzl", "cc_library")

package(
    default_visibility = ["//visibility:public"],
)

genrule(
    name = "ExternalPlugins_inc__generate",
    outs = ["ExternalPlugins.inc"],
    cmd = "printf -- '%s' > $@",
)

cc_library(
    name = "ExternalPlugins",
    deps = [ %s ],
)
""" % (handle_plugin_ids, cc_library_deps))

# TODO(sflur): make sure the libraries are "watched", so the repo is re-fetched
# when they change.
iree_external_plugins = repository_rule(
    implementation = _iree_external_plugins_impl,
    attrs = { "plugins": attr.string_dict(mandatory=True) }
)

def _iree_external_drivers_impl(repository_ctx):
    drivers = repository_ctx.attr.drivers

    if not drivers:
        repository_ctx.file("BUILD.bazel", """
load("@rules_cc//cc:defs.bzl", "cc_library")

package(
    default_visibility = ["//visibility:public"],
)

cc_library(
    name = "ExternalDrivers",
)
""")
        return

    decls = []
    calls = []
    targets = []
    for _driver_name, driver_json in drivers.items():
        driver_info = json.decode(driver_json)
        register_fn = driver_info["register_fn"]
        target = driver_info["target"]

        decls.append("extern iree_status_t %s(iree_hal_driver_registry_t* registry);" % register_fn)
        calls.append("  IREE_RETURN_IF_ERROR(%s(registry));" % register_fn)
        targets.append(target)

    init_external_c = """
#include "iree/base/api.h"
#include "iree/hal/api.h"

%s

iree_status_t iree_hal_register_external_drivers(
    iree_hal_driver_registry_t* registry) {
%s
  return iree_ok_status();
}
""" % ("\n".join(decls), "\n".join(calls))

    repository_ctx.file("init_external.c", init_external_c)

    deps = ", ".join(["\"%s\"" % target for target in targets])

    repository_ctx.file("BUILD.bazel", """
load("@rules_cc//cc:defs.bzl", "cc_library")

package(
    default_visibility = ["//visibility:public"],
)

cc_library(
    name = "ExternalDrivers",
    srcs = ["init_external.c"],
    deps = [
        "@iree_core//runtime/src/iree/base",
        "@iree_core//runtime/src/iree/hal",
        %s
    ],
    defines = ["IREE_HAVE_HAL_EXTERNAL_DRIVERS=1"],
)
""" % deps)

iree_external_drivers = repository_rule(
    implementation = _iree_external_drivers_impl,
    attrs = {
        "drivers": attr.string_dict(mandatory=True), # driver_name -> JSON string
    }
)

def _pip_alias_repository_impl(repository_ctx):
    target_hub = repository_ctx.attr.target_hub
    packages = repository_ctx.attr.packages

    build_content = "package(default_visibility = [\"//visibility:public\"])\n\n"
    for pkg in packages:
        build_content += "alias(\n    name = \"{pkg}\",\n    actual = \"{target_hub}//{pkg}\",\n)\n\n".format(
            pkg = pkg,
            target_hub = target_hub,
        )

    repository_ctx.file("BUILD.bazel", build_content)

pip_alias_repository = repository_rule(
    implementation = _pip_alias_repository_impl,
    attrs = {
        "target_hub": attr.string(mandatory = True),
        "packages": attr.string_list(mandatory = True),
    },
)

def _iree_extension_impl(module_ctx):
    """Implementation of the IREE module extension."""

    # Create llvm-raw only when IREE is the root module.
    # This allows downstream consumers to provide their own LLVM.
    if any([m.is_root and m.name == "iree_core" for m in module_ctx.modules]):
        new_local_repository(
            name = "llvm-raw",
            build_file_content = "# empty",
            path = "third_party/llvm-project",
        )
    numpy_configure(name = "numpy_configure")
    pip_alias_repository(
        name = "pip",
        target_hub = "@pip_internal",
        packages = ["numpy", "ml_dtypes"],
    )

    maybe_iree_root = module_ctx.path(Label("//:MODULE.bazel")).dirname
    iree_root = str(maybe_iree_root) if maybe_iree_root else "."

    # iree_root = "."
    # for mod in module_ctx.modules:
    #     if mod.tags.location:
    #         iree_root = mod.tags.location[-1].path

    # Googletest
    local_repository(
        name = "com_google_googletest",
        path = iree_root + "/third_party/googletest",
    )

    # Flatcc
    new_local_repository(
        name = "com_github_dvidelabs_flatcc",
        build_file = "@iree_core//:build_tools/third_party/flatcc/BUILD.overlay",
        path = iree_root + "/third_party/flatcc",
    )

    # Vulkan headers
    new_local_repository(
        name = "vulkan_headers",
        build_file = "@iree_core//:build_tools/third_party/vulkan_headers/BUILD.overlay",
        path = iree_root + "/third_party/vulkan_headers",
    )

    # StableHLO
    local_repository(
        name = "stablehlo",
        path = iree_root + "/third_party/stablehlo",
    )

    # Benchmark
    local_repository(
        name = "com_google_benchmark",
        path = iree_root + "/third_party/benchmark",
    )

    # SPIRV-Cross
    new_local_repository(
        name = "spirv_cross",
        build_file = "@iree_core//:build_tools/third_party/spirv_cross/BUILD.overlay",
        path = iree_root + "/third_party/spirv_cross",
    )

    # Tracy
    new_local_repository(
        name = "tracy_client",
        build_file = "@iree_core//:build_tools/third_party/tracy_client/BUILD.overlay",
        path = iree_root + "/third_party/tracy",
    )

    # NCCL
    new_local_repository(
        name = "nccl",
        build_file = "@iree_core//:build_tools/third_party/nccl/BUILD.overlay",
        path = iree_root + "/third_party/nccl",
    )

    # HIP API headers
    new_local_repository(
        name = "hip_api_headers",
        build_file = "@iree_core//:build_tools/third_party/hip-api-headers/BUILD.overlay",
        path = iree_root + "/third_party/hip-build-deps",
    )

    # HSA runtime headers
    new_local_repository(
        name = "hsa_runtime_headers",
        build_file = "@iree_core//:build_tools/third_party/hsa-runtime-headers/BUILD.overlay",
        path = iree_root + "/third_party/hsa-runtime-headers",
    )

    # RCCL
    new_local_repository(
        name = "rccl",
        build_file = "@iree_core//:build_tools/third_party/rccl/BUILD.overlay",
        path = iree_root + "/third_party/rccl",
    )

    # eyalroz/printf (platform-independent printf implementation)
    new_local_repository(
        name = "printf_lib",
        build_file = "@iree_core//:build_tools/third_party/printf/BUILD.overlay",
        path = iree_root + "/third_party/printf",
    )

    # WebGPU headers
    new_local_repository(
        name = "webgpu_headers",
        build_file = "@iree_core//:build_tools/third_party/webgpu-headers/BUILD.overlay",
        path = iree_root + "/third_party/webgpu-headers",
    )

    # AMDGPU device library bitcode (ocml, ockl) for ROCM compilation.
    # Matches the CMake fetch in compiler/plugins/target/ROCM/CMakeLists.txt.
    http_archive(
        name = "amdgpu_device_libs",
        urls = ["https://github.com/shark-infra/amdgpu-device-libs/releases/download/v20231101/amdgpu-device-libs-llvm-6086c272a3a59eb0b6b79dcbe00486bf4461856a.tgz"],
        sha256 = "336362416c68fdd8bb80328f65ca7ebaa0c119ea19c95df6df30c832a4df39b9",
        build_file = "@iree_core//:build_tools/third_party/amdgpu_device_libs/BUILD.overlay",
    )

    # CUDA auto-configuration
    cuda_auto_configure(
        name = "iree_cuda",
        iree_repo_alias = "@iree_core",
    )

    external_plugins = {}
    for mod in module_ctx.modules:
        for register_plugin in mod.tags.register_plugin:
            external_plugins[register_plugin.plugin_id] = str(register_plugin.target)

    iree_external_plugins(
        name = "iree_external_plugins",
        plugins = external_plugins,
    )

    external_drivers = {}
    for mod in module_ctx.modules:
        for register_driver in mod.tags.register_driver:
            external_drivers[register_driver.driver_name] = json.encode({
                "register_fn": register_driver.register_fn,
                "target": str(register_driver.target),
            })

    iree_external_drivers(
        name = "iree_external_drivers",
        drivers = external_drivers,
    )

_location = tag_class(attrs = {"path": attr.string()})

_register_plugin = tag_class(attrs = {
    "plugin_id": attr.string(mandatory=True),
    "target": attr.label(mandatory=True),
})

_register_driver = tag_class(attrs = {
    "driver_name": attr.string(mandatory=True),
    "register_fn": attr.string(mandatory=True),
    "target": attr.label(mandatory=True),
})

iree_extension = module_extension(
    implementation = _iree_extension_impl,
    tag_classes = {
        "location": _location,
        "register_plugin" : _register_plugin,
        "register_driver" : _register_driver,
    },
)
