// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/phi/kernels/funcs/deformable_conv_functor.h"
#include "paddle/phi/backends/gpu/gpu_context.h"
#include "paddle/phi/common/amp_type_traits.h"
#include "paddle/phi/common/float16.h"
#include "paddle/phi/core/device_context.h"

namespace phi {
namespace funcs {

static constexpr int kNumCUDAThreads = 512;
static constexpr int kNumMaximumNumBlocks = 4096;

static inline int NumBlocks(const int N) {
  return std::min((N + kNumCUDAThreads - 1) / kNumCUDAThreads,
                  kNumMaximumNumBlocks);
}

template <typename T>
__global__ void ModulatedDeformableIm2colGpuKernel(
    const int nthreads,
    const T* data_im,
    const T* data_offset,
    const T* data_mask,
    const int height,
    const int width,
    const int kernel_h,
    const int kernel_w,
    const int pad_h,
    const int pad_w,
    const int stride_h,
    const int stride_w,
    const int dilation_h,
    const int dilation_w,
    const int channel_per_deformable_group,
    const int batch_size,
    const int num_channels,
    const int deformable_group,
    const int height_col,
    const int width_col,
    T* data_col) {
  int index = blockIdx.x * blockDim.x + threadIdx.x;
  int offset = blockDim.x * gridDim.x;

  using MT = typename phi::dtype::MPTypeTrait<T>::Type;
  for (size_t i = index; i < nthreads; i += offset) {
    const int w_col = i % width_col;
    const int h_col = (i / width_col) % height_col;
    const int b_col = (i / width_col) / height_col % batch_size;
    const int c_im = (i / width_col / height_col) / batch_size;
    const int c_col = c_im * kernel_h * kernel_w;

    const int deformable_group_index = c_im / channel_per_deformable_group;

    const int h_in = h_col * stride_h - pad_h;
    const int w_in = w_col * stride_w - pad_w;

    T* data_col_ptr =
        data_col +
        ((c_col * batch_size + b_col) * height_col + h_col) * width_col + w_col;
    const T* data_im_ptr =
        data_im + (b_col * num_channels + c_im) * height * width;
    const T* data_offset_ptr =
        data_offset + (b_col * deformable_group + deformable_group_index) * 2 *
                          kernel_h * kernel_w * height_col * width_col;
    const T* data_mask_ptr =
        data_mask
            ? data_mask + (b_col * deformable_group + deformable_group_index) *
                              kernel_h * kernel_w * height_col * width_col
            : nullptr;

    for (int i = 0; i < kernel_h; ++i) {
      for (int j = 0; j < kernel_w; ++j) {
        const int data_offset_h_ptr =
            ((2 * (i * kernel_w + j)) * height_col + h_col) * width_col + w_col;
        const int data_offset_w_ptr =
            ((2 * (i * kernel_w + j) + 1) * height_col + h_col) * width_col +
            w_col;

        const MT offset_h = static_cast<MT>(data_offset_ptr[data_offset_h_ptr]);
        const MT offset_w = static_cast<MT>(data_offset_ptr[data_offset_w_ptr]);
        MT val = static_cast<MT>(0);
        const MT h_im = h_in + i * dilation_h + offset_h;
        const MT w_im = w_in + j * dilation_w + offset_w;
        if (h_im > -1 && w_im > -1 && h_im < height && w_im < width) {
          val = DmcnIm2colBilinear<T, MT>(
              data_im_ptr, width, height, width, h_im, w_im);
        }
        if (data_mask_ptr) {
          const int data_mask_hw_ptr =
              ((i * kernel_w + j) * height_col + h_col) * width_col + w_col;
          const MT mask = static_cast<MT>(data_mask_ptr[data_mask_hw_ptr]);
          val *= mask;
        }
        *data_col_ptr = static_cast<T>(val);
        data_col_ptr += batch_size * height_col * width_col;
      }
    }
  }
}

template <typename T, typename Context>
void ModulatedDeformableIm2col(const Context& dev_ctx,
                               const T* data_im,
                               const T* data_offset,
                               const T* data_mask,
                               const std::vector<int64_t>& im_shape,
                               const std::vector<int64_t>& col_shape,
                               const std::vector<int64_t>& filter_shape,
                               const std::vector<int>& paddings,
                               const std::vector<int>& strides,
                               const std::vector<int>& dilations,
                               const int deformable_groups,
                               T* data_col) {
  int channel_per_deformable_group = im_shape[0] / deformable_groups;
  int num_kernels = im_shape[0] * col_shape[1] * col_shape[2] * col_shape[3];

  int blocks = NumBlocks(num_kernels);
  int threads = kNumCUDAThreads;

  ModulatedDeformableIm2colGpuKernel<T>
      <<<blocks, threads, 0, dev_ctx.stream()>>>(num_kernels,
                                                 data_im,
                                                 data_offset,
                                                 data_mask,
                                                 im_shape[1],
                                                 im_shape[2],
                                                 filter_shape[2],
                                                 filter_shape[3],
                                                 paddings[0],
                                                 paddings[1],
                                                 strides[0],
                                                 strides[1],
                                                 dilations[0],
                                                 dilations[1],
                                                 channel_per_deformable_group,
                                                 col_shape[1],
                                                 im_shape[0],
                                                 deformable_groups,
                                                 col_shape[2],
                                                 col_shape[3],
                                                 data_col);
}

template void ModulatedDeformableIm2col(
    const phi::GPUContext& dev_ctx,
    const float* data_im,
    const float* data_offset,
    const float* data_mask,
    const std::vector<int64_t>& im_shape,
    const std::vector<int64_t>& col_shape,
    const std::vector<int64_t>& filter_shape,
    const std::vector<int>& paddings,
    const std::vector<int>& strides,
    const std::vector<int>& dilations,
    const int deformable_groups,
    float* data_col);

template void ModulatedDeformableIm2col(
    const phi::GPUContext& dev_ctx,
    const phi::dtype::float16* data_im,
    const phi::dtype::float16* data_offset,
    const phi::dtype::float16* data_mask,
    const std::vector<int64_t>& im_shape,
    const std::vector<int64_t>& col_shape,
    const std::vector<int64_t>& filter_shape,
    const std::vector<int>& paddings,
    const std::vector<int>& strides,
    const std::vector<int>& dilations,
    const int deformable_groups,
    phi::dtype::float16* data_col);

template void ModulatedDeformableIm2col(
    const phi::GPUContext& dev_ctx,
    const double* data_im,
    const double* data_offset,
    const double* data_mask,
    const std::vector<int64_t>& im_shape,
    const std::vector<int64_t>& col_shape,
    const std::vector<int64_t>& filter_shape,
    const std::vector<int>& paddings,
    const std::vector<int>& strides,
    const std::vector<int>& dilations,
    const int deformable_groups,
    double* data_col);

}  // namespace funcs
}  // namespace phi
