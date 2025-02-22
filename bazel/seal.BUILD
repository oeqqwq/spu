# Copyright 2022 Ant Group Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@spulib//bazel:spu.bzl", "spu_cmake_external")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

arm64_config = {
    "SEAL_USE_MSGSL": "OFF",
    "SEAL_BUILD_DEPS": "OFF",
    "SEAL_USE_ZSTD": "ON",
    "SEAL_USE_ZLIB": "OFF",
    "SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT": "OFF",  #NOTE(juhou) required by apsi
    "CMAKE_INSTALL_LIBDIR": "lib",
}

x64_config = {
    "SEAL_USE_MSGSL": "OFF",
    "SEAL_BUILD_DEPS": "OFF",
    "SEAL_USE_ZSTD": "ON",
    "SEAL_USE_ZLIB": "OFF",
    "SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT": "OFF",  #NOTE(juhou) required by apsi
    "CMAKE_INSTALL_LIBDIR": "lib",
    "CpuFeatures_DIR": "$EXT_BUILD_DEPS/cpu_features/lib/cmake/CpuFeatures/",
    "EXT_BUILD_DEPS": "$EXT_BUILD_DEPS",
    "SEAL_USE_INTEL_HEXL": "ON",
}

spu_cmake_external(
    name = "seal",
    cache_entries = select({
        "@platforms//cpu:x86_64": x64_config,
        "//conditions:default": arm64_config,
    }),
    lib_source = "@com_github_microsoft_seal//:all",
    out_include_dir = "include/SEAL-4.0",
    out_static_libs = ["libseal-4.0.a"],
    deps = [
        "@com_github_facebook_zstd//:zstd",
    ] + select({
        "@platforms//cpu:x86_64": ["@com_intel_hexl//:hexl"],
        "//conditions:default": [],
    }),
)
