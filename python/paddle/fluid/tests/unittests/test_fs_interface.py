# Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
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
import inspect

from paddle.distributed.fleet.utils.fs import FS


class FSTest(unittest.TestCase):

    def _test_method(self, func):
        args = inspect.getfullargspec(func).args

        a = None
        try:
            if len(args) == 1:
                func()
            elif len(args) == 2:
                func(a)
            elif len(args) == 3:
                func(a, a)
            elif len(args) == 5:
                func(a, a, a, a)
            print("args:", args, len(args), "func:", func)
            self.assertFalse(True)
        except NotImplementedError as e:
            pass

    def test(self):
        fs = FS()
        for name, func in inspect.getmembers(fs, predicate=inspect.ismethod):
            self._test_method(func)


if __name__ == '__main__':
    unittest.main()
