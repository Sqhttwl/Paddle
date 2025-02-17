# Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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

import os
import unittest
from test_dist_fleet_base import TestFleetBase


class TestDistMnistAsyncInMemoryDataset2x2(TestFleetBase):

    def _setup_config(self):
        self._mode = "async"
        #self._reader = "pyreader"
        self._reader = "dataset"

    def check_with_place(self,
                         model_file,
                         delta=1e-3,
                         check_error_log=False,
                         need_envs={}):
        required_envs = {
            "PATH": os.getenv("PATH", ""),
            "PYTHONPATH": os.getenv("PYTHONPATH", ""),
            "LD_LIBRARY_PATH": os.getenv("LD_LIBRARY_PATH", ""),
            "FLAGS_rpc_deadline": "5000",  # 5sec to fail fast
            "http_proxy": "",
            "CPU_NUM": "2",
            "LOG_DIRNAME": "/tmp",
            "SAVE_DIRNAME": "/tmp/TestDistMnistAsyncInMemoryDataset2x2/model",
            "SAVE_CACHE_DIRNAME":
            "/tmp/TestDistMnistAsyncInMemoryDataset2x2/cache_model",
            "SAVE_DENSE_PARAM_DIRNAME":
            "/tmp/TestDistMnistAsyncInMemoryDataset2x2/dense_param",
            "SAVE_ONE_TABLE_DIRNAME":
            "/tmp/TestDistMnistAsyncInMemoryDataset2x2/table_0",
            "SAVE_PATCH_DIRNAME":
            "/tmp/TestDistMnistAsyncInMemoryDataset2x2/patch_model",
            "LOG_PREFIX": self.__class__.__name__,
        }

        required_envs.update(need_envs)

        if check_error_log:
            required_envs["GLOG_v"] = "3"
            required_envs["GLOG_logtostderr"] = "1"

        tr0_losses, tr1_losses = self._run_cluster(model_file, required_envs)

    def test_dist_train(self):
        self.check_with_place("dist_fleet_ctr.py",
                              delta=1e-5,
                              check_error_log=False)


class TestDistMnistAsync2x2(TestFleetBase):

    def _setup_config(self):
        self._mode = "async"
        self._reader = "pyreader"

    def check_with_place(self,
                         model_file,
                         delta=1e-3,
                         check_error_log=False,
                         need_envs={}):
        required_envs = {
            "PATH": os.getenv("PATH", ""),
            "PYTHONPATH": os.getenv("PYTHONPATH", ""),
            "LD_LIBRARY_PATH": os.getenv("LD_LIBRARY_PATH", ""),
            "FLAGS_rpc_deadline": "5000",  # 5sec to fail fast
            "http_proxy": "",
            "CPU_NUM": "2",
            "LOG_DIRNAME": "/tmp",
            "LOG_PREFIX": self.__class__.__name__,
        }

        required_envs.update(need_envs)

        if check_error_log:
            required_envs["GLOG_v"] = "3"
            required_envs["GLOG_logtostderr"] = "1"

        tr0_losses, tr1_losses = self._run_cluster(model_file, required_envs)

    def test_dist_train(self):
        self.check_with_place("dist_fleet_ctr.py",
                              delta=1e-5,
                              check_error_log=False)


class TestDistCtrHalfAsync2x2(TestFleetBase):

    def _setup_config(self):
        self._mode = "async"
        self._reader = "pyreader"

    def check_with_place(self,
                         model_file,
                         delta=1e-3,
                         check_error_log=False,
                         need_envs={}):
        required_envs = {
            "PATH": os.getenv("PATH", ""),
            "PYTHONPATH": os.getenv("PYTHONPATH", ""),
            "LD_LIBRARY_PATH": os.getenv("LD_LIBRARY_PATH", ""),
            "FLAGS_rpc_deadline": "30000",  # 5sec to fail fast
            "http_proxy": "",
            "FLAGS_communicator_send_queue_size": "2",
            "FLAGS_communicator_max_merge_var_num": "2",
            "CPU_NUM": "2",
            "SAVE_MODEL": "0",
            "LOG_DIRNAME": "/tmp",
            "LOG_PREFIX": self.__class__.__name__,
        }

        required_envs.update(need_envs)

        if check_error_log:
            required_envs["GLOG_v"] = "3"
            required_envs["GLOG_logtostderr"] = "1"

        tr0_losses, tr1_losses = self._run_cluster(model_file, required_envs)

    def test_dist_train(self):
        self.check_with_place("dist_fleet_ctr.py",
                              delta=1e-5,
                              check_error_log=False)


if __name__ == "__main__":
    unittest.main()
