#   Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
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

import paddle
import unittest
import numpy as np
import paddle.fluid as fluid
from op_test import OpTest
from paddle.fluid.framework import _test_eager_guard

paddle.enable_static()


def dmc_bilinear(data_im, height, width, h, w):
    h_low = int(np.floor(h))
    w_low = int(np.floor(w))
    h_high = h_low + 1
    w_high = w_low + 1

    lh = h - h_low
    lw = w - w_low
    hh = 1 - lh
    hw = 1 - lw

    v1 = 0
    if h_low >= 0 and w_low >= 0:
        v1 = data_im[h_low, w_low]
    v2 = 0
    if h_low >= 0 and w_high <= width - 1:
        v2 = data_im[h_low, w_high]
    v3 = 0
    if h_high <= height - 1 and w_low >= 0:
        v3 = data_im[h_high, w_low]
    v4 = 0
    if h_high <= height - 1 and w_high <= width - 1:
        v4 = data_im[h_high, w_high]

    w1, w2, w3, w4 = hh * hw, hh * lw, lh * hw, lh * lw
    val = w1 * v1 + w2 * v2 + w3 * v3 + w4 * v4

    return val


def dconv_im2col_gemm(input, offset, filter, group, conv_param):
    in_n, in_c, in_h, in_w = input.shape
    out_c, f_c, f_h, f_w = filter.shape

    assert offset.shape == (in_n, 2 * f_h * f_w, in_h, in_w)
    assert f_c * group == in_c
    assert np.mod(out_c, group) == 0

    stride, pad, dilation = conv_param['stride'], conv_param['pad'], \
                            conv_param['dilation']
    out_h = 1 + (in_h + 2 * pad[0] - (dilation[0] * (f_h - 1) + 1)) // stride[0]
    out_w = 1 + (in_w + 2 * pad[1] - (dilation[1] * (f_w - 1) + 1)) // stride[1]
    assert out_h == in_h
    assert out_w == in_w

    col_buffer = np.zeros((in_n, in_c * f_h * f_w, in_h * in_w))
    for n in range(in_n):
        for c in range(in_c):
            for h in range(out_h):
                for w in range(out_w):
                    for kh in range(f_h):
                        for kw in range(f_w):
                            offset_h_table = \
                                offset[n, ::2, h, w].reshape(f_h, f_w)
                            offset_w_table = \
                                offset[n, 1::2, h, w].reshape(f_h, f_w)
                            offset_h = offset_h_table[kh, kw]
                            offset_w = offset_w_table[kh, kw]
                            val = 0
                            im_h = h * stride[0] + kh * dilation[0] \
                                   + offset_h - pad[0]
                            im_w = w * stride[0] + kw * dilation[0] \
                                   + offset_w - pad[1]
                            if im_h > -1 and im_w > -1 and \
                                    im_h < in_h and im_w < in_h:
                                val = dmc_bilinear(input[n, c], in_h, in_w,
                                                   im_h, im_w)
                            val_out = val

                            col_buffer[n, c * f_h * f_w + kh * f_w + kw,
                                       h * in_w + w] = val_out

    out = np.zeros((in_n, group, int(out_c // group), out_h * out_w))
    weight = filter.reshape(group, int(out_c // group), f_c * f_h * f_w)
    col_buffer = col_buffer.reshape(
        (in_n, group, int(in_c // group * f_h * f_w), in_h * in_w))
    for n in range(in_n):
        for g in range(group):
            out[n, g] = np.matmul(weight[g], col_buffer[n, g])
    out = out.reshape(in_n, out_c, out_h, out_w)
    return out


def deform_conv2d_wrapper(x,
                          offset,
                          weight,
                          mask=None,
                          stride=1,
                          padding=0,
                          dilation=1,
                          deformable_groups=1,
                          groups=1,
                          im2col_step=1):
    return paddle.vision.ops.deform_conv2d(x, offset, weight, None, stride,
                                           padding, dilation, deformable_groups,
                                           groups, mask)


class TestModulatedDeformableConvOp(OpTest):

    def setUp(self):
        self.python_api = deform_conv2d_wrapper
        self.op_type = "deformable_conv_v1"
        self.init_type()
        self.init_group()
        self.init_dilation()
        self.init_test_case()

        conv_param = {
            'stride': self.stride,
            'pad': self.pad,
            'dilation': self.dilations
        }

        input = np.random.random(self.input_size).astype(self.dtype)
        offset = 10 * np.random.random(self.offset_size).astype(self.dtype)
        filter = np.random.random(self.filter_size).astype(self.dtype)

        output = dconv_im2col_gemm(input, offset, filter, self.groups,
                                   conv_param)
        output = output.astype(self.dtype)
        self.inputs = {
            'Input': OpTest.np_dtype_to_fluid_dtype(input),
            'Offset': OpTest.np_dtype_to_fluid_dtype(offset),
            'Filter': OpTest.np_dtype_to_fluid_dtype(filter)
        }
        self.attrs = {
            'strides': self.stride,
            'paddings': self.pad,
            'groups': self.groups,
            'deformable_groups': self.deformable_groups,
            'im2col_step': self.im2col_step,
            'dilations': self.dilations,
        }
        self.outputs = {'Output': output}

    def test_check_output(self):
        self.check_output(check_eager=True)

    def test_check_grad(self):
        self.check_grad(['Input', 'Offset', 'Filter'],
                        'Output',
                        max_relative_error=0.05,
                        check_eager=True)

    def test_check_grad_no_filter(self):
        self.check_grad(['Input', 'Offset'],
                        'Output',
                        max_relative_error=0.1,
                        no_grad_set=set(['Filter']),
                        check_eager=True)

    def init_test_case(self):
        self.pad = [1, 1]
        self.stride = [1, 1]
        self.dilations = [1, 1]
        self.input_size = [2, 4, 4, 4]  # NCHW
        assert np.mod(self.input_size[1], self.groups) == 0
        f_c = self.input_size[1] // self.groups
        self.filter_size = [4, f_c, 3, 3]
        self.im2col_step = 1
        self.deformable_groups = 1
        offset_c = 2 * self.deformable_groups * self.filter_size[
            2] * self.filter_size[3]
        self.offset_size = [
            self.input_size[0], offset_c, self.input_size[2], self.input_size[3]
        ]

    def init_dilation(self):
        self.dilations = [1, 1]

    def init_group(self):
        self.groups = 1

    def init_type(self):
        self.dtype = np.float32


class TestWithStride(TestModulatedDeformableConvOp):

    def init_test_case(self):
        self.pad = [3, 3]
        self.stride = [2, 2]
        self.input_size = [2, 3, 5, 5]  # NCHW
        assert np.mod(self.input_size[1], self.groups) == 0
        f_c = self.input_size[1] // self.groups
        self.filter_size = [6, f_c, 3, 3]
        self.im2col_step = 1
        self.deformable_groups = 1
        offset_c = 2 * self.deformable_groups * self.filter_size[
            2] * self.filter_size[3]
        self.offset_size = [
            self.input_size[0], offset_c, self.input_size[2], self.input_size[3]
        ]


class TestWithDilation(TestModulatedDeformableConvOp):

    def init_test_case(self):
        self.pad = [2, 2]
        self.stride = [1, 1]
        self.input_size = [5, 3, 4, 4]  # NCHW
        assert np.mod(self.input_size[1], self.groups) == 0
        f_c = self.input_size[1] // self.groups
        self.filter_size = [6, f_c, 3, 3]
        self.im2col_step = 1
        self.deformable_groups = 1
        offset_c = 2 * self.deformable_groups * self.filter_size[
            2] * self.filter_size[3]
        self.offset_size = [
            self.input_size[0], offset_c, self.input_size[2], self.input_size[3]
        ]

    def init_dilation(self):
        self.dilations = [2, 2]


class TestWith1x1(TestModulatedDeformableConvOp):

    def init_test_case(self):
        self.pad = [0, 0]
        self.stride = [1, 1]
        self.input_size = [2, 3, 5, 5]  # NCHW
        assert np.mod(self.input_size[1], self.groups) == 0
        f_c = self.input_size[1] // self.groups
        self.filter_size = [40, f_c, 1, 1]
        self.im2col_step = 1
        self.deformable_groups = 1
        offset_c = 2 * self.deformable_groups * self.filter_size[
            2] * self.filter_size[3]
        self.offset_size = [
            self.input_size[0], offset_c, self.input_size[2], self.input_size[3]
        ]


class TestWithGroup(TestModulatedDeformableConvOp):

    def init_test_case(self):
        self.pad = [1, 1]
        self.stride = [1, 1]
        self.dilations = [1, 1]
        self.input_size = [2, 8, 4, 4]  # NCHW
        assert np.mod(self.input_size[1], self.groups) == 0
        f_c = self.input_size[1] // self.groups
        self.filter_size = [4, f_c, 3, 3]
        self.im2col_step = 1
        self.deformable_groups = 1
        offset_c = 2 * self.deformable_groups * self.filter_size[
            2] * self.filter_size[3]
        self.offset_size = [
            self.input_size[0], offset_c, self.input_size[2], self.input_size[3]
        ]

    def init_group(self):
        self.groups = 2


class TestWithDouble(TestModulatedDeformableConvOp):

    def init_type(self):
        self.dtype = np.float64


class TestFP16(unittest.TestCase):

    def check_main(self, input_np, offset_np, filter_np, dtype):
        paddle.disable_static()
        input_np = input_np.astype(dtype)
        offset_np = offset_np.astype(dtype)
        filter_np = filter_np.astype(dtype)

        input = paddle.to_tensor(input_np)
        offset = paddle.to_tensor(offset_np)
        filter = paddle.to_tensor(filter_np)

        input.stop_gradient = False
        offset.stop_gradient = False
        filter.stop_gradient = False

        y = paddle.vision.ops.deform_conv2d(input, offset, filter)
        input_grad, offset_grad, filter_grad = paddle.grad(
            y, [input, offset, filter])
        y_np = y.numpy().astype('float32')
        input_grad_np = input_grad.numpy().astype('float32')
        offset_grad_np = offset_grad.numpy().astype('float32')
        filter_grad_np = filter_grad.numpy().astype('float32')
        paddle.enable_static()
        return y_np, input_grad_np, offset_grad_np, filter_grad_np

    def test_main(self):
        if not paddle.is_compiled_with_cuda():
            return
        self.pad = [1, 1]
        self.stride = [1, 1]
        self.dilations = [1, 1]
        self.groups = 1
        self.input_size = [2, 3, 5, 5]  # NCHW
        assert np.mod(self.input_size[1], self.groups) == 0
        f_c = self.input_size[1] // self.groups
        self.filter_size = [40, f_c, 1, 1]
        self.im2col_step = 1
        self.deformable_groups = 1
        offset_c = 2 * self.deformable_groups * self.filter_size[
            2] * self.filter_size[3]
        self.offset_size = [
            self.input_size[0], offset_c, self.input_size[2], self.input_size[3]
        ]

        input = np.random.random(self.input_size)
        offset = 10 * np.random.random(self.offset_size)
        filter = np.random.random(self.filter_size)

        y_np_1, input_g_np_1, offset_g_np_1, filter_g_np_1 = self.check_main(
            input, offset, filter, 'float16')
        y_np_2, input_g_np_2, offset_g_np_2, filter_g_np_2 = self.check_main(
            input, offset, filter, 'float32')

        def assert_equal(x, y):
            np.testing.assert_allclose(x, y, atol=3e-2)

        assert_equal(y_np_1, y_np_2)
        assert_equal(input_g_np_1, input_g_np_2)
        assert_equal(offset_g_np_1, offset_g_np_2)
        assert_equal(filter_g_np_1, filter_g_np_2)


class TestModulatedDeformableConvV1InvalidInput(unittest.TestCase):

    def test_error(self):

        def test_invalid_input():
            input = [1, 3, 32, 32]
            offset = fluid.data(name='offset',
                                shape=[None, 3, 32, 32],
                                dtype='float32')
            loss = fluid.layers.deformable_conv(input,
                                                offset,
                                                mask=None,
                                                num_filters=4,
                                                filter_size=1,
                                                modulated=False)

        self.assertRaises(TypeError, test_invalid_input)

        def test_invalid_offset():
            input = fluid.data(name='input',
                               shape=[None, 3, 32, 32],
                               dtype='int32')
            offset = fluid.data(name='offset',
                                shape=[None, 3, 32, 32],
                                dtype='float32')
            loss = fluid.layers.deformable_conv(input,
                                                offset,
                                                mask=None,
                                                num_filters=4,
                                                filter_size=1,
                                                modulated=False)

        self.assertRaises(TypeError, test_invalid_offset)

    def test_error_with_eager_guard(self):
        with _test_eager_guard():
            self.test_error()


if __name__ == '__main__':
    unittest.main()
