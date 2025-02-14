#!/bin/bash -e

# Copyright 2018- The Pixie Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

# This version is identical to the Python gRPC module version for easier recognition.
# This is because the produced docker image is mainly used for testing tracing Python gRPC app,
# which depends on the version of the module.
version=1.0
tag="gcr.io/pixie-oss/pixie-dev-public/python_grpc_1_19_0_helloworld:$version"

docker build . -t $tag
docker push $tag


sha=$(docker inspect --format='{{index .RepoDigests 0}}' $tag | cut -f2 -d'@')

echo ""
echo "Image pushed!"
echo "IMPORTANT: Now update //bazel/container_images.bzl with the following digest: $sha"
