/* Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
// disable numpy compile error

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#include <Python.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "paddle/fluid/eager/api/all.h"
#include "paddle/fluid/eager/grad_node_info.h"
#include "paddle/fluid/eager/hooks.h"
#include "paddle/fluid/eager/utils.h"
#include "paddle/fluid/framework/convert_utils.h"
#include "paddle/fluid/memory/allocation/allocator.h"
#include "paddle/fluid/memory/memcpy.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/pybind/eager.h"
#include "paddle/fluid/pybind/eager_utils.h"
#include "paddle/fluid/pybind/exception.h"
#include "paddle/phi/api/include/api.h"
#include "paddle/phi/common/data_type.h"
#include "paddle/phi/core/compat/convert_utils.h"
#include "paddle/phi/core/dense_tensor.h"
#include "pybind11/detail/internals.h"
#include "pybind11/numpy.h"
#include "pybind11/pybind11.h"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "paddle/fluid/eager/api/generated/eager_generated/forwards/dygraph_functions.h"
#include "paddle/fluid/framework/data_type.h"
#include "paddle/fluid/framework/python_headers.h"
#include "paddle/fluid/memory/allocation/mmap_allocator.h"
#include "paddle/fluid/pybind/op_function_common.h"
#include "paddle/fluid/pybind/tensor_py.h"
#include "paddle/phi/core/ddim.h"
#include "paddle/phi/kernels/funcs/math_function.h"

namespace paddle {
namespace pybind {

extern PyTypeObject* p_tensor_type;

bool PyCheckTensor(PyObject* obj) {
  return PyObject_IsInstance(obj, reinterpret_cast<PyObject*>(p_tensor_type));
}

static bool PyCheckInteger(PyObject* obj) {
#if PY_VERSION_HEX < 0x03000000
  return (PyLong_Check(obj) || PyInt_Check(obj)) && !PyBool_Check(obj);
#else
  return PyLong_Check(obj) && !PyBool_Check(obj);
#endif
}

static bool IsNumpyType(PyObject* obj) {
  // It is not a good way to judge the type of obj by its type'name. Maybe using
  // `PyArray_IsScalar` will be better. However, this interface cannot be used
  // by including pybind11, and it needs to compile with numpy.
  auto type_name = std::string(Py_TYPE(obj)->tp_name);
  return type_name == "numpy.int64" || type_name == "numpy.longlong" ||
         type_name == "numpy.int32" || type_name == "numpy.int16";
}

std::set<phi::DataType> _supported_int_dtype_{DataType::UINT8,
                                              DataType::INT8,
                                              DataType::INT16,
                                              DataType::INT32,
                                              DataType::INT64,
                                              DataType::BOOL};
std::set<phi::DataType> _complex_dtypes{
    DataType::COMPLEX64,
    DataType::COMPLEX128,
};

// _supported_promote_complex_types_
//     '__add__',
//     '__radd__',
//     '__sub__',
//     '__rsub__',
//     '__mul__',
//     '__rmul__',
//     '__div__',
//     '__truediv__',
//     '__rdiv__',
//     '__rtruediv__',
//     '__matmul__',

void SetDevice(paddle::platform::Place place) {
  if (paddle::platform::is_gpu_place(place)) {
#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
    phi::backends::gpu::SetDeviceId(place.device);
    VLOG(6) << "CurrentDeviceId: " << phi::backends::gpu::GetCurrentDeviceId()
            << " from " << static_cast<int>(place.device);
#else
    PADDLE_THROW(paddle::platform::errors::PreconditionNotMet(
        "PaddlePaddle should compile with GPU if use CUDAPlace."));
#endif
  }

  if (paddle::platform::is_custom_place(place)) {
#if defined(PADDLE_WITH_CUSTOM_DEVICE)
    phi::DeviceManager::SetDevice(place);
    VLOG(6) << "CurrentDeviceId: "
            << phi::DeviceManager::GetDevice(place.GetDeviceType()) << " from "
            << static_cast<int>(place.device);
#else
    PADDLE_THROW(paddle::platform::errors::PreconditionNotMet(
        "PaddlePaddle should compile with CUSTOM_DEVICE if use "
        "CustomPlace."));
#endif
  }
}

// scalar func only support add, radd, sub, rsub, mul, rmul, div, truediv.
// this function will update gradually.
paddle::experimental::Tensor CallScalarFuction(
    const paddle::experimental::Tensor& self_tensor,
    float other,
    std::string op_type) {
  paddle::experimental::Tensor ret;
  if (op_type == "add" || op_type == "radd") {
    ret = scale_ad_func(self_tensor, phi::Scalar(1.0), other, true);
  } else if (op_type == "sub") {
    ret = scale_ad_func(self_tensor, phi::Scalar(1.0), -other, true);

  } else if (op_type == "rsub") {
    ret = scale_ad_func(self_tensor, phi::Scalar(-1.0), other, true);
  } else if (op_type == "mul") {
    ret = scale_ad_func(self_tensor, phi::Scalar(other), 0.0, true);
  } else if (op_type == "div") {
    ret = scale_ad_func(self_tensor, phi::Scalar(1.0 / other), 0.0, true);
  }

  return ret;
}

static PyObject* tensor__add__method(TensorObject* self,
                                     PyObject* args,
                                     PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__add__ or __radd_ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(6) << "Running Eager tensor__add__method";
  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;
  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    float other = 0.0;
    if (PyFloat_Check(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
    }

    {
      eager_gil_scoped_release guard;
      ret = CallScalarFuction(self_tensor, other, "add");
    }
    return ToPyObject(ret);
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__add__", 0);
    {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    // note: only op_type in _supported_promote_complex_types_ should promote
    // dtype
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }

  // 4. calculation
  VLOG(6) << "Calling add_ad_func in tensor__add__method";

  {
    eager_gil_scoped_release guard;
    ret = add_ad_func(self_tensor, other_tensor);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__sub__method(TensorObject* self,
                                     PyObject* args,
                                     PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__sub__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(6) << "Running Eager tensor__sub__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;

  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);
  // 1. scalar exists cases
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    float other = 0.0;
    if (PyFloat_Check(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
    }
    {
      eager_gil_scoped_release guard;
      ret = CallScalarFuction(self_tensor, other, "sub");
    }

    return ToPyObject(ret);
  }
  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__sub__", 0);
    {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }
  // 4. calculation
  VLOG(6) << "Calling subtract_ad_func in tensor__sub__method";
  {
    eager_gil_scoped_release guard;
    ret = subtract_ad_func(self_tensor, other_tensor);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__rsub__method(TensorObject* self,
                                      PyObject* args,
                                      PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__rsub__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(1) << "Running Eager tensor__rsub__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;
  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    float other = 0.0;
    if (PyFloat_Check(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
    }
    {
      eager_gil_scoped_release guard;
      ret = CallScalarFuction(self_tensor, other, "rsub");
    }
    return ToPyObject(ret);
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__rsub__", 0);
    {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }

  // 4. calculation
  VLOG(6) << "Calling subtract_ad_func in tensor__rsub__method";
  {
    eager_gil_scoped_release guard;
    ret = subtract_ad_func(other_tensor, self_tensor);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__mul__method(TensorObject* self,
                                     PyObject* args,
                                     PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__mul__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(6) << "Running Eager tensor__mul__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;

  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    float other = 0.0;
    if (PyFloat_Check(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
    }
    {
      eager_gil_scoped_release guard;
      ret = CallScalarFuction(self_tensor, other, "mul");
    }
    return ToPyObject(ret);
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__mul__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    // note: only op_type in _supported_promote_complex_types_ should promote
    // dtype
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }

  // 4. calculation
  VLOG(6) << "Calling multiply_ad_func in tensor__mul__method";
  {
    eager_gil_scoped_release guard;
    ret = multiply_ad_func(self_tensor, other_tensor);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__div__method(TensorObject* self,
                                     PyObject* args,
                                     PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__div__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY

  VLOG(6) << "Running Eager tensor__div__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;

  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    float other = 0.0;
    if (PyFloat_Check(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other = CastPyArg2AttrFloat(other_obj, 0);
    }
    if (_supported_int_dtype_.find(self_tensor.dtype()) !=
        _supported_int_dtype_.end()) {
      eager_gil_scoped_release guard;
      self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
    }
    {
      eager_gil_scoped_release guard;
      ret = CallScalarFuction(self_tensor, other, "div");
    }
    return ToPyObject(ret);
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__div__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    // note: only op_type in _supported_promote_complex_types_ should promote
    // dtype
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }
  if (_supported_int_dtype_.find(self_tensor.dtype()) !=
      _supported_int_dtype_.end()) {
    eager_gil_scoped_release guard;
    self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
  }
  if (_supported_int_dtype_.find(other_tensor.dtype()) !=
      _supported_int_dtype_.end()) {
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, DataType::FLOAT32);
  }

  // 4. calculation
  VLOG(6) << "Calling divide_ad_func in tensor__div__method";
  {
    eager_gil_scoped_release guard;
    ret = divide_ad_func(self_tensor, other_tensor);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__rdiv__method(TensorObject* self,
                                      PyObject* args,
                                      PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__rdiv__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);
  EAGER_TRY

  VLOG(6) << "Running Eager tensor__rdiv__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;

  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar_div function for __rdiv__ and __rtruediv__
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
    if (_supported_int_dtype_.find(self_tensor.dtype()) !=
        _supported_int_dtype_.end()) {
      eager_gil_scoped_release guard;
      self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor = full_ad_func(self_tensor.shape(),
                                phi::Scalar(other_float),
                                self_tensor.dtype(),
                                place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__rdiv__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    // note: only op_type in _supported_promote_complex_types_ should promote
    // dtype
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }
  if (_supported_int_dtype_.find(self_tensor.dtype()) !=
      _supported_int_dtype_.end()) {
    eager_gil_scoped_release guard;
    self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
  }
  if (_supported_int_dtype_.find(other_tensor.dtype()) !=
      _supported_int_dtype_.end()) {
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, DataType::FLOAT32);
  }

  // 4. calculation
  VLOG(6) << "Calling divide_ad_func in tensor__rdiv__method";
  {
    eager_gil_scoped_release guard;
    ret = divide_ad_func(other_tensor, self_tensor);
  }
  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__gt__method(TensorObject* self,
                                    PyObject* args,
                                    PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__gt__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(1) << "Running Eager tensor__gt__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;
  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar function for __gt__ now
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor = full_ad_func(self_tensor.shape(),
                                phi::Scalar(other_float),
                                self_tensor.dtype(),
                                place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__gt__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    VLOG(6) << "The dtype of left and right Tensor are not the same, left "
               "dtype is "
            << lhs_dtype << ", but right dtype is " << rhs_dtype
            << ", the right dtype will convert to " << lhs_dtype;
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, lhs_dtype);
  }

  // 4. calculation
  VLOG(6) << "Calling greater_than_ad_func in tensor__gt__method";
  {
    eager_gil_scoped_release guard;
    ret = greater_than_ad_func(self_tensor, other_tensor, -1);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__ge__method(TensorObject* self,
                                    PyObject* args,
                                    PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__ge__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(1) << "Running Eager tensor__ge__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;
  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar function for __ge__ now
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor = full_ad_func(self_tensor.shape(),
                                phi::Scalar(other_float),
                                self_tensor.dtype(),
                                place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__ge__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    VLOG(6) << "The dtype of left and right Tensor are not the same, left "
               "dtype is "
            << lhs_dtype << ", but right dtype is " << rhs_dtype
            << ", the right dtype will convert to " << lhs_dtype;
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, lhs_dtype);
  }

  // 4. calculation
  VLOG(6) << "Calling greater_equal_ad_func in tensor__ge__method";
  {
    eager_gil_scoped_release guard;
    ret = greater_equal_ad_func(self_tensor, other_tensor, -1);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__mod__method(TensorObject* self,
                                     PyObject* args,
                                     PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__mod__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);
  EAGER_TRY

  VLOG(6) << "Running Eager tensor__mod__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;

  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar_mod function for __mod__ now
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor = full_ad_func(self_tensor.shape(),
                                phi::Scalar(other_float),
                                self_tensor.dtype(),
                                place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__mod__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    VLOG(6) << "The  dtype of left and right Tensor are not the same, left "
               "dtype is "
            << lhs_dtype << ", but right dtype is " << rhs_dtype
            << ", the right dtype will convert to " << lhs_dtype;
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, lhs_dtype);
  }

  // 4. calculation
  VLOG(6) << "Calling remainder_ad_func in tensor__mod__method";
  {
    eager_gil_scoped_release guard;
    ret = remainder_ad_func(self_tensor, other_tensor);
  }
  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__matmul__method(TensorObject* self,
                                        PyObject* args,
                                        PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__matmul__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);
  EAGER_TRY

  VLOG(6) << "Running Eager tensor__matmul__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;

  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar_matmul function for __matmul__ now
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor =
        full_ad_func({1}, phi::Scalar(other_float), self_tensor.dtype(), place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__matmul__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    // note: only op_type in _supported_promote_complex_types_ should promote
    // dtype
    if (_complex_dtypes.find(lhs_dtype) != _complex_dtypes.end() ||
        _complex_dtypes.find(rhs_dtype) != _complex_dtypes.end()) {
      phi::DataType promote_dtype =
          framework::TransToPhiDataType(framework::PromoteTypesIfComplexExists(
              framework::TransToProtoVarType(lhs_dtype),
              framework::TransToProtoVarType(rhs_dtype)));
      if (lhs_dtype != promote_dtype) {
        // cast
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, promote_dtype);
      }
      if (rhs_dtype != promote_dtype) {
        eager_gil_scoped_release guard;
        other_tensor = cast_ad_func(other_tensor, promote_dtype);
      }
    } else {
      VLOG(6) << "The dtype of left and right Tensor are not the same, left "
                 "dtype is "
              << lhs_dtype << ", but right dtype is " << rhs_dtype
              << ", the right dtype will convert to " << lhs_dtype;
      eager_gil_scoped_release guard;
      other_tensor = cast_ad_func(other_tensor, lhs_dtype);
    }
  }

  // 4. calculation
  VLOG(6) << "Calling matmul_ad_func in tensor__matmul__method";
  {
    eager_gil_scoped_release guard;
    ret = matmul_ad_func(self_tensor, other_tensor, false, false);
  }
  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__lt__method(TensorObject* self,
                                    PyObject* args,
                                    PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__lt__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(1) << "Running Eager tensor__lt__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;
  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar function for __lt__ now
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor = full_ad_func(self_tensor.shape(),
                                phi::Scalar(other_float),
                                self_tensor.dtype(),
                                place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__lt__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    VLOG(6) << "The dtype of left and right Tensor are not the same, left "
               "dtype is "
            << lhs_dtype << ", but right dtype is " << rhs_dtype
            << ", the right dtype will convert to " << lhs_dtype;
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, lhs_dtype);
  }

  // 4. calculation
  VLOG(6) << "Calling less_than_ad_func in tensor__lt__method";
  {
    eager_gil_scoped_release guard;
    ret = less_than_ad_func(self_tensor, other_tensor, -1);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

static PyObject* tensor__le__method(TensorObject* self,
                                    PyObject* args,
                                    PyObject* kwargs) {
  paddle::platform::RecordEvent pythonc_record_event(
      "__le__ pybind_patch_func",
      paddle::platform::TracerEventType::UserDefined,
      1);

  EAGER_TRY
  VLOG(1) << "Running Eager tensor__le__method";

  // Set Device ID
  auto place = egr::Controller::Instance().GetExpectedPlace();
  SetDevice(place);

  paddle::experimental::Tensor ret;
  paddle::experimental::Tensor self_tensor = self->tensor;
  PyObject* other_obj = PyTuple_GET_ITEM(args, 0);

  // 1. scalar exists cases
  // there is no scalar function for __le__ now
  float other_float = 0.0;
  bool has_other_float = false;
  if (PyFloat_Check(other_obj) || PyCheckInteger(other_obj) ||
      IsNumpyType(other_obj)) {
    if (PyFloat_Check(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
      if (_supported_int_dtype_.find(self_tensor.dtype()) !=
          _supported_int_dtype_.end()) {
        eager_gil_scoped_release guard;
        self_tensor = cast_ad_func(self_tensor, DataType::FLOAT32);
      }
    } else if (PyCheckInteger(other_obj) || IsNumpyType(other_obj)) {
      other_float = CastPyArg2AttrFloat(other_obj, 0);
      has_other_float = true;
    }
  }

  // 2. create or get tensor for other_obj
  paddle::experimental::Tensor other_tensor;
  if (has_other_float) {
    eager_gil_scoped_release guard;
    other_tensor = full_ad_func(self_tensor.shape(),
                                phi::Scalar(other_float),
                                self_tensor.dtype(),
                                place);
  } else if (!PyCheckTensor(other_obj)) {
    paddle::experimental::Scalar value =
        CastPyArg2Scalar(other_obj, "__le__", 0);
    if (PyComplex_Check(other_obj)) {
      eager_gil_scoped_release guard;
      other_tensor = full_ad_func({1}, value, DataType::COMPLEX64, place);
    } else {
      eager_gil_scoped_release guard;
      other_tensor =
          full_ad_func(self_tensor.shape(), value, self_tensor.dtype(), place);
    }
  } else {
    other_tensor = CastPyArg2Tensor(other_obj, 0);
  }

  // 3. promote types or unify right var type to left var
  phi::DataType lhs_dtype = self_tensor.dtype();
  phi::DataType rhs_dtype = other_tensor.dtype();
  if (lhs_dtype != rhs_dtype) {
    VLOG(6) << "The dtype of left and right Tensor are not the same, left "
               "dtype is "
            << lhs_dtype << ", but right dtype is " << rhs_dtype
            << ", the right dtype will convert to " << lhs_dtype;
    eager_gil_scoped_release guard;
    other_tensor = cast_ad_func(other_tensor, lhs_dtype);
  }

  // 4. calculation
  VLOG(6) << "Calling less_equal_ad_func in tensor__le__method";
  {
    eager_gil_scoped_release guard;
    ret = less_equal_ad_func(self_tensor, other_tensor, -1);
  }

  return ToPyObject(ret);
  EAGER_CATCH_AND_THROW_RETURN_NULL
}

PyMethodDef math_op_patch_methods[] = {
    {"__add__",
     (PyCFunction)(void (*)(void))tensor__add__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__radd__",
     (PyCFunction)(void (*)(void))tensor__add__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__sub__",
     (PyCFunction)(void (*)(void))tensor__sub__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__rsub__",
     (PyCFunction)(void (*)(void))tensor__rsub__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__mul__",
     (PyCFunction)(void (*)(void))tensor__mul__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__rmul__",
     (PyCFunction)(void (*)(void))tensor__mul__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__div__",
     (PyCFunction)(void (*)(void))tensor__div__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__truediv__",
     (PyCFunction)(void (*)(void))tensor__div__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__rdiv__",
     (PyCFunction)(void (*)(void))tensor__rdiv__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__rtruediv__",
     (PyCFunction)(void (*)(void))tensor__rdiv__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__mod__",
     (PyCFunction)(void (*)(void))tensor__mod__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__matmul__",
     (PyCFunction)(void (*)(void))tensor__matmul__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__gt__",
     (PyCFunction)(void (*)(void))tensor__gt__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__ge__",
     (PyCFunction)(void (*)(void))tensor__ge__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__lt__",
     (PyCFunction)(void (*)(void))tensor__lt__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"__le__",
     (PyCFunction)(void (*)(void))tensor__le__method,
     METH_VARARGS | METH_KEYWORDS,
     NULL},
    {NULL, NULL, 0, NULL}};

}  // namespace pybind
}  // namespace paddle
