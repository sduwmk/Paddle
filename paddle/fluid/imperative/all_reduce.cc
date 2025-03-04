// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#if defined(PADDLE_WITH_NCCL) || defined(PADDLE_WITH_RCCL)

#include "paddle/fluid/imperative/all_reduce.h"

#include "paddle/fluid/framework/convert_utils.h"

#ifdef PADDLE_WITH_NCCL
#include <nccl.h>
#endif

#ifdef PADDLE_WITH_RCCL
#include <rccl.h>
#endif

#include "paddle/fluid/framework/scope.h"
#include "paddle/fluid/framework/selected_rows_utils.h"
#include "paddle/fluid/framework/variable.h"
#include "paddle/fluid/imperative/parallel_context.h"
#include "paddle/fluid/platform/device/gpu/nccl_helper.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/utils/string/string_helper.h"

namespace paddle {
namespace imperative {

static const phi::Place &GetVarPlace(const framework::Variable &src) {
  if (src.IsType<phi::DenseTensor>()) {
    return src.Get<phi::DenseTensor>().place();
#if NCCL_VERSION_CODE >= 2212
  } else if (src.IsType<phi::SelectedRows>()) {
    return src.Get<phi::SelectedRows>().value().place();
#endif
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "Cannot get unsupported variable type %s for imperative allreduce, "
        "only "
        "LoDTensor and SelectedRows are supported.",
        platform::demangle(framework::ToTypeName(src.Type()))));
  }
}

static void AllReduce(const phi::DenseTensor &src,
                      phi::DenseTensor *dst,
                      const gpuStream_t stream,
                      const platform::NCCLComm *comm) {
  const auto &place = src.place();
  PADDLE_ENFORCE_EQ(
      phi::is_gpu_place(place),
      true,
      platform::errors::Unimplemented(
          "Imperative mode does not support multi-CPU training yet."));

  const void *src_ptr = src.data();
  dst->Resize(src.dims());
  auto *dst_ptr = dst->mutable_data(src.place(), src.dtype());
  auto nccl_dtype =
      platform::ToNCCLDataType(framework::TransToProtoVarType(src.dtype()));
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::ncclAllReduce(src_ptr,
                                                              dst_ptr,
                                                              src.numel(),
                                                              nccl_dtype,
                                                              ncclSum,
                                                              comm->comm(),
                                                              stream));
}

