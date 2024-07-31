/* Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once
#include "paddle/fluid/framework/data_type.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/op_registry.h"

namespace paddle {
namespace operators {

template <typename T, typename DeviceContext>
class GlobalGatherOpCPUKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx UNUSED) const override {
    PADDLE_THROW(common::errors::Unavailable(
        "Do not support global gather op for cpu kernel now."));
  }
};

template <typename Context, typename T>
struct GlobalGatherFunctor {
  void operator()(const framework::ExecutionContext& ctx);
};

template <typename Context, typename T>
struct GlobalGatherProcessGroupFunctor {
  void operator()(const framework::ExecutionContext& ctx);
};

}  // namespace operators
}  // namespace paddle
