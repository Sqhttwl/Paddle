# Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
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

import unittest
import numpy as np
import paddle
import os
import sys

sys.path.append("..")

from paddle.fluid.tests.unittests.op_test import OpTest, _set_use_system_allocator

from test_sync_batch_norm_base_npu import TestDistBase

_set_use_system_allocator(False)
paddle.enable_static()


class TestSyncBatchNormOp(TestDistBase):

    def _setup_config(self):
        pass

    def test_identity(self, col_type="identity"):
        dist_env = os.environ
        self.check_with_place("sync_batch_norm_op_npu.py",
                              col_type,
                              need_envs=dist_env)


if __name__ == '__main__':
    unittest.main()