#if NCCL_VERSION_CODE >= 2212
static void AllReduce(const phi::SelectedRows &src,
                      phi::SelectedRows *dst,
                      const ParallelStrategy &strategy,
                      const gpuStream_t stream,
                      const platform::NCCLComm *comm) {
  VLOG(3) << "SelectedRows AllReduce start";
  const auto &src_tensor = src.value();
  const auto &place = src_tensor.place();
  PADDLE_ENFORCE_EQ(
      phi::is_gpu_place(place),
      true,
      platform::errors::Unimplemented(
          "Imperative mode does not support multi-CPU training yet."));

  auto dtype = framework::TransToProtoVarType(src_tensor.dtype());
  auto nccl_dtype = platform::ToNCCLDataType(dtype);
  auto *dev_ctx = static_cast<phi::GPUContext *>(
      platform::DeviceContextPool::Instance().Get(place));

  bool use_calc_stream = (dev_ctx->stream() == stream);
  VLOG(4) << "Is use calculate stream: " << use_calc_stream;

  // 1. Gather rows number from all workers. Here use ncclAllGather to do this,
  // but we can use other ways to implement is in the future
  const auto &src_rows = src.rows();
  phi::Vector<int64_t> rows_num_vector(strategy.nranks_);
  rows_num_vector[strategy.local_rank_] = static_cast<int64_t>(src_rows.size());
  // CUDAMutableData use CalStream
  phi::MixVector<int64_t> mixv_rows_num_vector(&rows_num_vector);
  auto *gpu_rows_num_ptr = mixv_rows_num_vector.CUDAMutableData(place);
  VLOG(4) << "start dev_ctx->wait";
  if (!use_calc_stream) {
    dev_ctx->Wait();
  }
  PADDLE_ENFORCE_GPU_SUCCESS(
      platform::dynload::ncclAllGather(gpu_rows_num_ptr + strategy.local_rank_,
                                       gpu_rows_num_ptr,
                                       1,
                                       ncclInt64,
                                       comm->comm(),
                                       stream));

  if (!use_calc_stream) {
    platform::GpuStreamSync(stream);
  }

  mixv_rows_num_vector.CopyToCPU();
  const auto *cpu_rows_num_ptr = rows_num_vector.data();
  auto rows_num = std::accumulate(cpu_rows_num_ptr,
                                  cpu_rows_num_ptr + strategy.nranks_,
                                  static_cast<int64_t>(0));
  dst->set_height(src.height());

  VLOG(3) << "Gather rows: " << string::join_strings(rows_num_vector, ',')
          << ", total rows number: " << rows_num
          << ", height: " << src.height();

  auto *dst_rows = dst->mutable_rows();
  dst_rows->resize(rows_num);
  phi::MixVector<int64_t> mixv_dst_rows(dst_rows);
  auto *dst_rows_ptr = mixv_dst_rows.CUDAMutableData(place);
  phi::MixVector<int64_t> mixv_src_rows(&src_rows);
  const auto *src_rows_ptr = mixv_src_rows.CUDAData(place);

  auto *dst_tensor = dst->mutable_value();
  auto dims = src_tensor.dims();
  dims[0] = rows_num;
  auto feature_size = common::product(dims) / dims[0];
  dst_tensor->Resize(dims);
  auto *dst_tensor_ptr = dst_tensor->mutable_data(place, src_tensor.dtype());
  const auto *src_tensor_ptr = src_tensor.data();

  auto sizeof_dtype = framework::SizeOfType(dtype);
  int64_t row_offset = 0;
  if (!use_calc_stream) {
    dev_ctx->Wait();
  }
  if (std::all_of(cpu_rows_num_ptr,
                  cpu_rows_num_ptr + strategy.nranks_,
                  [&](int64_t row) { return row == cpu_rows_num_ptr[0]; })) {
    // During sparse communication, the number of each card is same.
    // allgather is used to speed up the allreduce by replacing broadcast.
    auto row_sendcount = cpu_rows_num_ptr[0];
    VLOG(3) << "allgather replaces broadcast to speed up in sparse allreduce";
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::ncclAllGather(src_rows_ptr,
                                                                dst_rows_ptr,
                                                                row_sendcount,
                                                                ncclInt64,
                                                                comm->comm(),
                                                                stream));
    auto value_sendcount = cpu_rows_num_ptr[0] * feature_size;
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::ncclAllGather(src_tensor_ptr,
                                                                dst_tensor_ptr,
                                                                value_sendcount,
                                                                nccl_dtype,
                                                                comm->comm(),
                                                                stream));
  } else {
    for (int i = 0; i < strategy.nranks_; ++i) {
      if (cpu_rows_num_ptr[i] > 0) {
        // 2. Broadcast the rows of SelectedRows
        PADDLE_ENFORCE_GPU_SUCCESS(
            platform::dynload::ncclBroadcast(src_rows_ptr,
                                             dst_rows_ptr + row_offset,
                                             cpu_rows_num_ptr[i],
                                             ncclInt64,
                                             i,
                                             comm->comm(),
                                             stream));
        // 3. Broadcast the tensor data of SelectedRows
        auto *dst_tensor_ptr_i = reinterpret_cast<uint8_t *>(dst_tensor_ptr) +
                                 row_offset * feature_size * sizeof_dtype;
        PADDLE_ENFORCE_GPU_SUCCESS(
            platform::dynload::ncclBroadcast(src_tensor_ptr,
                                             dst_tensor_ptr_i,
                                             cpu_rows_num_ptr[i] * feature_size,
                                             nccl_dtype,
                                             i,
                                             comm->comm(),
                                             stream));
        row_offset += cpu_rows_num_ptr[i];
      }
    }
  }
  if (!use_calc_stream) {
    platform::GpuStreamSync(stream);
  }
  mixv_dst_rows.CopyToCPU();
  VLOG(3) << "Original SelectedRows rows: "
          << string::join_strings(src_rows, ',');
  VLOG(3) << "Result SelectedRows rows: "
          << string::join_strings(*dst_rows, ',');
}
#endif

void AllReduce(const framework::Variable &src,
               framework::Variable *dst,
               const ParallelStrategy &strategy,
               int ring_id,
               bool use_calc_stream) {
  const auto &place = GetVarPlace(src);
  auto *dev_ctx = static_cast<phi::GPUContext *>(
      platform::DeviceContextPool::Instance().Get(place));
  platform::NCCLComm *comm =
      platform::NCCLCommContext::Instance().Get(ring_id, place);
  gpuStream_t stream = (use_calc_stream ? dev_ctx->stream() : comm->stream());

  if (src.IsType<phi::DenseTensor>()) {
    if (!dst->IsType<phi::DenseTensor>()) {
      dst->Clear();
    }
    AllReduce(src.Get<phi::DenseTensor>(),
              dst->GetMutable<phi::DenseTensor>(),
              stream,
              comm);
#if NCCL_VERSION_CODE >= 2212
  } else if (src.IsType<phi::SelectedRows>()) {
    if (&src != dst) {
      if (!dst->IsType<phi::SelectedRows>()) {
        dst->Clear();
      }
      AllReduce(src.Get<phi::SelectedRows>(),
                dst->GetMutable<phi::SelectedRows>(),
                strategy,
                stream,
                comm);
    } else {
      // SelectedRows cannot be allreduce in-place
      framework::Variable tmp_dst;
      AllReduce(src.Get<phi::SelectedRows>(),
                tmp_dst.GetMutable<phi::SelectedRows>(),
                strategy,
                stream,
                comm);
      // stream must synchronize to ensure accuracy of the move operation
      platform::GpuStreamSync(stream);
      *dst = std::move(tmp_dst);
    }
#endif
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "Unsupported variable type %s for imperative allreduce, only "
        "LoDTensor and SelectedRows are supported.",
        platform::demangle(framework::ToTypeName(src.Type()))));
  }
}

void AllReduce(const framework::Variable &src,
               framework::Variable *dst,
               const ParallelStrategy &strategy) {
  AllReduce(src, dst, strategy, 0, true);
}

}  // namespace imperative
}  // namespace paddle

#endif
