/*******************************************************************************
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/edgeset.h"
#include "tensorflow/core/lib/core/errors.h"

#include "ngraph/op/util/attr_types.hpp"
#include "ngraph/op/util/logical_reduction.hpp"
#include "ngraph/pass/constant_folding.hpp"
#include "ngraph/pass/manager.hpp"
#include "ngraph/pass/pass_config.hpp"
#include "ngraph/slice_plan.hpp"

#include "api.h"
#include "logging/ovtf_log.h"
#include "openvino_tensorflow/backend_manager.h"
#include "openvino_tensorflow/default_opset.h"
#include "openvino_tensorflow/layout_conversions.h"
#include "openvino_tensorflow/mark_for_clustering.h"
#include "openvino_tensorflow/ovtf_builder.h"
#include "openvino_tensorflow/ovtf_utils.h"
#include "openvino_tensorflow/pass/transpose_sinking.h"

using tensorflow::int32;
using namespace std;
namespace ng = ngraph;

namespace tensorflow {
namespace openvino_tensorflow {

static bool VecStrCmp(const std::vector<string>& a,
                      const std::vector<string>& b) {
  return a == b;
}

static Status ValidateInputCount(const Node* op, tensorflow::int32 count) {
  if (op->num_inputs() != count) {
    return errors::InvalidArgument("\"", op->name(), "\" requires ", count,
                                   " input(s), got ", op->num_inputs(),
                                   " instead");
  }
  return Status::OK();
}

static Status ValidateInputCountMin(const Node* op, tensorflow::int32 count) {
  if (op->num_inputs() < count) {
    return errors::InvalidArgument("\"", op->name(), "\" requires at least ",
                                   count, " input(s), got ", op->num_inputs(),
                                   " instead");
  }
  return Status::OK();
}

// Check to make sure the axis dimension for reduction are in within range.
// Returns error if axis is out of range. Otherwise returns Status::OK().
static Status CheckAxisDimInRange(std::vector<int64> axes, size_t rank) {
  for (auto i : axes) {
    if (i < (int)-rank || i >= (int)rank) {
      return errors::InvalidArgument("Axis Dimension is out of range. Got ", i,
                                     ", should be in range [-", rank, ", ",
                                     rank, ")");
    }
  }
  return Status::OK();
}

//
// Helper for storing ops in ng_op_map.
// For most of the cases, op would have one output so
// vector ng_op_map[op_name] would contain one element.
//
// If storing more than one output_nodes, make sure it's in
// the same order as tensorflow would do that.
//
// Parameters:
//    Builder::OpMap& ng_op_map        - The TF-to-nGraph op map.
//    std::string op_name              - Name of the op.
//
//    ng::Output<ng::Node> output_node - ng::Node to store
//

static void SaveNgOp(Builder::OpMap& ng_op_map, const std::string& op_name,
                     ng::Output<ng::Node> output_node) {
  // no need to try-catch, map[key] will create vector object
  // if not exists
  ng_op_map[op_name].push_back(output_node);
}

void Builder::SetTracingInfo(const std::string& op_name,
                             const ng::Output<ng::Node> ng_node) {
  auto node = ng_node.get_node_shared_ptr();
  node->set_friendly_name(op_name + "/" + node->get_name());
  node->add_provenance_tag(op_name);
  if (api::IsLoggingPlacement()) {
    cout << "TF_to_NG: " << op_name << " --> " << node << endl;
  }
}

template <class TOpType, class... TArg>
ng::Output<ng::Node> ConstructNgNode(const std::string& op_name,
                                     TArg&&... Args) {
  auto ng_node = std::make_shared<TOpType>(std::forward<TArg>(Args)...);
  Builder::SetTracingInfo(op_name, ng_node);
  return ng_node;
}

// Helper for fetching correct input node from ng_op_map.
// Handles edge checking to make sure correct input node is
// fetched.
//
// Reduces some boilerplate code (incorrect from now) like this:
//
//      Node* tf_input;
//      TF_RETURN_IF_ERROR(op->input_node(0, &tf_input));
//
//      ng::Output<ng::Node> ng_input;
//      try {
//        ng_input = ng_op_map.at(tf_input->name());
//      } catch (const std::out_of_range&) {
//        return errors::NotFound(tf_input->name(),
//                                    " is not found in the ng_op_map");
//      }
//
// Into 2 lines:
//
//      ng::Output<ng::node> ng_input;
//      TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, &ng_input))
//
//
//
// Parameters:
//    Builder::OpMap& ng_op_map     - The TF-to-nGraph op map.
//    Node* op                  - TF op being translated.
//    input_idx                     - index of input
//
//    ng::Output<ng::Node> *result  - ng::Node pointer where result
//                                    will be written
//
//

static Status GetInputNode(const Builder::OpMap& ng_op_map, const Node* op,
                           size_t input_idx, ng::Output<ng::Node>& result) {
  // input op may have resulted in more than one ng::Node (eg. Split)
  // we need to look at Edge to check index of the input op
  std::vector<const Edge*> edges;
  TF_RETURN_IF_ERROR(op->input_edges(&edges));
  size_t src_output_idx;
  try {
    src_output_idx = edges.at(input_idx)->src_output();
  } catch (const out_of_range&) {
    return Status(error::NOT_FOUND, "Edge not found");
  }

  Node* tf_input;
  TF_RETURN_IF_ERROR(op->input_node(input_idx, &tf_input));
  std::vector<ng::Output<ng::Node>> ng_op;
  try {
    ng_op = ng_op_map.at(tf_input->name());
  } catch (const out_of_range&) {
    return Status(error::NOT_FOUND,
                  string("Ngraph op not found for ") + tf_input->name());
  }
  try {
    result = ng_op.at(src_output_idx);
  } catch (const out_of_range&) {
    return Status(error::NOT_FOUND, string("Input node not found at index ") +
                                        to_string(src_output_idx));
  }
  return Status::OK();
}

namespace detail {
static Status GetInputNodes(const Builder::OpMap&, const Node*, size_t) {
  return Status::OK();
}

template <typename... Arguments>
static Status GetInputNodes(const Builder::OpMap& ng_op_map, const Node* op,
                            size_t index, ng::Output<ng::Node>& result,
                            Arguments&... remaining) {
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, index, result));
  return GetInputNodes(ng_op_map, op, index + 1, remaining...);
}
}  // namespace detail

template <typename... Arguments>
static Status GetInputNodes(const Builder::OpMap& ng_op_map, const Node* op,
                            Arguments&... remaining) {
  constexpr size_t args_len = sizeof...(Arguments);
  TF_RETURN_IF_ERROR(ValidateInputCount(op, args_len));
  return detail::GetInputNodes(ng_op_map, op, 0, remaining...);
}

static Status GetStaticNodeTensor(
    const Node* node, const std::vector<const Tensor*>& static_input_map,
    Tensor* result) {
  if (node->IsArg()) {
    int arg_index;
    TF_RETURN_IF_ERROR(GetNodeAttr(node->attrs(), "index", &arg_index));
    const Tensor* source_tensor = static_input_map[arg_index];
    if (source_tensor == nullptr) {
      return errors::Internal(
          "GetStaticNodeTensor called on _Arg but input tensor is missing from "
          "static input map");
    }
    *result = *source_tensor;
    return Status::OK();
  } else if (node->type_string() == "Const") {
    if (!result->FromProto(node->def().attr().at("value").tensor())) {
      return errors::Internal(
          "GetStaticNodeTensor: Const tensor proto parsing failed");
    }
    return Status::OK();
  } else {
    return errors::Internal("GetStaticNodeTensor called on node with type ",
                            node->type_string(), "; _Arg or Const expected");
  }
}

template <typename Ttensor, typename Tvector>
static void ConvertTensorDataToVector(const Tensor& tensor,
                                      std::vector<Tvector>* vector) {
  const Ttensor* data = tensor.flat<Ttensor>().data();
  vector->resize(tensor.NumElements());
  for (int64 i = 0; i < tensor.NumElements(); i++) {
    (*vector)[i] = Tvector(data[i]);
  }
}

template <typename T>
static Status TensorDataToVector(const Tensor& tensor, std::vector<T>* vector) {
  DataType dt = tensor.dtype();

  // If dt and T match, we can just copy.
  if (dt == DataTypeToEnum<T>::value) {
    *vector = std::vector<T>(tensor.flat<T>().data(),
                             tensor.flat<T>().data() + tensor.NumElements());
  }
  // Else we have to convert.
  else {
    switch (dt) {
      case DT_FLOAT:
        ConvertTensorDataToVector<float, T>(tensor, vector);
        break;
      case DT_DOUBLE:
        ConvertTensorDataToVector<double, T>(tensor, vector);
        break;
      case DT_INT8:
        ConvertTensorDataToVector<int8, T>(tensor, vector);
        break;
      case DT_INT16:
        ConvertTensorDataToVector<int16, T>(tensor, vector);
        break;
      case DT_INT32:
        ConvertTensorDataToVector<int32, T>(tensor, vector);
        break;
      case DT_INT64:
        ConvertTensorDataToVector<int64, T>(tensor, vector);
        break;
      case DT_UINT8:
        ConvertTensorDataToVector<uint8, T>(tensor, vector);
        break;
      case DT_UINT16:
        ConvertTensorDataToVector<uint16, T>(tensor, vector);
        break;
      case DT_UINT32:
        ConvertTensorDataToVector<uint32, T>(tensor, vector);
        break;
      case DT_UINT64:
        ConvertTensorDataToVector<uint64, T>(tensor, vector);
        break;
      case DT_BOOL:
        ConvertTensorDataToVector<bool, T>(tensor, vector);
        break;
      default:
        return errors::Internal("TensorDataToVector: tensor has element type ",
                                DataType_Name(dt), ", vector has type ",
                                DataType_Name(DataTypeToEnum<T>::value),
                                "; don't know how to convert");
    }
  }
  return Status::OK();
}

template <typename T>
static Status GetStaticInputVector(
    const Node* op, int64 input_index,
    const std::vector<const Tensor*>& static_input_map,
    std::vector<T>* vector) {
  Node* input_node;
  TF_RETURN_IF_ERROR(op->input_node(input_index, &input_node));
  Tensor input_tensor;
  TF_RETURN_IF_ERROR(
      GetStaticNodeTensor(input_node, static_input_map, &input_tensor));
  TF_RETURN_IF_ERROR(TensorDataToVector(input_tensor, vector));
  return Status::OK();
}

static Status GetStaticInputNode(
    const Node* op, int64 input_index,
    const std::vector<const Tensor*>& static_input_map, DataType dt,
    ng::Output<ng::Node>& node_) {
  ng::element::Type type;
  TF_RETURN_IF_ERROR(util::TFDataTypeToNGraphElementType(dt, &type));
  switch (dt) {
    case DataType::DT_FLOAT: {
      std::vector<float> vec_float;
      TF_RETURN_IF_ERROR(
          GetStaticInputVector(op, input_index, static_input_map, &vec_float));
      node_ = ConstructNgNode<opset::Constant>(op->name(), type, ng::Shape{},
                                               vec_float[0]);
    } break;
    case DataType::DT_DOUBLE: {
      std::vector<double> vec_double;
      TF_RETURN_IF_ERROR(
          GetStaticInputVector(op, input_index, static_input_map, &vec_double));
      node_ = ConstructNgNode<opset::Constant>(op->name(), type, ng::Shape{},
                                               vec_double[0]);
    } break;
    case DataType::DT_INT32: {
      std::vector<int32> vec_i32;
      TF_RETURN_IF_ERROR(
          GetStaticInputVector(op, input_index, static_input_map, &vec_i32));
      node_ = ConstructNgNode<opset::Constant>(op->name(), type, ng::Shape{},
                                               vec_i32[0]);
    } break;
    case DataType::DT_INT64: {
      std::vector<int64> vec_i64;
      TF_RETURN_IF_ERROR(
          GetStaticInputVector(op, input_index, static_input_map, &vec_i64));
      node_ = ConstructNgNode<opset::Constant>(op->name(), type, ng::Shape{},
                                               vec_i64[0]);
    } break;
    default:
      return errors::Internal("GetStaticInputNode: TF data type ",
                              DataType_Name(dt), " not supported.");
      break;
  }
  return Status::OK();
}

// Taken from: tensorflow/core/grappler/optimizers/arithmetic_optimizer.cc
// Extract values from a Const op to `values`. Returns true if succeeds.
//
// Modified with an extra `VecT` parameter to handle the case where the type
// in the vector does not match TensorFlow's notion of what the C++ type
// should be (e.g. when T is `bool`, we actually need a vector of `char` for
// compatibility with nGraph).
template <typename T, typename VecT = T>
static Status ValuesFromConstNode(const NodeDef& node,
                                  TensorShapeProto* const_tensor_shape,
                                  std::vector<VecT>* values) {
  if (node.op() != "Const") {
    return errors::InvalidArgument("Node not a Const");
  }

  if (node.attr().at("dtype").type() != DataTypeToEnum<T>::value) {
    std::stringstream ss;
    ss << "Invalid data type defined for Const. Defined: "
       << node.attr().at("dtype").type();
    return errors::InvalidArgument(ss.str());
  }

  // TensorProto represents the content of the tensor in either <type>_val or
  // tensor_content.
  const TensorProto& tensor = node.attr().at("value").tensor();
  typename checkpoint::SaveTypeTraits<T>::RepeatedField* tensor_values =
      checkpoint::MutableTensorProtoData<T>(const_cast<TensorProto*>(&tensor));

  const TensorShapeProto& shape = tensor.tensor_shape();
  *const_tensor_shape = shape;
  if (!tensor_values->empty() && tensor.has_tensor_shape()) {
    // When tensor_shape is set, theoretically the representation of the data
    // could be compressed. So, before copying values to the returned vector,
    // make sure no compression happens.
    if (shape.dim_size() == 1 && shape.dim(0).size() == tensor_values->size()) {
      values->insert(values->end(), tensor_values->begin(),
                     tensor_values->end());
      return Status::OK();
    }
  }

  const auto tensor_content_size = tensor.tensor_content().size();
  CHECK_EQ(0, tensor_content_size % sizeof(VecT))
      << " tensor_content_size (" << tensor_content_size
      << ") is not a multiple of " << sizeof(VecT);

  // If tensor_content_size is zero, we'll have to take the values from
  // int_val, float_val, etc.
  if (tensor_content_size == 0) {
    int64 n_elements = 1;
    for (auto i = 0; i < shape.dim_size(); i++) {
      if (shape.dim(i).size() < 0) {
        return errors::InvalidArgument(
            "Const node has empty tensor and an unknown dimension size");
      }
      n_elements *= shape.dim(i).size();
    }
    values->resize(n_elements);

    auto val_lastsaved = (T)0;  // cast

    for (auto i = 0; i < n_elements; i++) {
      auto& tensor = node.attr().at("value").tensor();
      auto dt = node.attr().at("dtype").type();
      int64 val_size = 0;
      auto val_i = (T)0;  // cast
      switch (dt) {
        // TODO(amprocte/NGRAPH-2502): there are more element types to support
        // here
        case DT_INT32:
          val_size = tensor.int_val_size();
          if (val_size > 0) val_i = tensor.int_val()[i];
          break;
        case DT_INT64:
          val_size = tensor.int64_val_size();
          if (val_size > 0) val_i = tensor.int64_val()[i];
          break;
        case DT_FLOAT:
          val_size = tensor.float_val_size();
          if (val_size > 0) val_i = tensor.float_val()[i];
          break;
        case DT_BOOL:
          val_size = tensor.bool_val_size();
          if (val_size > 0) val_i = tensor.bool_val()[i];
          break;
        case DT_DOUBLE:
          val_size = tensor.double_val_size();
          if (val_size > 0) val_i = tensor.double_val()[i];
          break;
        default:
          OVTF_VLOG(0)
              << "Const node has empty tensor and we don't know how to "
                 "handle this element type";
          OVTF_VLOG(0) << node.DebugString();
          OVTF_VLOG(0) << shape.DebugString();
          return errors::Unimplemented("Encountered unknown element type ",
                                       DataType_Name(dt),
                                       " on an empty tensor");
      }
      if (val_size == 0) {
#if (TF_MAJOR_VERSION > 1 && TF_MINOR_VERSION >= 7)
        val_i = 0;
#else
        return errors::InvalidArgument("Empty values vector");
#endif

      } else if (i < val_size) {
        (*values)[i] = val_i;
        val_lastsaved = val_i;
      } else {
        (*values)[i] = val_lastsaved;
      }
    }
  } else {
    values->resize(tensor_content_size / sizeof(VecT));
    port::CopyToArray(tensor.tensor_content(),
                      reinterpret_cast<char*>(values->data()));
  }

  return Status::OK();
}

template <typename T>
static Status MakeConstOpForParam(const Tensor& tensor, string prov_tag,
                                  ng::element::Type ng_et, ng::Shape ng_shape,
                                  ng::Output<ng::Node>& ng_node) {
  vector<T> const_values;

  TensorDataToVector(tensor, &const_values);
  ng_node =
      ConstructNgNode<opset::Constant>(prov_tag, ng_et, ng_shape, const_values);

  return Status::OK();
}

// Helper for Builder::TranslateGraph ("Const" op)
template <typename T, typename VecT = T>
static Status MakeConstOp(const Node* op, ng::element::Type et,
                          ng::Output<ng::Node>& ng_node) {
  vector<VecT> const_values;
  TensorShapeProto shape_proto;

  TF_RETURN_IF_ERROR(
      ValuesFromConstNode<T, VecT>(op->def(), &shape_proto, &const_values));

  TensorShape const_shape(shape_proto);

  ng::Shape ng_shape;
  TF_RETURN_IF_ERROR(util::TFTensorShapeToNGraphShape(const_shape, &ng_shape));

  ng_node =
      ConstructNgNode<opset::Constant>(op->name(), et, ng_shape, const_values);
  return Status::OK();
}

const Builder::ConstMap& Builder::TF_NGRAPH_CONST_MAP() {
  static const Builder::ConstMap the_map = {
      {DataType::DT_FLOAT, make_pair(MakeConstOp<float>, ng::element::f32)},
      {DataType::DT_DOUBLE, make_pair(MakeConstOp<double>, ng::element::f64)},
      {DataType::DT_INT8, make_pair(MakeConstOp<int8>, ng::element::i8)},
      {DataType::DT_INT16, make_pair(MakeConstOp<int16>, ng::element::i16)},
      {DataType::DT_QINT8, make_pair(MakeConstOp<qint8>, ng::element::i8)},
      {DataType::DT_QUINT8, make_pair(MakeConstOp<quint8>, ng::element::u8)},
      {DataType::DT_QUINT16, make_pair(MakeConstOp<quint16>, ng::element::u16)},
      {DataType::DT_INT32, make_pair(MakeConstOp<int32>, ng::element::i32)},
      {DataType::DT_INT64, make_pair(MakeConstOp<int64>, ng::element::i64)},
      {DataType::DT_UINT8, make_pair(MakeConstOp<uint8>, ng::element::u8)},
      {DataType::DT_UINT16, make_pair(MakeConstOp<uint16>, ng::element::u16)},
      {DataType::DT_BOOL,
       make_pair(MakeConstOp<bool, char>, ng::element::boolean)}};
  return the_map;
}

// Helper function to translate a unary op.
//
// Parameters:
//
//    Node* op                   - TF op being translated. Must have one input.
//    const std::vector<const Tensor*>& static_input_map
//                               - the static input map
//    Builder::OpMap& ng_op_map  - The TF-to-nGraph op map.
//
//    std::function<ng::Output<ng::Node>(ng::Output<ng::Node>>
//      create_unary_op           - Function to construct the graph implementing
//                                 the unary op, given the input to the unop
//                                 as an argument.
//
// Example usage:
//
//  if (n->type_string == "Square") {
//    TF_RETURN_IF_ERROR(TranslateUnaryOp(n, static_input_map, ng_op_map,
//                       [] (ng::Output<ng::Node> n) {
//                           return
//                           (ng::Output<opset::Multiply>(n,n));
//                       });
//  }
static Status TranslateUnaryOp(
    const Node* op, const std::vector<const Tensor*>&,
    Builder::OpMap& ng_op_map,
    std::function<ng::Output<ng::Node>(ng::Output<ng::Node>)> create_unary_op) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));
  auto ng_node = create_unary_op(ng_input);
  if (ng_node != ng_input) {
    Builder::SetTracingInfo(op->name(), ng_node);
  }
  SaveNgOp(ng_op_map, op->name(), ng_node);
  return Status::OK();
}

// Helper function to translate a unary op in cases where there is a one-to-one
// mapping from TensorFlow ops to nGraph ops.
//
// Example usage:
//
//  if (n->type_string == "Abs") {
//    TF_RETURN_IF_ERROR(TranslateUnaryOp<ng::op::Abs>(n, static_input_map,
//    ng_op_map));
//  }
//
template <typename T>
static Status TranslateUnaryOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return TranslateUnaryOp(op, static_input_map, ng_op_map,
                          [&op](ng::Output<ng::Node> n) {
                            return ConstructNgNode<T>(op->name(), n);
                          });
}

// Helper function to translate a binary op
// Parameters:
//
//    Node* op               - TF op being translated. Must have only two
//    inputs.
//    const std::vector<const Tensor*>& static_input_map - the static input map
//    Builder::OpMap& ng_op_map  - The TF-to-nGraph op map.
//    std::function<ng::Output<ng::Node>(ng::Output<ng::Node>,
//    ng::Output<ng::Node>)>
//    create_binary_op           - Function to construct the graph implementing
//                                 the binary op, given the 2 ng_inputs to the
//                                 binaryop
// Example Usage:
//
// if (op->type_string() == "SquaredDifference") {
//      TF_RETURN_IF_ERROR(TranslateBinaryOp(op, ng_op_map,
//         [](ng::Output<ng::Node> ng_input1, ng::Output<ng::Node>
//         ng_input2) {
//           auto ng_diff = ng::Output<opset::Subtract>(input1,
//           input2);
//           return ng::Output<opset::Multiply>(ng_diff,ng_diff);
//         }));
//    }
//

static Status TranslateBinaryOp(
    const Node* op, const std::vector<const Tensor*>&,
    Builder::OpMap& ng_op_map,
    std::function<ng::Output<ng::Node>(ng::Output<ng::Node>&,
                                       ng::Output<ng::Node>&)>
        create_binary_op) {
  ng::Output<ng::Node> ng_lhs, ng_rhs;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_lhs, ng_rhs));
  auto ng_node = create_binary_op(ng_lhs, ng_rhs);
  if (ng_node != ng_lhs && ng_node != ng_rhs) {
    Builder::SetTracingInfo(op->name(), ng_node);
  }
  SaveNgOp(ng_op_map, op->name(), ng_node);
  return Status::OK();
}

// Helper function to translate a binary op in cases where there is a one-to-one
// mapping from TensorFlow ops to nGraph ops.
//
// Example usage:
//
//  if (n->type_string == "Add") {
//    TF_RETURN_IF_ERROR(TranslateBinaryOp<opset::Add>(op,
//    static_input_map,
//    ng_op_map));
//  }
//
template <typename T>
static Status TranslateBinaryOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return TranslateBinaryOp(
      op, static_input_map, ng_op_map,
      [&op](ng::Output<ng::Node>& ng_lhs, ng::Output<ng::Node>& ng_rhs) {
        return ConstructNgNode<T>(op->name(), ng_lhs, ng_rhs);
      });
}

static Status TranslateAddNOp(const Node* op, const std::vector<const Tensor*>&,
                              Builder::OpMap& ng_op_map) {
  std::vector<ng::Output<ng::Node>> ng_arg_vec(op->num_inputs());

  for (int inp_idx = 0; inp_idx < op->num_inputs(); inp_idx++)
    TF_RETURN_IF_ERROR(
        GetInputNode(ng_op_map, op, inp_idx, ng_arg_vec[inp_idx]));
  auto ng_addn = std::accumulate(
      std::next(ng_arg_vec.begin()), ng_arg_vec.end(), ng_arg_vec.at(0),
      [&op](ng::Output<ng::Node> a, ng::Output<ng::Node> b) {
        return ConstructNgNode<opset::Add>(op->name(), a, b);
      });  // accumulation: start with
           // first element. default op is
           // addition
  SaveNgOp(ng_op_map, op->name(), ng_addn);
  return Status::OK();
}
static Status TranslateArgMinMax(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map, std::string mode) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  std::vector<int64> tf_dim;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &tf_dim));

  size_t input_rank = ng_input.get_partial_shape().rank().get_length();

  if (tf_dim.size() != 1) {
    return errors::InvalidArgument(
        "ArgMax Op: dimension must be scalar, operates on a single axis");
  }

  // If input dimension is negative, make it positive
  if (tf_dim[0] < 0) {
    OVTF_VLOG(3) << "Input dimension is negative, make it positive "
                 << tf_dim[0];
    tf_dim[0] = (int64)input_rank + tf_dim[0];
  }
  OVTF_VLOG(3) << "Axis along which to compute " << tf_dim[0];
  size_t k_axis = tf_dim[0];

  DataType dtype;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "output_type", &dtype));

  ng::element::Type ng_et;
  TF_RETURN_IF_ERROR(util::TFDataTypeToNGraphElementType(dtype, &ng_et));

  auto ng_k = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{}, std::vector<int64>({1}));

  std::string sort = "none";
  auto ng_topk =
      std::make_shared<opset::TopK>(ng_input, ng_k, k_axis, mode, sort, ng_et);
  auto ng_indices = ng_topk->output(1);
  int axis = ng_topk->get_axis();
  auto axis_to_remove = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{1}, std::vector<int64>({axis}));
  auto reshaped_indices =
      ConstructNgNode<opset::Squeeze>(op->name(), ng_indices, axis_to_remove);
  Builder::SetTracingInfo(op->name(), reshaped_indices);
  SaveNgOp(ng_op_map, op->name(), reshaped_indices);
  return Status::OK();
}

static Status TranslateArgMaxOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return (TranslateArgMinMax(op, static_input_map, ng_op_map, "max"));
}

static Status TranslateArgMinOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return (TranslateArgMinMax(op, static_input_map, ng_op_map, "min"));
}

template <unsigned int N>
static Status TranslateAvgPoolOp(const Node* op,
                                 const std::vector<const Tensor*>&,
                                 Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  std::vector<int32> tf_strides;
  std::vector<int32> tf_ksize;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "ksize", &tf_ksize));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if ((tf_data_format != "NHWC") && (tf_data_format != "NCHW") &&
      (tf_data_format != "NDHWC")) {
    return errors::InvalidArgument(
        "AvgPool data format is none of NHWC, NCHW, or NDHWC");
  }

  bool is_nhwc = (tf_data_format == "NHWC") || (tf_data_format == "NDHWC");

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_ksize);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(N);
  ng::Shape ng_kernel_shape(N);

  NHWCtoHW(is_nhwc, tf_strides, ng_strides);
  NHWCtoHW(is_nhwc, tf_ksize, ng_kernel_shape);
  NHWCtoNCHW(op->name(), is_nhwc, ng_input);
  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::Shape ng_padding_below, ng_padding_above;

  ng::op::PadType auto_pad_type;
  if (tf_padding_type == "SAME")
    auto_pad_type = ng::op::PadType::SAME_UPPER;
  else if (tf_padding_type == "VALID")
    auto_pad_type = ng::op::PadType::VALID;

  // since we are using auto_pad, all the explicit padding arguments will be
  // ignored
  ng::Output<ng::Node> ng_avgpool = ConstructNgNode<opset::AvgPool>(
      op->name(), ng_input, ng_strides, ng_padding_below, ng_padding_above,
      ng_kernel_shape, true, ng::op::RoundingType::FLOOR, auto_pad_type);

  NCHWtoNHWC(op->name(), is_nhwc, ng_avgpool);
  OVTF_VLOG(3) << "avgpool outshape: {" << ng::join(ng_avgpool.get_shape())
               << "}";

  SaveNgOp(ng_op_map, op->name(), ng_avgpool);
  return Status::OK();
}

static Status TranslateBatchNDAndSpaceNDOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_block_shape, ng_crops;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_input, ng_block_shape, ng_crops));

  // ng_crops should be of shape N=[ng_input.get_shape()).size()]
  // But TF's ng_crops input is limited only to the spatial dimensions (neither
  // batch nor innermost),
  // which would mean ngraph inputs have missing ng_crops[0] and ng_crops[N].
  // Hence, pad ng_crops with zeros at both ends

  std::vector<int> tf_block_shape;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 1, static_input_map, &tf_block_shape));

  auto N = (int)ng_input.get_partial_shape().rank().get_length();
  auto M = (int)tf_block_shape.size();

  // return with input if rank < 2 as ngraph's impl doesn't support it
  if (N < 2) {
    SaveNgOp(ng_op_map, op->name(), ng_input);
    return Status::OK();
  }

  auto crops = ConstructNgNode<opset::Pad>(
      op->name(), ng_crops,
      make_shared<opset::Constant>(ng_crops.get_element_type(), ng::Shape{2},
                                   std::vector<int>{1, 0}),
      make_shared<opset::Constant>(ng_crops.get_element_type(), ng::Shape{2},
                                   std::vector<int>{N - M - 1, 0}),
      ng::op::PadMode::CONSTANT);

  // Padding needs to be done for block_shape as done for crops above but with
  // value=1
  auto block_shape = ConstructNgNode<opset::Pad>(
      op->name(), ng_block_shape,
      make_shared<opset::Constant>(ng_block_shape.get_element_type(),
                                   ng::Shape{1}, std::vector<int>{1}),
      make_shared<opset::Constant>(ng_block_shape.get_element_type(),
                                   ng::Shape{1}, std::vector<int>{N - M - 1}),
      make_shared<opset::Constant>(ng_block_shape.get_element_type(),
                                   ng::Shape{}, 1),
      ng::op::PadMode::CONSTANT);

  auto target_axis =
      make_shared<opset::Constant>(ng::element::i64, ng::Shape{}, 1);
  // split into two 1-D vectors crops_begin and crops_end along axis 1
  auto crops_split =
      ConstructNgNode<opset::Split>(op->name(), crops, target_axis, 2);

  // crops: [[0, 1], [1, 2], ...]
  // crops_split: [[[0], [1]], [[1], [2]], ...]
  // crops_begin: [0, 1, ...], crops_end: [1, 2, ...]
  auto axes = make_shared<opset::Constant>(ng::element::i32, ng::Shape{}, -1);
  auto crops_begin = ConstructNgNode<opset::Squeeze>(
      op->name(), crops_split.get_node()->outputs()[0], axes);
  auto crops_end = ConstructNgNode<opset::Squeeze>(
      op->name(), crops_split.get_node()->outputs()[1], axes);

  if (op->type_string() == "BatchToSpaceND") {
    auto ng_batch_to_space_nd = ConstructNgNode<opset::BatchToSpace>(
        op->name(), ng_input, block_shape, crops_begin, crops_end);
    SaveNgOp(ng_op_map, op->name(), ng_batch_to_space_nd);
  } else if (op->type_string() == "SpaceToBatchND") {
    auto ng_space_to_batch_nd = ConstructNgNode<opset::SpaceToBatch>(
        op->name(), ng_input, block_shape, crops_begin, crops_end);
    SaveNgOp(ng_op_map, op->name(), ng_space_to_batch_nd);
  } else {
    return errors::Unknown("Unknown Op Name: ", op->name());
  }

  return Status::OK();
}

static Status TranslateBiasAddOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_bias;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_bias));

  std::string tf_data_format;
  if (GetNodeAttr(op->attrs(), "data_format", &tf_data_format) !=
      Status::OK()) {
    tf_data_format = "NHWC";
  }

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "BiasAdd data format is neither NHWC nor NCHW");
  }

  auto ng_input_shape = ng_input.get_shape();
  auto ng_bias_shape = ng_bias.get_shape();
  if (ng_bias_shape.size() != 1) {
    return errors::InvalidArgument(
        "Bias argument to BiasAdd does not have one dimension");
  }

  // We'll choose reshape over broadcast
  // Reshape the bias to (1, C, 1, ...) if input is channels-first.
  ng::Output<ng::Node> ng_bias_reshaped = ng_bias;
  if (tf_data_format == "NCHW") {
    auto channel_dim = ng_input_shape[1];
    std::vector<int64> target_shape(ng_input_shape.size());
    for (int64_t i = 0; i < ng_input_shape.size(); i++) {
      if (i == 1) {
        target_shape[i] = channel_dim;
      } else {
        target_shape[i] = 1;
      }
    }
    auto target_shape_node = make_shared<opset::Constant>(
        ng::element::i64, ng::Shape{ng_input_shape.size()}, target_shape);
    ng_bias_reshaped = ConstructNgNode<opset::Reshape>(
        op->name(), ng_bias, target_shape_node, false);
  }

  ng::Output<ng::Node> ng_add =
      ConstructNgNode<opset::Add>(op->name(), ng_input, ng_bias_reshaped);

  SaveNgOp(ng_op_map, op->name(), ng_add);
  return Status::OK();
}

static Status TranslateCastOp(const Node* op, const std::vector<const Tensor*>&,
                              Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  DataType dtype;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "DstT", &dtype));

  ng::element::Type ng_et;
  TF_RETURN_IF_ERROR(util::TFDataTypeToNGraphElementType(dtype, &ng_et));

  try {
    SaveNgOp(ng_op_map, op->name(),
             ConstructNgNode<opset::Convert>(op->name(), ng_input, ng_et));
  } catch (const std::out_of_range&) {
    return errors::Unimplemented("Failed to convert TF data type: ",
                                 DataType_Name(dtype));
  }
  return Status::OK();
}

static Status TranslateConcatV2Op(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  TF_RETURN_IF_ERROR(ValidateInputCountMin(op, 2));

  std::vector<int64> tf_concat_axis_vec;
  TF_RETURN_IF_ERROR(GetStaticInputVector(
      op, op->num_inputs() - 1, static_input_map, &tf_concat_axis_vec));

  int64 concat_axis = tf_concat_axis_vec[0];

  if (concat_axis < 0) {
    ng::Output<ng::Node> ng_first_arg;
    TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_first_arg));

    concat_axis += int64(ng_first_arg.get_shape().size());
  }

  ng::OutputVector ng_args;

  for (int i = 0; i < op->num_inputs() - 1; i++) {
    ng::Output<ng::Node> ng_arg;
    TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, i, ng_arg));
    ng_args.push_back(ng_arg);
  }

  SaveNgOp(
      ng_op_map, op->name(),
      ConstructNgNode<opset::Concat>(op->name(), ng_args, size_t(concat_axis)));
  return Status::OK();
}

static Status TranslateConstOp(const Node* op,
                               const std::vector<const Tensor*>&,
                               Builder::OpMap& ng_op_map) {
  DataType dtype;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dtype", &dtype));

  ng::Output<ng::Node> ng_node;

  // For some reason the following do not work (no specialization of
  // tensorflow::checkpoint::SavedTypeTraits...)
  // case DataType::DT_UINT32:
  //   TF_RETURN_IF_ERROR(MakeConstOp<uint32>(op, ng::element::u32,
  //   &ng_node));
  //   break;
  // case DataType::DT_UINT64:
  //   TF_RETURN_IF_ERROR(MakeConstOp<uint64>(op, ng::element::u64,
  //   &ng_node));
  //   break;
  try {
    const auto& func_param = Builder::TF_NGRAPH_CONST_MAP().at(dtype);
    TF_RETURN_IF_ERROR(func_param.first(op, func_param.second, ng_node));
  } catch (const std::out_of_range&) {
    return errors::Unimplemented("Failed to translate Constant with TF type:",
                                 DataType_Name(dtype));
  }

  SaveNgOp(ng_op_map, op->name(), ng_node);
  return Status::OK();
}

static Status TranslateConv2DOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_filter;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_filter));

  std::vector<int32> tf_strides;
  std::vector<int32> tf_dilations;
  std::vector<int32> tf_paddings;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "Conv2D data format is neither NHWC nor NCHW");
  }

  bool is_nhwc = (tf_data_format == "NHWC");

  // TF Kernel Test Checks
  // Strides in the batch and depth dimension is not supported
  if (tf_strides[0] != 1 || tf_strides[is_nhwc ? 3 : 1] != 1) {
    return errors::InvalidArgument(
        "Strides in batch and depth dimensions is not supported: ",
        op->type_string());
  }

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_dilations);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(2);
  ng::Strides ng_dilations(2);
  ng::Shape ng_image_shape(2);
  ng::Shape ng_kernel_shape(2);

  NHWCtoHW(is_nhwc, tf_strides, ng_strides);
  NHWCtoHW(is_nhwc, ng_input.get_shape(), ng_image_shape);
  NHWCtoHW(is_nhwc, tf_dilations, ng_dilations);
  NHWCtoNCHW(op->name(), is_nhwc, ng_input);

  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
  OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

  auto& ng_filter_shape = ng_filter.get_shape();
  ng_kernel_shape[0] = ng_filter_shape[0];
  ng_kernel_shape[1] = ng_filter_shape[1];
  Transpose<3, 2, 0, 1>(ng_filter);
  Builder::SetTracingInfo(op->name(), ng_filter);

  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::CoordinateDiff ng_padding_below;
  ng::CoordinateDiff ng_padding_above;
  if (tf_padding_type == "EXPLICIT") {
    TF_RETURN_IF_ERROR(
        GetNodeAttr(op->attrs(), "explicit_paddings", &tf_paddings));
    if (is_nhwc) {
      ng_padding_below.push_back(tf_paddings[2]);
      ng_padding_below.push_back(tf_paddings[4]);
      ng_padding_above.push_back(tf_paddings[3]);
      ng_padding_above.push_back(tf_paddings[5]);
    } else {
      ng_padding_below.push_back(tf_paddings[4]);
      ng_padding_below.push_back(tf_paddings[6]);
      ng_padding_above.push_back(tf_paddings[5]);
      ng_padding_above.push_back(tf_paddings[7]);
    }
    OVTF_VLOG(3) << " ========== EXPLICIT Padding ========== ";
    OVTF_VLOG(3) << "ng_padding_below: " << ng::join(ng_padding_below);
    OVTF_VLOG(3) << "ng_padding_above: " << ng::join(ng_padding_above);
  } else {
    Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                         ng_strides, ng_dilations, ng_padding_below,
                         ng_padding_above);
  }

  ng::Output<ng::Node> ng_conv = ConstructNgNode<opset::Convolution>(
      op->name(), ng_input, ng_filter, ng_strides, ng_padding_below,
      ng_padding_above, ng_dilations);

  NCHWtoNHWC(op->name(), is_nhwc, ng_conv);
  SaveNgOp(ng_op_map, op->name(), ng_conv);
  return Status::OK();
}

static Status TranslateConv2DBackpropInputOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_filter, ng_out_backprop, ng_unused;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_unused, ng_filter, ng_out_backprop));

  // TODO: refactor me to be less redundant with other convolution ops
  std::vector<int32> tf_strides;
  std::vector<int32> tf_dilations;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "Conv2DBackpropInput data format is neither NHWC nor NCHW: %s",
        tf_data_format);
  }

  std::vector<int64> tf_input_sizes;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 0, static_input_map, &tf_input_sizes));

  if (std::any_of(tf_input_sizes.begin(), tf_input_sizes.end(),
                  [](int32 size) { return size <= 0; })) {
    return errors::InvalidArgument(
        "Conv2DBackpropInput input sizes must be positive integers");
  }

  bool is_nhwc = (tf_data_format == "NHWC");

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_dilations);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(2);
  ng::Strides ng_dilations(2);
  ng::Shape ng_image_shape(2);
  ng::Shape ng_kernel_shape(2);
  ng::Shape ng_batch_shape(4);

  NHWCtoHW(is_nhwc, tf_strides, ng_strides);
  NHWCtoHW(is_nhwc, tf_dilations, ng_dilations);
  NHWCtoHW(is_nhwc, tf_input_sizes, ng_image_shape);
  NHWCtoNCHW(op->name(), is_nhwc, ng_out_backprop);
  if (is_nhwc) {
    ng_batch_shape = {static_cast<unsigned long>(tf_input_sizes[0]),
                      static_cast<unsigned long>(tf_input_sizes[3]),
                      static_cast<unsigned long>(tf_input_sizes[1]),
                      static_cast<unsigned long>(tf_input_sizes[2])};
  } else {
    ng_batch_shape = {static_cast<unsigned long>(tf_input_sizes[0]),
                      static_cast<unsigned long>(tf_input_sizes[1]),
                      static_cast<unsigned long>(tf_input_sizes[2]),
                      static_cast<unsigned long>(tf_input_sizes[3])};
  }

  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
  OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

  auto& ng_filter_shape = ng_filter.get_shape();
  ng_kernel_shape[0] = ng_filter_shape[0];
  ng_kernel_shape[1] = ng_filter_shape[1];
  Transpose<3, 2, 0, 1>(ng_filter);
  Builder::SetTracingInfo(op->name(), ng_filter);

  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::CoordinateDiff ng_padding_below;
  ng::CoordinateDiff ng_padding_above;
  Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                       ng_strides, ng_dilations, ng_padding_below,
                       ng_padding_above);

  auto ng_output_shape = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{ng_batch_shape.size() - 2},
      vector<size_t>(ng_batch_shape.begin() + 2, ng_batch_shape.end()));

  auto ng_data = ConstructNgNode<opset::ConvolutionBackpropData>(
      op->name(), ng_out_backprop, ng_filter, ng_output_shape, ng_strides,
      ng_padding_below, ng_padding_above, ng_dilations);

  NCHWtoNHWC(op->name(), is_nhwc, ng_data);
  SaveNgOp(ng_op_map, op->name(), ng_data);
  return Status::OK();
}

// Translate Conv3D Op
static Status TranslateConv3DOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_filter;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_filter));

  std::vector<int32> tf_strides;
  std::vector<int32> tf_dilations;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NDHWC" && tf_data_format != "NCDHW") {
    return errors::InvalidArgument(
        "Conv3D data format is neither NDHWC nor NCDHW");
  }

  bool is_ndhwc = (tf_data_format == "NDHWC");

  // TODO: in 3D
  // TF Kernel Test Checks
  // // Strides in the batch and depth dimension is not supported
  // if (tf_strides[0] != 1 || tf_strides[is_nhwc ? 3 : 1] != 1) {
  //   return errors::InvalidArgument(
  //       "Strides in batch and depth dimensions is not supported: ",
  //       op->type_string());
  // }

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_dilations);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(3);
  ng::Strides ng_dilations(3);
  ng::Shape ng_image_shape(3);
  ng::Shape ng_kernel_shape(3);

  NHWCtoHW(is_ndhwc, tf_strides, ng_strides);
  NHWCtoHW(is_ndhwc, ng_input.get_shape(), ng_image_shape);
  NHWCtoHW(is_ndhwc, tf_dilations, ng_dilations);
  NHWCtoNCHW(op->name(), is_ndhwc, ng_input);

  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
  OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

  auto& ng_filter_shape = ng_filter.get_shape();
  ng_kernel_shape[0] = ng_filter_shape[0];
  ng_kernel_shape[1] = ng_filter_shape[1];
  ng_kernel_shape[2] = ng_filter_shape[2];
  Transpose3D<4, 3, 0, 1, 2>(ng_filter);
  Builder::SetTracingInfo(op->name(), ng_filter);

  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::CoordinateDiff ng_padding_below;
  ng::CoordinateDiff ng_padding_above;
  Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                       ng_strides, ng_dilations, ng_padding_below,
                       ng_padding_above);

  ng::Output<ng::Node> ng_conv = ConstructNgNode<opset::Convolution>(
      op->name(), ng_input, ng_filter, ng_strides, ng_padding_below,
      ng_padding_above, ng_dilations);

  NCHWtoNHWC(op->name(), is_ndhwc, ng_conv);
  SaveNgOp(ng_op_map, op->name(), ng_conv);
  return Status::OK();
}

static Status TranslateConv3DBackpropInputV2Op(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_filter, ng_out_backprop, ng_unused;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_unused, ng_filter, ng_out_backprop));

  // TODO: refactor me to be less redundant with other convolution ops
  std::vector<int32> tf_strides;
  std::vector<int32> tf_dilations;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NDHWC" && tf_data_format != "NCDHW") {
    return errors::InvalidArgument(
        "Conv2DBackpropInput data format is neither NDHWC nor NCDHW: %s",
        tf_data_format);
  }

  std::vector<int64> tf_input_sizes;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 0, static_input_map, &tf_input_sizes));

  if (std::any_of(tf_input_sizes.begin(), tf_input_sizes.end(),
                  [](int32 size) { return size <= 0; })) {
    return errors::InvalidArgument(
        "Conv2DBackpropInput input sizes must be positive integers");
  }

  bool is_ndhwc = (tf_data_format == "NDHWC");

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_dilations);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(3);
  ng::Strides ng_dilations(3);
  ng::Shape ng_image_shape(3);
  ng::Shape ng_kernel_shape(3);
  ng::Shape ng_batch_shape(5);

  NHWCtoHW(is_ndhwc, tf_strides, ng_strides);
  NHWCtoHW(is_ndhwc, tf_dilations, ng_dilations);
  NHWCtoHW(is_ndhwc, tf_input_sizes, ng_image_shape);
  NHWCtoNCHW(op->name(), is_ndhwc, ng_out_backprop);
  if (is_ndhwc) {
    ng_batch_shape = {static_cast<unsigned long>(tf_input_sizes[0]),
                      static_cast<unsigned long>(tf_input_sizes[4]),
                      static_cast<unsigned long>(tf_input_sizes[1]),
                      static_cast<unsigned long>(tf_input_sizes[2]),
                      static_cast<unsigned long>(tf_input_sizes[3])};
  } else {
    ng_batch_shape = {static_cast<unsigned long>(tf_input_sizes[0]),
                      static_cast<unsigned long>(tf_input_sizes[1]),
                      static_cast<unsigned long>(tf_input_sizes[2]),
                      static_cast<unsigned long>(tf_input_sizes[3]),
                      static_cast<unsigned long>(tf_input_sizes[4])};
  }

  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
  OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

  auto& ng_filter_shape = ng_filter.get_shape();
  ng_kernel_shape[0] = ng_filter_shape[0];
  ng_kernel_shape[1] = ng_filter_shape[1];
  ng_kernel_shape[2] = ng_filter_shape[2];
  Transpose3D<4, 3, 0, 1, 2>(ng_filter);
  Builder::SetTracingInfo(op->name(), ng_filter);

  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::CoordinateDiff ng_padding_below;
  ng::CoordinateDiff ng_padding_above;
  Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                       ng_strides, ng_dilations, ng_padding_below,
                       ng_padding_above);

  auto ng_output_shape = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{ng_batch_shape.size() - 2},
      vector<size_t>(ng_batch_shape.begin() + 2, ng_batch_shape.end()));

  auto ng_data = ConstructNgNode<opset::ConvolutionBackpropData>(
      op->name(), ng_out_backprop, ng_filter, ng_output_shape, ng_strides,
      ng_padding_below, ng_padding_above, ng_dilations);

  NCHWtoNHWC(op->name(), is_ndhwc, ng_data);
  SaveNgOp(ng_op_map, op->name(), ng_data);
  return Status::OK();
}

static Status TranslateCropAndResizeOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  /// ng_input: [batch, image_height, image_width, depth]
  /// ng_boxes: [num_boxes, 4]; each box is a normalized [0.to 1.] co-ordinate
  /// [y1,
  /// x1, y2, x2]
  /// ng_box_ind: [num_boxes]; i-th ng_box_ind refers to the image to crop and
  /// ranges from 0 to batch
  /// ng_crop_size: [crop_height, crop_width];

  /// for each box b specified in ng_boxes:
  ///  1. crop ng_input[ng_box_ind[b]] w/ co-ordinates in ng_boxes
  ///  2. resize according to method

  ng::Output<ng::Node> ng_input, ng_boxes, ng_box_ind, ng_size;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_input, ng_boxes, ng_box_ind, ng_size));

  string tf_resize_method;
  float tf_extrapolation_value;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "method", &tf_resize_method));
  TF_RETURN_IF_ERROR(
      GetNodeAttr(op->attrs(), "extrapolation_value", &tf_extrapolation_value));

  auto spatial_shape = ng_input.get_shape();
  auto image_height = spatial_shape[1];
  auto image_width = spatial_shape[2];
  auto image_depth = spatial_shape[3];

  std::vector<float> boxes;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &boxes));

  std::vector<int64> box_ind;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 2, static_input_map, &box_ind));

  std::vector<int64> crop_size;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 3, static_input_map, &crop_size));

  ng::OutputVector ng_crop_outputs(box_ind.size());
  if (box_ind.size() == 0) {
    SaveNgOp(ng_op_map, op->name(),
             ConstructNgNode<opset::Constant>(
                 op->name(), ng::element::f32,
                 ngraph::Shape{0, static_cast<unsigned long>(crop_size.at(0)),
                               static_cast<unsigned long>(crop_size.at(1)),
                               image_depth},
                 std::vector<float>({})));
  } else {
    for (int i = 0; i < box_ind.size(); i++) {
      int y1, x1, y2, x2;
      y1 = boxes.at(0 + i * 4) * (image_height - 1);
      x1 = boxes.at(1 + i * 4) * (image_width - 1);
      y2 = boxes.at(2 + i * 4) * (image_height - 1);
      x2 = boxes.at(3 + i * 4) * (image_width - 1);

      int crop_height = std::abs(y2 - y1);
      int crop_width = std::abs(x2 - x1);

      // account for flip crops when y1>y2 or x1>x2 with negative striding
      int stride_height = 1, stride_width = 1;
      if (y1 > y2) {
        y1 = y1 - image_height;
        y2 = y2 - image_height - 2;
        stride_height = -1;
      }
      if (x1 > x2) {
        x1 = x1 - image_height;
        x2 = x2 - image_height - 2;
        stride_width = -1;
      }

      auto begin = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::i64, ng::Shape{4},
          std::vector<int64>({static_cast<int64>(box_ind[i]), y1, x1, 0}));
      auto end = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::i64, ng::Shape{4},
          std::vector<int64>({static_cast<int64>(box_ind[i]) + 1, y2 + 1,
                              x2 + 1, static_cast<int64>(image_depth + 1)}));
      auto strides = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::i64, ng::Shape{4},
          std::vector<int64>({1, stride_height, stride_width, 1}));

      // crop
      auto ng_crop = ConstructNgNode<opset::StridedSlice>(
          op->name(), ng_input, begin, end, strides, std::vector<int64_t>{},
          std::vector<int64_t>{});

      opset::Interpolate::InterpolateAttrs interpolate_attrs;
      // always corner aligned
      interpolate_attrs.coordinate_transformation_mode =
          opset::Interpolate::CoordinateTransformMode::align_corners;

      // TODO: handle the case when extrapolation value is greatger than 1.0
      // arguments for resizing
      auto ng_spatial_shape = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::i32, ng::Shape{2},
          std::vector<int32>{crop_height, crop_width});
      auto ng_input_shape = ConstructNgNode<opset::Convert>(
          op->name(), ng_spatial_shape, ng::element::f32);
      auto ng_crop_size = ConstructNgNode<opset::Convert>(op->name(), ng_size,
                                                          ng::element::f32);
      auto ng_scales = ConstructNgNode<opset::Divide>(op->name(), ng_crop_size,
                                                      ng_input_shape);
      auto ng_axes = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::i32, ng::Shape{2}, std::vector<int>({2, 3}));

      if (tf_resize_method == "bilinear") {
        interpolate_attrs.mode = opset::Interpolate::InterpolateMode::linear;
      } else {  // nearest
        interpolate_attrs.mode = opset::Interpolate::InterpolateMode::nearest;
      }

      Transpose<0, 3, 1, 2>(ng_crop);
      auto ng_output = ConstructNgNode<opset::Interpolate>(
          op->name(), ng_crop, ng_size, ng_scales, ng_axes, interpolate_attrs);
      Transpose<0, 2, 3, 1>(ng_output);
      ng_crop_outputs.at(i) = ng_output;
    }

    auto ng_crop_and_resize =
        ConstructNgNode<opset::Concat>(op->name(), ng_crop_outputs, 0);

    SaveNgOp(ng_op_map, op->name(), ng_crop_and_resize);
  }
  return Status::OK();
}

static Status TranslateCumsumOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_x, ng_axis;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_x, ng_axis));
  bool exclusive, reverse;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "exclusive", &exclusive));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "reverse", &reverse));

  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::CumSum>(op->name(), ng_x, ng_axis, exclusive,
                                          reverse));
  return Status::OK();
}

// Translate DepthToSpace op
static Status TranslateDepthToSpaceOp(const Node* op,
                                      const std::vector<const Tensor*>&,
                                      Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  // Get the attributes
  int64 block_size;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "block_size", &block_size));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "DepthToSpace data format is neither NHWC nor NCHW");
  }

  bool is_nhwc = (tf_data_format == "NHWC");

  NHWCtoNCHW(op->name(), is_nhwc, ng_input);
  auto ng_mode = opset::DepthToSpace::DepthToSpaceMode::BLOCKS_FIRST;
  ng::Output<ng::Node> depth_to_space = ConstructNgNode<opset::DepthToSpace>(
      op->name(), ng_input, ng_mode, block_size);
  NCHWtoNHWC(op->name(), is_nhwc, depth_to_space);
  SaveNgOp(ng_op_map, op->name(), depth_to_space);
  return Status::OK();
}

static Status TranslateDepthwiseConv2dNativeOp(
    const Node* op, const std::vector<const Tensor*>&,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_filter;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_filter));

  std::vector<int32> tf_strides;
  std::vector<int32> tf_dilations;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "DepthwiseConv2D data format is neither NHWC nor NCHW");
  }

  bool is_nhwc = (tf_data_format == "NHWC");

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_dilations);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(2);
  ng::Strides ng_dilations(2);
  ng::Shape ng_image_shape(2);
  ng::Shape ng_kernel_shape(2);

  NHWCtoHW(is_nhwc, ng_input.get_shape(), ng_image_shape);
  NHWCtoHW(is_nhwc, tf_strides, ng_strides);
  NHWCtoHW(is_nhwc, tf_dilations, ng_dilations);
  NHWCtoNCHW(op->name(), is_nhwc, ng_input);

  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
  OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

  auto& ng_filter_shape = ng_filter.get_shape();
  ng_kernel_shape[0] = ng_filter_shape[0];
  ng_kernel_shape[1] = ng_filter_shape[1];

  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::CoordinateDiff ng_padding_below;
  ng::CoordinateDiff ng_padding_above;
  Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                       ng_strides, ng_dilations, ng_padding_below,
                       ng_padding_above);

  // H W I M -> H W I 1 M
  auto filter_shape = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::u64, ng::Shape{5},
      ngraph::Shape{ng_filter_shape[0], ng_filter_shape[1], ng_filter_shape[2],
                    1, ng_filter_shape[3]});
  auto reshaped_filter = ConstructNgNode<opset::Reshape>(op->name(), ng_filter,
                                                         filter_shape, false);

  // H W I 1 M -> I M 1 H W
  auto order = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{5}, vector<int64>{2, 4, 3, 0, 1});
  auto transposed_filter =
      ConstructNgNode<opset::Transpose>(op->name(), reshaped_filter, order);

  auto ng_conv = ConstructNgNode<opset::GroupConvolution>(
      op->name(), ng_input, transposed_filter, ng_strides, ng_padding_below,
      ng_padding_above, ng_dilations);

  NCHWtoNHWC(op->name(), is_nhwc, ng_conv);
  SaveNgOp(ng_op_map, op->name(), ng_conv);
  return Status::OK();
}

static Status TranslateEluOp(const Node* op,
                             const std::vector<const Tensor*>& static_input_map,
                             Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  // No alpha in TF, so default to 1.0
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::Elu>(op->name(), ng_input, 1.0));
  return Status::OK();
}

static Status TranslateExpandDimsOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));
  std::vector<int64> dims;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &dims));
  auto ng_dims = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ngraph::Shape{dims.size()}, dims);
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::Unsqueeze>(op->name(), ng_input, ng_dims));
  return Status::OK();
}

static Status TranslateFakeQuantWithMinMaxVarsOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_min, ng_max;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 1, ng_min));
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 2, ng_max));

  bool narrow_range = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "narrow_range", &narrow_range));
  int64 num_bits;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "num_bits", &num_bits));

  auto levels = std::pow(2, num_bits) - int(narrow_range);

  auto min_less_max = ConstructNgNode<opset::Less>(
      op->name() + "/if_min_less_max", ng_min, ng_max);
  auto minimum = ConstructNgNode<opset::Select>(op->name() + "/minimum",
                                                min_less_max, ng_min, ng_max);
  auto maximum = ConstructNgNode<opset::Select>(op->name() + "/maximum",
                                                min_less_max, ng_max, ng_min);

  auto zero =
      ConstructNgNode<opset::Constant>(op->name(), ng_min.get_element_type(),
                                       ngraph::Shape{}, std::vector<int>({0}));

  auto min_greater_zero = ConstructNgNode<opset::Greater>(
      op->name() + "/if_minimum_greater_zero", minimum, zero);
  auto max_minus_min = ConstructNgNode<opset::Subtract>(
      op->name() + "/max_minus_min", maximum, minimum);
  minimum = ConstructNgNode<opset::Select>(op->name() + "/first_adj_min",
                                           min_greater_zero, zero, minimum);
  maximum = ConstructNgNode<opset::Select>(
      op->name() + "/first_adj_max", min_greater_zero, max_minus_min, maximum);

  auto max_less_zero = ConstructNgNode<opset::Less>(
      op->name() + "/if_max_less_zero", maximum, zero);
  auto min_minus_max = ConstructNgNode<opset::Subtract>(
      op->name() + "/min_minus_max", minimum, maximum);
  minimum = ConstructNgNode<opset::Select>(
      op->name() + "/second_adj_min", max_less_zero, min_minus_max, minimum);
  maximum = ConstructNgNode<opset::Select>(op->name() + "/second_adj_max",
                                           max_less_zero, zero, maximum);

  auto float_range = ConstructNgNode<opset::Subtract>(
      op->name() + "/float_range", maximum, minimum);
  auto quant_min_value = int(narrow_range);
  auto quant_max_value = std::pow(2, num_bits) - 1;
  float value = static_cast<float>(quant_max_value - quant_min_value);
  auto int_range = ConstructNgNode<opset::Constant>(
      op->name() + "/int_range", ng::element::f32, ngraph::Shape{},
      std::vector<float>({value}));
  auto scale = ConstructNgNode<opset::Divide>(op->name() + "/scale",
                                              float_range, int_range);
  auto descaled_min = ConstructNgNode<opset::Divide>(
      op->name() + "/descaled_min", minimum, scale);
  auto rounded_descaled_min = ConstructNgNode<opset::Round>(
      op->name() + "/rounded_descaled_min", descaled_min,
      opset::Round::RoundMode::HALF_TO_EVEN);
  auto min_adj = ConstructNgNode<opset::Multiply>(op->name() + "/min_adj",
                                                  scale, rounded_descaled_min);
  auto adjustment = ConstructNgNode<opset::Subtract>(
      op->name() + "/limits_adjustment", min_adj, minimum);
  auto max_adj =
      ConstructNgNode<opset::Add>(op->name() + "/max_adj", maximum, adjustment);

  auto ng_input_shape = ng_input.get_shape();
  if (ng_input_shape.size() == 4) Transpose<0, 3, 1, 2>(ng_input);
  auto ng_output = ConstructNgNode<opset::FakeQuantize>(
      op->name(), ng_input, min_adj, max_adj, min_adj, max_adj, levels);
  if (ng_input_shape.size() == 4) Transpose<0, 2, 3, 1>(ng_output);

  SaveNgOp(ng_op_map, op->name(), ng_output);

  return Status::OK();
}

static Status TranslateFillOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_value, ng_dims;
  // TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_dims, ng_value));
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_dims));
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 1, ng_value));
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::Broadcast>(op->name(), ng_value, ng_dims));
  return Status::OK();
}

static Status TranslateFloorDivOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  auto floordiv_fn = [&op](ng::Output<ng::Node> x, ng::Output<ng::Node> y) {
    return ConstructNgNode<opset::Floor>(
        op->name(), ConstructNgNode<opset::Divide>(op->name(), x, y));
  };
  return TranslateBinaryOp(op, static_input_map, ng_op_map, floordiv_fn);
}

static Status TranslateFusedBatchNormOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_scale, ng_offset, ng_mean, ng_variance;
  bool is_v3 = op->type_string() == "FusedBatchNormV3";
  bool is_Ex = op->type_string() == "_FusedBatchNormEx";

  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_scale, ng_offset,
                                   ng_mean, ng_variance));

  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "Conv2D data format is neither NHWC nor NCHW");
  }

  bool is_nhwc = (tf_data_format == "NHWC");

  OVTF_VLOG(3) << "data_format: " << tf_data_format;

  float tf_epsilon;
  if (GetNodeAttr(op->attrs(), "epsilon", &tf_epsilon) != Status::OK()) {
    OVTF_VLOG(3) << "epsilon attribute not present, setting to 0.0001";
    // TensorFlow default
    tf_epsilon = 0.0001;
  }

  OVTF_VLOG(3) << "epsilon: " << tf_epsilon;

  NHWCtoNCHW(op->name(), is_nhwc, ng_input);

  auto ng_batch_norm = ConstructNgNode<opset::BatchNormInference>(
      op->name(), ng_input, ng_scale, ng_offset, ng_mean, ng_variance,
      tf_epsilon);
  NCHWtoNHWC(op->name(), is_nhwc, ng_batch_norm);

  if (is_Ex) {
    string activation_mode;
    TF_RETURN_IF_ERROR(
        GetNodeAttr(op->attrs(), "activation_mode", &activation_mode));

    if (activation_mode == "Relu") {
      auto relu_op = ConstructNgNode<opset::Relu>(op->name(), ng_batch_norm);
      SaveNgOp(ng_op_map, op->name(), relu_op);
    } else {
      return errors::Unimplemented(
          "Unsupported _FusedBatchNormEx activation mode in " + op->name());
    }
  } else {
    SaveNgOp(ng_op_map, op->name(), ng_batch_norm);
    SaveNgOp(ng_op_map, op->name(), ng_mean);
    SaveNgOp(ng_op_map, op->name(), ng_variance);
    SaveNgOp(ng_op_map, op->name(), ng_mean);      // reserve_space_1
    SaveNgOp(ng_op_map, op->name(), ng_variance);  // reserve_space_2
    if (is_v3) {
      // FusedBatchNormV3 has 6 outputs
      SaveNgOp(ng_op_map, op->name(), ng_mean);  // reserve_space_3
    }
  }
  return Status::OK();
}

static Status TranslateFusedMatMulOp(const Node* op,
                                     const std::vector<const Tensor*>&,
                                     Builder::OpMap& ng_op_map) {
  int num_args;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "num_args", &num_args));

  std::vector<string> fused_ops;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "fused_ops", &fused_ops));

  // Transpose arguments if requested.
  bool transpose_a = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "transpose_a", &transpose_a));

  bool transpose_b = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "transpose_b", &transpose_b));

  ng::Output<ng::Node> ng_lhs, ng_rhs, ng_bias, ng_matmul;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_lhs, ng_rhs, ng_bias));
  ng_matmul = ConstructNgNode<opset::MatMul>(op->name(), ng_lhs, ng_rhs,
                                             transpose_a, transpose_b);

  auto ng_matmul_shape = ng_matmul.get_shape();
  auto ng_bias_shape = ng_bias.get_shape();

  if (ng_bias_shape.size() != 1) {
    return errors::InvalidArgument(
        "Bias argument to BiasAdd does not have one dimension");
  }

  auto ng_add = ConstructNgNode<opset::Add>(op->name(), ng_matmul, ng_bias);
  if (fused_ops.size() == 1) {  // Only fusing BiasAdd
    SaveNgOp(ng_op_map, op->name(), ng_add);
  } else if (fused_ops.size() == 2) {  // Also has activation
    if (fused_ops[1] == "Relu") {
      SaveNgOp(ng_op_map, op->name(),
               ConstructNgNode<opset::Relu>(op->name(), ng_add));
    } else if (fused_ops[1] == "Relu6") {
      SaveNgOp(ng_op_map, op->name(),
               ConstructNgNode<opset::Clamp>(op->name(), ng_add, 0, 6));
    } else {
      return errors::Internal(
          "Expected activation to be Relu or Relu6 but got ", fused_ops[1]);
    }
  } else {
    // Adding this here to catch future changes in _FusedMatMul
    return errors::Internal("Unsupported combination");
  }

  return Status::OK();
}

// See .../tensorflow/include/tensorflow/cc/ops/array_ops.h
// and .../openvino/ngraph/core/include/ngraph/op/gather.hpp
static Status TranslateGatherOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_input_indices;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_input_indices));

  auto ng_axis = ConstructNgNode<opset::Constant>(op->name(), ng::element::i64,
                                                  ng::Shape{}, 0);

  auto gather_op = ConstructNgNode<opset::Gather>(op->name(), ng_input,
                                                  ng_input_indices, ng_axis);

  SaveNgOp(ng_op_map, op->name(), gather_op);
  return Status::OK();
}

static Status TranslateGatherV2Op(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_input_coords, ng_unused;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_input, ng_input_coords, ng_unused));

  std::vector<int64> tf_axis;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 2, static_input_map, &tf_axis));

  if (tf_axis.size() > 1) {
    return errors::Internal("Found axis in GatherV2 op (", op->name(),
                            ") translation to be non scalar, of size ",
                            tf_axis.size());
  }

  // Negative axis is supported. Accounting for that
  size_t ng_input_rank = ng_input.get_partial_shape().rank().get_length();
  int axis;
  if (tf_axis[0] >= 0) {
    axis = tf_axis[0];
  } else {
    axis = tf_axis[0] + ng_input_rank;
  }
  if (axis < 0 || axis >= ng_input_rank) {
    return errors::InvalidArgument("Expected axis in the range [-",
                                   ng_input_rank, ", ", ng_input_rank,
                                   "), but got ", tf_axis[0]);
  }

  auto ng_axis = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{tf_axis.size()}, tf_axis);

  auto gather_op = ConstructNgNode<opset::Gather>(op->name(), ng_input,
                                                  ng_input_coords, ng_axis);

  SaveNgOp(ng_op_map, op->name(), gather_op);
  return Status::OK();
}

static Status TranslateGatherNdOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_input_indices;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_input_indices));

  int batch_dims = 0;
  // TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "batch_dims", &batch_dims));

  auto gathernd_op = ConstructNgNode<opset::GatherND>(
      op->name(), ng_input, ng_input_indices, batch_dims);

  SaveNgOp(ng_op_map, op->name(), gathernd_op);
  return Status::OK();
}

static Status TranslateFusedConv2DOp(const Node* op,
                                     const std::vector<const Tensor*>&,
                                     Builder::OpMap& ng_op_map) {
  int num_args;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "num_args", &num_args));

  std::vector<string> fused_ops;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "fused_ops", &fused_ops));

  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));
  bool is_nhwc = (tf_data_format == "NHWC");

  auto CreateNgConv = [&](ng::Output<ng::Node>& ng_input,
                          ng::Output<ng::Node>& ng_filter,
                          ng::Output<ng::Node>& ng_conv) {
    std::vector<int32> tf_strides;
    std::vector<int32> tf_dilations;
    std::string tf_padding_type;
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));

    if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
      return errors::InvalidArgument(
          "Conv2D data format is neither NHWC nor NCHW");
    }

    // TF Kernel Test Checks
    // Strides in the batch and depth dimension is not supported
    if (tf_strides[0] != 1 || tf_strides[is_nhwc ? 3 : 1] != 1) {
      return errors::InvalidArgument(
          "Strides in batch and depth dimensions is not supported: ",
          op->type_string());
    }

    OVTF_VLOG(3) << ng::join(tf_strides);
    OVTF_VLOG(3) << ng::join(tf_dilations);
    OVTF_VLOG(3) << tf_padding_type;
    OVTF_VLOG(3) << tf_data_format;

    ng::Strides ng_strides(2);
    ng::Strides ng_dilations(2);
    ng::Shape ng_image_shape(2);
    ng::Shape ng_kernel_shape(2);

    NHWCtoHW(is_nhwc, tf_strides, ng_strides);
    NHWCtoHW(is_nhwc, ng_input.get_shape(), ng_image_shape);
    NHWCtoHW(is_nhwc, tf_dilations, ng_dilations);
    NHWCtoNCHW(op->name(), is_nhwc, ng_input);

    OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
    OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
    OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

    auto& ng_filter_shape = ng_filter.get_shape();
    ng_kernel_shape[0] = ng_filter_shape[0];
    ng_kernel_shape[1] = ng_filter_shape[1];
    Transpose<3, 2, 0, 1>(ng_filter);
    Builder::SetTracingInfo(op->name(), ng_filter);

    OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

    ng::CoordinateDiff ng_padding_below;
    ng::CoordinateDiff ng_padding_above;
    Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                         ng_strides, ng_dilations, ng_padding_below,
                         ng_padding_above);

    ng_conv = ConstructNgNode<opset::Convolution>(
        op->name() + "_FusedConv2D_Conv", ng_input, ng_filter, ng_strides,
        ng_padding_below, ng_padding_above, ng_dilations);

    return Status::OK();
  };

  if (VecStrCmp(fused_ops, {"BiasAdd"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Relu"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Relu6"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "LeakyRelu"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Elu"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Add", "Relu"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Add"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Add", "LeakyRelu"})) {
    ng::Output<ng::Node> ng_input, ng_filter, ng_bias, ng_conv, ng_input2;
    if (VecStrCmp(fused_ops, {"BiasAdd", "Add", "Relu"}) ||
        VecStrCmp(fused_ops, {"BiasAdd", "Add"}) ||
        VecStrCmp(fused_ops, {"BiasAdd", "Add", "LeakyRelu"})) {
      if (num_args != 2) {
        return errors::InvalidArgument(
            "FusedConv2DBiasAdd has incompatible num_args");
      }
      TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_filter,
                                       ng_bias, ng_input2));
    } else {
      if (num_args != 1) {
        return errors::InvalidArgument(
            "FusedConv2DBiasAdd has incompatible num_args");
      }
      TF_RETURN_IF_ERROR(
          GetInputNodes(ng_op_map, op, ng_input, ng_filter, ng_bias));
    }

    TF_RETURN_IF_ERROR(CreateNgConv(ng_input, ng_filter, ng_conv));

    auto ng_conv_shape = ng_conv.get_shape();
    auto ng_bias_shape = ng_bias.get_shape();
    if (ng_bias_shape.size() != 1) {
      return errors::InvalidArgument(
          "Bias argument to BiasAdd does not have one dimension");
    }

    std::vector<size_t> reshape_pattern_values(ng_conv_shape.size(), 1U);
    reshape_pattern_values[1] = ng_bias.get_shape().front();
    auto reshape_pattern = make_shared<opset::Constant>(
        ng::element::u64, ng::Shape{reshape_pattern_values.size()},
        reshape_pattern_values);
    auto ng_bias_reshaped = ConstructNgNode<opset::Reshape>(
        op->name(), ng_bias, reshape_pattern, false);

    auto ng_add = ConstructNgNode<opset::Add>(
        op->name() + "_FusedConv2D_BiasAdd", ng_conv, ng_bias_reshaped);

    if (VecStrCmp(fused_ops, {"BiasAdd", "Relu"})) {
      auto ng_relu = ConstructNgNode<opset::Relu>(
          op->name() + "_FusedConv2D_Relu", ng_add);
      NCHWtoNHWC(op->name(), is_nhwc, ng_relu);
      SaveNgOp(ng_op_map, op->name(), ng_relu);
    } else if (VecStrCmp(fused_ops, {"BiasAdd", "Relu6"})) {
      auto ng_relu6 = ConstructNgNode<opset::Clamp>(
          op->name() + "_FusedConv2D_Relu6", ng_add, 0, 6);
      NCHWtoNHWC(op->name(), is_nhwc, ng_relu6);
      SaveNgOp(ng_op_map, op->name(), ng_relu6);
    } else if (VecStrCmp(fused_ops, {"BiasAdd", "LeakyRelu"})) {
      float tf_leakyrelu_alpha;
      TF_RETURN_IF_ERROR(
          GetNodeAttr(op->attrs(), "leakyrelu_alpha", &tf_leakyrelu_alpha));
      auto ng_leakyrelu_alpha = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::f32, ng::Shape{}, tf_leakyrelu_alpha);
      auto ng_alphax = ConstructNgNode<opset::Multiply>(
          op->name(), ng_leakyrelu_alpha, ng_add);
      auto ng_lrelu = ConstructNgNode<opset::Maximum>(
          op->name() + "_FusedConv2D_LeakyRelu", ng_alphax, ng_add);
      NCHWtoNHWC(op->name(), is_nhwc, ng_lrelu);
      SaveNgOp(ng_op_map, op->name(), ng_lrelu);
    } else if (VecStrCmp(fused_ops, {"BiasAdd", "Elu"})) {
      float tf_elu_alpha = 1.0;
      TF_RETURN_IF_ERROR(
          GetNodeAttr(op->attrs(), "leakyrelu_alpha", &tf_elu_alpha));
      auto ng_elu = ConstructNgNode<opset::Elu>(op->name() + "_FusedConv2D_Elu",
                                                ng_add, tf_elu_alpha);
      NCHWtoNHWC(op->name(), is_nhwc, ng_elu);
      SaveNgOp(ng_op_map, op->name(), ng_elu);
    } else if (VecStrCmp(fused_ops, {"BiasAdd", "Add", "Relu"})) {
      NHWCtoNCHW(op->name(), is_nhwc, ng_input2);
      auto ng_add2 = ConstructNgNode<opset::Add>(
          op->name() + "_FusedConv2D_Add", ng_add, ng_input2);
      auto ng_relu = ConstructNgNode<opset::Relu>(
          op->name() + "_FusedConv2D_Relu", ng_add2);
      NCHWtoNHWC(op->name(), is_nhwc, ng_relu);
      SaveNgOp(ng_op_map, op->name(), ng_relu);
    } else if (VecStrCmp(fused_ops, {"BiasAdd", "Add"})) {
      ng::Output<ng::Node> ng_add_inp;
      // TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 3, ng_add_inp));
      NCHWtoNHWC(op->name(), is_nhwc, ng_add);
      auto ng_out = ConstructNgNode<opset::Add>(
          op->name() + "_FusedConv2D_BiasAdd_Add", ng_add, ng_input2);
      SaveNgOp(ng_op_map, op->name(), ng_out);
    } else if (VecStrCmp(fused_ops, {"BiasAdd", "Add", "LeakyRelu"})) {
      NHWCtoNCHW(op->name(), is_nhwc, ng_input2);
      auto ng_add2 = ConstructNgNode<opset::Add>(
          op->name() + "_FusedConv2D_Add", ng_add, ng_input2);
      float tf_leakyrelu_alpha;
      TF_RETURN_IF_ERROR(
          GetNodeAttr(op->attrs(), "leakyrelu_alpha", &tf_leakyrelu_alpha));
      auto ng_leakyrelu_alpha = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::f32, ng::Shape{}, tf_leakyrelu_alpha);
      auto ng_alphax = ConstructNgNode<opset::Multiply>(
          op->name(), ng_leakyrelu_alpha, ng_add2);
      auto ng_alrelu = ConstructNgNode<opset::Maximum>(
          op->name() + "_FusedConv2D_Add_LeakyRelu", ng_alphax, ng_add2);
      NCHWtoNHWC(op->name(), is_nhwc, ng_alrelu);
      SaveNgOp(ng_op_map, op->name(), ng_alrelu);
    } else {
      NCHWtoNHWC(op->name(), is_nhwc, ng_add);
      SaveNgOp(ng_op_map, op->name(), ng_add);
    }
  } else if (VecStrCmp(fused_ops, {"FusedBatchNorm"}) ||
             VecStrCmp(fused_ops, {"FusedBatchNorm", "Relu"}) ||
             VecStrCmp(fused_ops, {"FusedBatchNorm", "Relu6"}) ||
             VecStrCmp(fused_ops, {"FusedBatchNorm", "LeakyRelu"})) {
    if (num_args != 4) {
      return errors::InvalidArgument(
          "FusedConv2D with FusedBatchNorm has incompatible num_args");
    }

    ng::Output<ng::Node> ng_input, ng_filter, ng_conv, ng_scale, ng_offset,
        ng_mean, ng_variance;
    TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_filter,
                                     ng_scale, ng_offset, ng_mean,
                                     ng_variance));
    TF_RETURN_IF_ERROR(CreateNgConv(ng_input, ng_filter, ng_conv));

    float tf_epsilon;
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "epsilon", &tf_epsilon));

    auto ng_batch_norm = ConstructNgNode<opset::BatchNormInference>(
        op->name() + "_FusedConv2D_BatchNorm", ng_conv, ng_scale, ng_offset,
        ng_mean, ng_variance, tf_epsilon);

    if (VecStrCmp(fused_ops, {"FusedBatchNorm", "Relu"})) {
      auto ng_relu = ConstructNgNode<opset::Relu>(
          op->name() + "_FusedConv2D_BatchNormRelu", ng_batch_norm);
      NCHWtoNHWC(op->name(), is_nhwc, ng_relu);
      SaveNgOp(ng_op_map, op->name(), ng_relu);
    } else if (VecStrCmp(fused_ops, {"FusedBatchNorm", "Relu6"})) {
      auto ng_relu6 = ConstructNgNode<opset::Clamp>(
          op->name() + "_FusedConv2D_BatchNormRelu", ng_batch_norm, 0, 6);
      NCHWtoNHWC(op->name(), is_nhwc, ng_relu6);
      SaveNgOp(ng_op_map, op->name(), ng_relu6);
    } else if (VecStrCmp(fused_ops, {"FusedBatchNorm", "LeakyRelu"})) {
      float tf_leakyrelu_alpha;
      TF_RETURN_IF_ERROR(
          GetNodeAttr(op->attrs(), "leakyrelu_alpha", &tf_leakyrelu_alpha));
      auto ng_leakyrelu_alpha = ConstructNgNode<opset::Constant>(
          op->name(), ng::element::f32, ng::Shape{}, tf_leakyrelu_alpha);
      auto ng_alphax = ConstructNgNode<opset::Multiply>(
          op->name(), ng_leakyrelu_alpha, ng_batch_norm);
      auto ng_lrelu = ConstructNgNode<opset::Maximum>(
          op->name() + "_FusedConv2D_BatchNormLeakyRelu", ng_alphax,
          ng_batch_norm);
      NCHWtoNHWC(op->name(), is_nhwc, ng_lrelu);
      SaveNgOp(ng_op_map, op->name(), ng_lrelu);
    } else {
      NCHWtoNHWC(op->name(), is_nhwc, ng_batch_norm);
      SaveNgOp(ng_op_map, op->name(), ng_batch_norm);
    }
  } else {
    return errors::Unimplemented("Unsupported _FusedConv2D " +
                                 absl::StrJoin(fused_ops, ","));
  }
  return Status::OK();
}

static Status TranslateFusedDepthwiseConv2dNativeOp(
    const Node* op, const std::vector<const Tensor*>&,
    Builder::OpMap& ng_op_map) {
  int num_args;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "num_args", &num_args));

  std::vector<string> fused_ops;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "fused_ops", &fused_ops));

  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));
  bool is_nhwc = (tf_data_format == "NHWC");

  auto CreateNgDepthwiseConv = [&](ng::Output<ng::Node>& ng_input,
                                   ng::Output<ng::Node>& ng_filter,
                                   ng::Output<ng::Node>& ng_conv) {
    std::vector<int32> tf_strides;
    std::vector<int32> tf_dilations;
    std::string tf_padding_type;
    std::string tf_data_format;
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "dilations", &tf_dilations));
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
    TF_RETURN_IF_ERROR(
        GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

    if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
      return errors::InvalidArgument(
          "DepthwiseConv2D data format is neither NHWC nor NCHW");
    }

    bool is_nhwc = (tf_data_format == "NHWC");

    OVTF_VLOG(3) << ng::join(tf_strides);
    OVTF_VLOG(3) << ng::join(tf_dilations);
    OVTF_VLOG(3) << tf_padding_type;
    OVTF_VLOG(3) << tf_data_format;

    ng::Strides ng_strides(2);
    ng::Strides ng_dilations(2);
    ng::Shape ng_image_shape(2);
    ng::Shape ng_kernel_shape(2);

    NHWCtoHW(is_nhwc, ng_input.get_shape(), ng_image_shape);
    NHWCtoHW(is_nhwc, tf_strides, ng_strides);
    NHWCtoHW(is_nhwc, tf_dilations, ng_dilations);
    NHWCtoNCHW(op->name(), is_nhwc, ng_input);

    OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
    OVTF_VLOG(3) << "ng_dilations: " << ng::join(ng_dilations);
    OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);

    auto& ng_filter_shape = ng_filter.get_shape();
    ng_kernel_shape[0] = ng_filter_shape[0];
    ng_kernel_shape[1] = ng_filter_shape[1];

    OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

    ng::CoordinateDiff ng_padding_below;
    ng::CoordinateDiff ng_padding_above;
    Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                         ng_strides, ng_dilations, ng_padding_below,
                         ng_padding_above);

    // H W I M -> H W I 1 M
    auto filter_shape = ConstructNgNode<opset::Constant>(
        op->name(), ng::element::u64, ng::Shape{5},
        ngraph::Shape{ng_filter_shape[0], ng_filter_shape[1],
                      ng_filter_shape[2], 1, ng_filter_shape[3]});
    auto reshaped_filter = ConstructNgNode<opset::Reshape>(
        op->name(), ng_filter, filter_shape, false);

    // H W I 1 M -> I M 1 H W
    auto order = ConstructNgNode<opset::Constant>(op->name(), ng::element::i64,
                                                  ng::Shape{5},
                                                  vector<int64>{2, 4, 3, 0, 1});
    auto transposed_filter =
        ConstructNgNode<opset::Transpose>(op->name(), reshaped_filter, order);

    ng_conv = ConstructNgNode<opset::GroupConvolution>(
        op->name(), ng_input, transposed_filter, ng_strides, ng_padding_below,
        ng_padding_above, ng_dilations);

    return Status::OK();
  };

  if (VecStrCmp(fused_ops, {"BiasAdd"}) ||
      VecStrCmp(fused_ops, {"BiasAdd", "Relu6"})) {
    if (num_args != 1) {
      return errors::InvalidArgument(
          "FusedDepthwiseConv2dNativeBiasAdd has incompatible num_args");
    }

    ng::Output<ng::Node> ng_input, ng_filter, ng_bias, ng_conv;
    TF_RETURN_IF_ERROR(
        GetInputNodes(ng_op_map, op, ng_input, ng_filter, ng_bias));

    TF_RETURN_IF_ERROR(CreateNgDepthwiseConv(ng_input, ng_filter, ng_conv));

    auto ng_conv_shape = ng_conv.get_shape();
    auto ng_bias_shape = ng_bias.get_shape();
    if (ng_bias_shape.size() != 1) {
      return errors::InvalidArgument(
          "Bias argument to BiasAdd does not have one dimension");
    }

    std::vector<size_t> reshape_pattern_values(ng_conv_shape.size(), 1U);
    reshape_pattern_values[1] = ng_bias.get_shape().front();
    auto reshape_pattern = make_shared<opset::Constant>(
        ng::element::u64, ng::Shape{reshape_pattern_values.size()},
        reshape_pattern_values);
    auto ng_bias_reshaped = ConstructNgNode<opset::Reshape>(
        op->name(), ng_bias, reshape_pattern, false);

    auto ng_add = ConstructNgNode<opset::Add>(
        op->name() + "_FusedDepthwiseConv2dNative_BiasAdd", ng_conv,
        ng_bias_reshaped);

    if (VecStrCmp(fused_ops, {"BiasAdd", "Relu6"})) {
      auto ng_relu6 = ConstructNgNode<opset::Clamp>(
          op->name() + "_FusedDepthwiseConv2dNative_Relu6", ng_add, 0, 6);
      NCHWtoNHWC(op->name(), is_nhwc, ng_relu6);
      SaveNgOp(ng_op_map, op->name(), ng_relu6);
    } else {
      NCHWtoNHWC(op->name(), is_nhwc, ng_add);
      SaveNgOp(ng_op_map, op->name(), ng_add);
    }
  } else {
    return errors::Unimplemented("Unsupported _FusedDepthwiseConv2dNative " +
                                 absl::StrJoin(fused_ops, ","));
  }
  return Status::OK();
}

static Status TranslateIdentityOp(const Node* op,
                                  const std::vector<const Tensor*>&,
                                  Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_arg;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_arg));
  SaveNgOp(ng_op_map, op->name(), ng_arg);
  return Status::OK();
}

static Status TranslateIsFiniteOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  // Implemented tf.is_finite by checking:
  // (in != inf) && (in != -inf) && (in == in)
  //                                 ^^^^^^^^ checks for NaN's
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  auto const_inf = ConstructNgNode<opset::Constant>(
      op->name(), ng_input.get_element_type(), ng::Shape{},
      std::vector<float>{std::numeric_limits<float>::infinity()});

  auto const_neg_inf = ConstructNgNode<opset::Constant>(
      op->name(), ng_input.get_element_type(), ng::Shape{},
      std::vector<float>{-std::numeric_limits<float>::infinity()});

  auto neq_inf =
      ConstructNgNode<opset::NotEqual>(op->name(), ng_input, const_inf);
  auto neq_neg_inf =
      ConstructNgNode<opset::NotEqual>(op->name(), ng_input, const_neg_inf);
  auto eq_nan = ConstructNgNode<opset::Equal>(op->name(), ng_input, ng_input);

  auto neq_inf_and_neq_neg_inf =
      ConstructNgNode<opset::LogicalAnd>(op->name(), neq_inf, neq_neg_inf);
  auto is_finite = ConstructNgNode<opset::LogicalAnd>(
      op->name(), neq_inf_and_neq_neg_inf, eq_nan);

  SaveNgOp(ng_op_map, op->name(), is_finite);
  return Status::OK();
}

static Status TranslateL2LossOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  std::vector<float> val;
  val.push_back(2.0);
  auto const_2 = ConstructNgNode<opset::Constant>(
      op->name(), ng_input.get_element_type(), ng::Shape{}, val[0]);

  auto ng_pow =
      ConstructNgNode<opset::Multiply>(op->name(), ng_input, ng_input);

  size_t input_rank = ng_input.get_shape().size();
  std::vector<int64> axes;
  for (size_t i = 0; i < input_rank; ++i) {
    axes.push_back(i);
  }

  auto ng_reduction_axes = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{axes.size()}, axes);
  auto ng_sum =
      ConstructNgNode<opset::ReduceSum>(op->name(), ng_pow, ng_reduction_axes);
  auto ng_l2loss = ConstructNgNode<opset::Divide>(op->name(), ng_sum, const_2);
  SaveNgOp(ng_op_map, op->name(), ng_l2loss);
  return Status::OK();
}

static Status TranslateLog1pOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return TranslateUnaryOp(
      op, static_input_map, ng_op_map, [&op](ng::Output<ng::Node> n) {
        auto et = n.get_element_type();
        auto shape = n.get_shape();
        std::vector<std::string> val_1(ng::shape_size(shape), "1");
        auto ng_const1 =
            ConstructNgNode<opset::Constant>(op->name(), et, shape, val_1);
        auto ng_add = ConstructNgNode<opset::Add>(op->name(), ng_const1, n);
        return ConstructNgNode<opset::Log>(op->name(), ng_add);
      });
}

static Status TranslateLRNOp(const Node* op,
                             const std::vector<const Tensor*>& static_input_map,
                             Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_inp;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_inp));

  float alpha;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "alpha", &alpha));
  float beta;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "beta", &beta));
  float bias;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "bias", &bias));
  int64 depth_radius;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "depth_radius", &depth_radius));

  // OV: Each input value is divided by (bias+(alpha/size)*sum(xi^2 for every xi
  // in the local region))^beta
  // TF: sqr_sum[a, b, c, d] = sum(input[a, b, c, d - depth_radius : d +
  // depth_radius + 1] ** 2)
  //     output = input / (bias + alpha * sqr_sum) ** beta
  int64 size = depth_radius * 2 + 1;
  alpha = alpha * size;
  // nGraph expects the input to be in NCHW format
  NHWCtoNCHW(op->name(), true, ng_inp);
  auto ng_output = ConstructNgNode<opset::LRN>(op->name(), ng_inp, alpha, beta,
                                               bias, (size_t)size);
  NCHWtoNHWC(op->name(), true, ng_output);
  SaveNgOp(ng_op_map, op->name(), ng_output);
  return Status::OK();
}

static Status TranslateLogSoftmaxOp(const Node* op,
                                    const std::vector<const Tensor*>&,
                                    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_inp;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_inp));
  auto inp_shape = ng_inp.get_shape();
  size_t rank = inp_shape.size();
  int64 axes = rank - 1;

  auto ng_output = ConstructNgNode<opset::LogSoftmax>(op->name(), ng_inp, axes);
  SaveNgOp(ng_op_map, op->name(), ng_output);
  return Status::OK();
}

static Status TranslateLeakyReluOp(const Node* op,
                                   const std::vector<const Tensor*>&,
                                   Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_inp;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_inp));
  float alpha = 0.0;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "alpha", &alpha));

  auto ng_alpha = ConstructNgNode<opset::Constant>(op->name(), ng::element::f32,
                                                   ng::Shape{1}, alpha);

  auto ng_output = ConstructNgNode<opset::PRelu>(op->name(), ng_inp, ng_alpha);
  SaveNgOp(ng_op_map, op->name(), ng_output);
  return Status::OK();
}

static Status TranslateMatMulOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_lhs, ng_rhs;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_lhs, ng_rhs));

  // Transpose arguments if requested.
  bool transpose_a = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "transpose_a", &transpose_a));

  bool transpose_b = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "transpose_b", &transpose_b));

  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::MatMul>(op->name(), ng_lhs, ng_rhs,
                                          transpose_a, transpose_b));
  return Status::OK();
}

template <unsigned int N>
static Status TranslateMaxPoolOp(const Node* op,
                                 const std::vector<const Tensor*>&,
                                 Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  std::vector<int32> tf_strides;
  std::vector<int32> tf_ksize;
  std::string tf_padding_type;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "strides", &tf_strides));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "ksize", &tf_ksize));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "padding", &tf_padding_type));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  bool is_nhwc = (tf_data_format == "NHWC") || (tf_data_format == "NDHWC");

  OVTF_VLOG(3) << ng::join(tf_strides);
  OVTF_VLOG(3) << ng::join(tf_ksize);
  OVTF_VLOG(3) << tf_padding_type;
  OVTF_VLOG(3) << tf_data_format;

  ng::Strides ng_strides(N);
  ng::Shape ng_image_shape(N);
  ng::Shape ng_kernel_shape(N);
  ng::Shape ng_dilations(N, 1);

  NHWCtoHW(is_nhwc, tf_strides, ng_strides);
  NHWCtoHW(is_nhwc, ng_input.get_shape(), ng_image_shape);
  NHWCtoHW(is_nhwc, tf_ksize, ng_kernel_shape);
  NHWCtoNCHW(op->name(), is_nhwc, ng_input);
  OVTF_VLOG(3) << "ng_strides: " << ng::join(ng_strides);
  OVTF_VLOG(3) << "ng_image_shape: " << ng::join(ng_image_shape);
  OVTF_VLOG(3) << "ng_kernel_shape: " << ng::join(ng_kernel_shape);

  ng::CoordinateDiff padding_below;
  ng::CoordinateDiff padding_above;
  Builder::MakePadding(tf_padding_type, ng_image_shape, ng_kernel_shape,
                       ng_strides, ng_dilations, padding_below, padding_above);

  // TODO: remove this once nGraph supports negative padding
  // (CoordinateDiff) for MaxPool
  ng::Shape ng_padding_below(padding_below.begin(), padding_below.end());
  ng::Shape ng_padding_above(padding_above.begin(), padding_above.end());

  auto ng_maxpool = ConstructNgNode<opset::MaxPool>(
      op->name(), ng_input, ng_strides, ng_padding_below, ng_padding_above,
      ng_kernel_shape, ng::op::RoundingType::FLOOR);

  NCHWtoNHWC(op->name(), is_nhwc, ng_maxpool);

  OVTF_VLOG(3) << "maxpool outshape: {" << ng::join(ng_maxpool.get_shape())
               << "}";

  SaveNgOp(ng_op_map, op->name(), ng_maxpool);
  return Status::OK();
}

static Status TranslateNonMaxSuppressionV2Op(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_boxes, ng_scores, ng_unused, ng_iou_threshold;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_boxes, ng_scores,
                                   ng_unused, ng_iou_threshold));

  auto ng_axis_boxes = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{1}, std::vector<int64>({0}));
  auto ng_boxes_unsqueezed =
      ConstructNgNode<opset::Unsqueeze>(op->name(), ng_boxes, ng_axis_boxes);

  auto ng_axis_scores = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{1}, std::vector<int64>({0}));
  auto ng_scores_unsqueezed1 =
      ConstructNgNode<opset::Unsqueeze>(op->name(), ng_scores, ng_axis_scores);
  auto ng_scores_unsqueezed2 = ConstructNgNode<opset::Unsqueeze>(
      op->name(), ng_scores_unsqueezed1, ng_axis_scores);

  std::vector<int> max_output_size;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 2, static_input_map, &max_output_size));

  // max_output_size must be scalar
  if (max_output_size.size() != 1) {
    return errors::InvalidArgument(
        "NonMaxSuppression Op: max_output_size of nms must be scalar ",
        max_output_size.size());
  }

  auto ng_max_output_size = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{}, max_output_size[0]);
  OVTF_VLOG(5) << "ng_max_output_size " << max_output_size[0];

  auto ng_nmsv = ConstructNgNode<opset::NonMaxSuppression>(
      op->name(), ng_boxes_unsqueezed, ng_scores_unsqueezed2,
      ng_max_output_size, ng_iou_threshold,
      opset::NonMaxSuppression::BoxEncodingType::CORNER, false,
      ngraph::element::Type_t::i32);

  auto begin = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{2}, std::vector<int64>({0, 2}));
  auto end = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{2},
      std::vector<int64>({max_output_size[0], 3}));
  auto ng_nmsv_slice = ConstructNgNode<opset::StridedSlice>(
      op->name(), ng_nmsv, begin, end, std::vector<int64_t>{0, 0},
      std::vector<int64_t>{0, 0}, std::vector<int64_t>{0, 0},
      std::vector<int64_t>{0, 1});

  Builder::SetTracingInfo(op->name(), ng_nmsv_slice);
  SaveNgOp(ng_op_map, op->name(), ng_nmsv_slice);
  return Status::OK();
}

static Status TranslateNonMaxSuppressionV3Op(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_boxes, ng_scores, ng_unused, ng_iou_threshold,
      ng_score_threshold;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_boxes, ng_scores,
                                   ng_unused, ng_iou_threshold,
                                   ng_score_threshold));

  auto ng_axis_boxes = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{1}, std::vector<int64>({0}));
  auto ng_boxes_unsqueezed =
      ConstructNgNode<opset::Unsqueeze>(op->name(), ng_boxes, ng_axis_boxes);

  auto ng_axis_scores = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{1}, std::vector<int64>({0}));
  auto ng_scores_unsqueezed1 =
      ConstructNgNode<opset::Unsqueeze>(op->name(), ng_scores, ng_axis_scores);
  auto ng_scores_unsqueezed2 = ConstructNgNode<opset::Unsqueeze>(
      op->name(), ng_scores_unsqueezed1, ng_axis_scores);

  std::vector<int> max_output_size;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 2, static_input_map, &max_output_size));

  // max_output_size must be scalar
  if (max_output_size.size() != 1) {
    return errors::InvalidArgument(
        "NonMaxSuppression Op: max_output_size of nms must be scalar ",
        max_output_size.size());
  }

  auto ng_max_output_size = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{}, max_output_size[0]);
  OVTF_VLOG(5) << "ng_max_output_size " << max_output_size[0];

  auto ng_nmsv = ConstructNgNode<opset::NonMaxSuppression>(
      op->name(), ng_boxes_unsqueezed, ng_scores_unsqueezed2,
      ng_max_output_size, ng_iou_threshold, ng_score_threshold,
      opset::NonMaxSuppression::BoxEncodingType::CORNER, false,
      ngraph::element::Type_t::i32);

  auto begin = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{2}, std::vector<int64>({0, 2}));
  auto end = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{2},
      std::vector<int64>({max_output_size[0], 3}));
  auto ng_nmsv_slice = ConstructNgNode<opset::StridedSlice>(
      op->name(), ng_nmsv, begin, end, std::vector<int64_t>{0, 0},
      std::vector<int64_t>{0, 0}, std::vector<int64_t>{0, 0},
      std::vector<int64_t>{0, 1});

  Builder::SetTracingInfo(op->name(), ng_nmsv_slice);
  SaveNgOp(ng_op_map, op->name(), ng_nmsv_slice);
  return Status::OK();
}

static Status TranslateReduceOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map,
    std::function<ng::Output<ng::Node>(ng::Output<ng::Node>,
                                       ng::Output<ng::Node>, const bool)>
        create_ng_node) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));
  bool tf_keep_dims;
  if (GetNodeAttr(op->attrs(), "keep_dims", &tf_keep_dims) != Status::OK()) {
    tf_keep_dims = false;
  }

  std::vector<int64> axes;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &axes));

  size_t input_rank = ng_input.get_partial_shape().rank().get_length();

  TF_RETURN_IF_ERROR(CheckAxisDimInRange(axes, input_rank));

  std::vector<size_t> ng_reduction_axes_vect(axes.size());
  std::transform(
      axes.begin(), axes.end(), ng_reduction_axes_vect.begin(),
      [input_rank](int idx) { return idx + (idx < 0 ? (int)input_rank : 0); });
  auto ng_reduction_axes = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{ng_reduction_axes_vect.size()},
      ng_reduction_axes_vect);

  ng::Output<ng::Node> ng_node =
      create_ng_node(ng_input, ng_reduction_axes, tf_keep_dims);

  SaveNgOp(ng_op_map, op->name(), ng_node);
  return Status::OK();
}

template <typename T>
static Status TranslateDirectReduceOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  // ensure its either an arithmetic or a logical reduction
  if (!(std::is_base_of<ngraph::op::util::ArithmeticReduction, T>::value ||
        std::is_base_of<ngraph::op::util::LogicalReduction, T>::value)) {
    return errors::InvalidArgument(
        "Expected node to be either a valid logical or arithmetic reduction "
        "type");
  }
  return TranslateReduceOp(
      op, static_input_map, ng_op_map,
      [&op](ng::Output<ng::Node> ng_input,
            ng::Output<ng::Node> ng_reduction_axes, const bool keep_dims) {
        return ConstructNgNode<T>(op->name(), ng_input, ng_reduction_axes,
                                  keep_dims);
      });
}

static Status TranslateOneHotOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_features, ng_unused, ng_on, ng_off, ng_depth;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_features, ng_unused, ng_on, ng_off));

  auto ng_features_shape = ng_features.get_shape();
  std::vector<int> depth;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &depth));

  // Depth must be scalar
  if (depth.size() != 1) {
    return errors::InvalidArgument(
        "OneHot Op: depth of one hot dimension must be scalar ", depth.size());
  }

  auto const_depth = ConstructNgNode<ng::op::Constant>(
      op->name(), ng::element::i64, ng::Shape{}, depth);

  int one_hot_axis;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "axis", &one_hot_axis));

  auto ng_onehot = ConstructNgNode<opset::OneHot>(
      op->name(), ng_features, const_depth, ng_on, ng_off, one_hot_axis);
  SaveNgOp(ng_op_map, op->name(), ng_onehot);
  return Status::OK();
}

static Status TranslatePackOp(const Node* op, const std::vector<const Tensor*>&,
                              Builder::OpMap& ng_op_map) {
  TF_RETURN_IF_ERROR(ValidateInputCountMin(op, 1));

  int32 tf_axis;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "axis", &tf_axis));
  auto ng_axis = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{1},
      std::vector<int64>({tf_axis}));

  ng::OutputVector ng_concat_inputs;
  for (tensorflow::int32 i = 0; i < op->num_inputs(); ++i) {
    ng::Output<ng::Node> ng_input;
    TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, i, ng_input));
    auto unsqueezed_input =
        ConstructNgNode<opset::Unsqueeze>(op->name(), ng_input, ng_axis);
    ng_concat_inputs.push_back(unsqueezed_input);
  }

  // if inputs shape is (2, 3, 4), and axis is 1, then we want
  // to create output_shape (2, num_inputs, 3, 4)
  SaveNgOp(ng_op_map, op->name(), ConstructNgNode<opset::Concat>(
                                      op->name(), ng_concat_inputs, tf_axis));
  return Status::OK();
}

// 3 different Pad Ops: Pad, PadV2, MirrorPad
// See https://www.tensorflow.org/api_docs/cc/class/tensorflow/ops/pad
// See https://www.tensorflow.org/api_docs/cc/class/tensorflow/ops/pad-v2
// See https://www.tensorflow.org/api_docs/cc/class/tensorflow/ops/mirror-pad
static Status TranslatePadOp(const Node* op,
                             const std::vector<const Tensor*>& static_input_map,
                             Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_paddings_op, pad_val_op, result_pad_op;

  // Set inputs and pad_val_op
  if (op->type_string() == "Pad" || op->type_string() == "MirrorPad") {
    TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_paddings_op));
    pad_val_op = ConstructNgNode<opset::Constant>(
        op->name(), ng_input.get_element_type(), ng::Shape(),
        std::vector<int>({0}));
  } else if (op->type_string() == "PadV2") {
    TF_RETURN_IF_ERROR(
        GetInputNodes(ng_op_map, op, ng_input, ng_paddings_op, pad_val_op));
  } else {
    return errors::InvalidArgument("Incorrect TF Pad OpType: " +
                                   op->type_string());
  }

  // Set pad_mode
  auto pad_mode = ng::op::PadMode::CONSTANT;
  if (op->type_string() == "MirrorPad") {
    std::string pad_mode_str;
    TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "mode", &pad_mode_str));
    if (pad_mode_str == "REFLECT") {
      pad_mode = ng::op::PadMode::REFLECT;
    } else if (pad_mode_str == "SYMMETRIC") {
      pad_mode = ng::op::PadMode::SYMMETRIC;
    } else {
      return errors::InvalidArgument(pad_mode_str,
                                     " is not an allowed padding mode.");
    }
  }

  // Set pads_begin & pads_end (from the pad_val_op)
  std::vector<int64> paddings;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &paddings));
  OVTF_VLOG(3) << op->name() << " pads {" << ng::join(paddings) << "}";
  if (paddings.size() % 2 != 0) {
    return errors::InvalidArgument(
        "Constant node for paddings does not have an even number of "
        "elements");
  }
  std::vector<int64> pad_begin(paddings.size() / 2);
  std::vector<int64> pad_end(paddings.size() / 2);
  for (size_t i = 0; i < paddings.size() / 2; i++) {
    pad_begin[i] = paddings[2 * i];
    pad_end[i] = paddings[2 * i + 1];
  }
  auto pads_begin_node = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{pad_begin.size()}, pad_begin);
  auto pads_end_node = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{pad_end.size()}, pad_end);

  // Create final Op
  result_pad_op =
      ConstructNgNode<opset::Pad>(op->name(), ng_input, pads_begin_node,
                                  pads_end_node, pad_val_op, pad_mode);

  SaveNgOp(ng_op_map, op->name(), result_pad_op);
  return Status::OK();
}

static Status TranslateRangeOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_start, ng_stop, ng_step;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_start, ng_stop, ng_step));

  DataType start_type = op->input_type(0);
  DataType stop_type = op->input_type(1);
  DataType step_type = op->input_type(2);
  ng::element::Type out_type;
  TF_RETURN_IF_ERROR(
      util::TFDataTypeToNGraphElementType(op->output_type(0), &out_type));
  ng::Output<ng::Node> start_node, stop_node, step_node;
  TF_RETURN_IF_ERROR(
      GetStaticInputNode(op, 0, static_input_map, start_type, start_node));
  TF_RETURN_IF_ERROR(
      GetStaticInputNode(op, 1, static_input_map, stop_type, stop_node));
  TF_RETURN_IF_ERROR(
      GetStaticInputNode(op, 2, static_input_map, step_type, step_node));
  auto ng_range = ConstructNgNode<opset::Range>(op->name(), start_node,
                                                stop_node, step_node, out_type);

  SaveNgOp(ng_op_map, op->name(), ng_range);
  return Status::OK();
}

static Status TranslateRankOp(const Node* op, const std::vector<const Tensor*>&,
                              Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  auto input_rank =
      static_cast<int>(ng_input.get_partial_shape().rank().get_length());

  auto ng_rank = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i32, ng::Shape(),
      std::vector<int>({input_rank}));

  SaveNgOp(ng_op_map, op->name(), ng_rank);
  return Status::OK();
}

static Status TranslateReciprocalOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return TranslateUnaryOp(
      op, static_input_map, ng_op_map, [&op](ng::Output<ng::Node> n) {
        // Create a constant tensor populated with the value -1.
        // (1/x = x^(-1))
        auto et = n.get_element_type();
        auto shape = n.get_shape();
        std::vector<std::string> constant_values(ng::shape_size(shape), "-1");
        auto ng_exponent = ConstructNgNode<opset::Constant>(
            op->name(), et, shape, constant_values);

        // Raise each element of the input to the power -1.
        return ConstructNgNode<opset::Power>(op->name(), n, ng_exponent);
      });
}

static Status TranslateRelu6Op(const Node* op,
                               const std::vector<const Tensor*>&,
                               Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));
  auto ng_input_shape = ng_input.get_shape();
  std::string device;
  // Enable transpose before and after only for CPU device
  BackendManager::GetBackendName(device);
  if (device == "CPU") {
    if (ng_input_shape.size() == 4) Transpose<0, 3, 1, 2>(ng_input);
  }
  auto ng_output = ConstructNgNode<opset::Clamp>(op->name(), ng_input, 0, 6);
  if (device == "CPU") {
    if (ng_input_shape.size() == 4) Transpose<0, 2, 3, 1>(ng_output);
  }
  SaveNgOp(ng_op_map, op->name(), ng_output);

  return Status::OK();
}

static Status TranslateReshapeOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_shape_op;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_shape_op));

  OVTF_VLOG(3) << "Input shape: " << ng::join(ng_input.get_shape());

  std::vector<int64> shape;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &shape));

  OVTF_VLOG(3) << "Requested result shape: " << ng::join(shape);

  auto ng_shape = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{shape.size()}, shape);
  SaveNgOp(ng_op_map, op->name(), ConstructNgNode<opset::Reshape>(
                                      op->name(), ng_input, ng_shape, false));
  return Status::OK();
}

static Status TranslateRoundOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  // using default round mode "half_to_even" in openvino,
  // as TF has only that mode
  opset::Round::RoundMode round_mode = opset::Round::RoundMode::HALF_TO_EVEN;
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::Round>(op->name(), ng_input, round_mode));
  return Status::OK();
}

static Status TranslateResizeBilinearOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_inp, ng_inp_sizes;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_inp, ng_inp_sizes));

  // Get Interpolate attributes
  using InterpolateV4Attrs = opset::Interpolate::InterpolateAttrs;
  InterpolateV4Attrs interpolate_attrs;
  interpolate_attrs.mode = opset::Interpolate::InterpolateMode::linear;
  interpolate_attrs.shape_calculation_mode =
      opset::Interpolate::ShapeCalcMode::sizes;
  bool align_corners = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "align_corners", &align_corners));
  if (align_corners)
    interpolate_attrs.coordinate_transformation_mode =
        opset::Interpolate::CoordinateTransformMode::align_corners;

  auto input_shape = ng_inp.get_shape();
  std::vector<uint64_t> spatial_shape = {input_shape[1], input_shape[2]};
  auto ng_spatial_shape = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i32, ng::Shape{2}, spatial_shape);
  auto ng_input_shape = ConstructNgNode<opset::Convert>(
      op->name(), ng_spatial_shape, ng::element::f32);
  auto ng_sizes = ConstructNgNode<opset::Convert>(op->name(), ng_inp_sizes,
                                                  ng::element::f32);
  auto ng_scales =
      ConstructNgNode<opset::Divide>(op->name(), ng_sizes, ng_input_shape);
  auto ng_axes = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i32, ng::Shape{2}, std::vector<int>({2, 3}));

  Transpose<0, 3, 1, 2>(ng_inp);
  auto ng_output = ConstructNgNode<opset::Interpolate>(
      op->name(), ng_inp, ng_inp_sizes, ng_scales, ng_axes, interpolate_attrs);
  Transpose<0, 2, 3, 1>(ng_output);
  SaveNgOp(ng_op_map, op->name(), ng_output);
  return Status::OK();
}

static Status TranslateResizeNearestNeighborOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_inp, ng_inp_sizes;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_inp, ng_inp_sizes));

  opset::Interpolate::InterpolateAttrs interpolate_attrs;
  interpolate_attrs.mode = opset::Interpolate::InterpolateMode::nearest;
  interpolate_attrs.shape_calculation_mode =
      opset::Interpolate::ShapeCalcMode::sizes;
  bool align_corners = false;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "align_corners", &align_corners));
  if (align_corners) {
    interpolate_attrs.coordinate_transformation_mode =
        opset::Interpolate::CoordinateTransformMode::align_corners;
  }
  interpolate_attrs.nearest_mode =
      opset::Interpolate::NearestMode::round_prefer_floor;

  auto input_shape = ng_inp.get_shape();
  std::vector<uint64_t> spatial_shape = {input_shape[1], input_shape[2]};
  auto ng_spatial_shape = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i32, ng::Shape{2}, spatial_shape);
  auto ng_input_shape = ConstructNgNode<opset::Convert>(
      op->name(), ng_spatial_shape, ng::element::f32);
  auto ng_sizes = ConstructNgNode<opset::Convert>(op->name(), ng_inp_sizes,
                                                  ng::element::f32);
  auto ng_scales =
      ConstructNgNode<opset::Divide>(op->name(), ng_sizes, ng_input_shape);
  auto ng_axes = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i32, ng::Shape{2}, std::vector<int>({2, 3}));

  Transpose<0, 3, 1, 2>(ng_inp);
  auto ng_output = ConstructNgNode<opset::Interpolate>(
      op->name(), ng_inp, ng_inp_sizes, ng_scales, ng_axes, interpolate_attrs);
  Transpose<0, 2, 3, 1>(ng_output);
  SaveNgOp(ng_op_map, op->name(), ng_output);
  return Status::OK();
}

static Status TranslateReverseOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_reversed_axis;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_reversed_axis));
  ngraph::op::v1::Reverse::Mode mode = ngraph::op::v1::Reverse::Mode::INDEX;
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<ngraph::op::v1::Reverse>(op->name(), ng_input,
                                                    ng_reversed_axis, mode));
  return Status::OK();
}

static Status TranslateRsqrtOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return TranslateUnaryOp(
      op, static_input_map, ng_op_map, [&op](ng::Output<ng::Node> n) {
        // Create a constant tensor populated with the value -1/2.
        // (1/sqrt(x) = x^(-1/2))
        auto et = n.get_element_type();
        auto shape = n.get_shape();
        std::vector<std::string> constant_values(ng::shape_size(shape), "-0.5");
        auto ng_exponent = ConstructNgNode<opset::Constant>(
            op->name(), et, shape, constant_values);

        // Raise each element of the input to the power -0.5.
        return ConstructNgNode<opset::Power>(op->name(), n, ng_exponent);
      });
}

static Status TranslateScatterNdOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input_indices, ng_updates, ng_shape;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_input_indices, ng_updates, ng_shape));

  std::vector<size_t> shape;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 2, static_input_map, &shape));

  auto ng_input = ConstructNgNode<opset::Constant>(
      op->name(), ng_updates.get_element_type(), ng::Shape(shape), 0);

  auto scatternd_op = ConstructNgNode<opset::ScatterNDUpdate>(
      op->name(), ng_input, ng_input_indices, ng_updates);

  SaveNgOp(ng_op_map, op->name(), scatternd_op);
  return Status::OK();
}

static Status TranslateShapeOp(const Node* op,
                               const std::vector<const Tensor*>&,
                               Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  DataType dtype;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "out_type", &dtype));

  ng::element::Type type;
  TF_RETURN_IF_ERROR(util::TFDataTypeToNGraphElementType(dtype, &type));

  // default output_type = element::i64
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::ShapeOf>(op->name(), ng_input, type));
  return Status::OK();
}

static Status TranslateSizeOp(const Node* op, const std::vector<const Tensor*>&,
                              Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  DataType dtype;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "out_type", &dtype));

  // Size has an attribute to specify output, int32 or int64
  ng::element::Type type;
  TF_RETURN_IF_ERROR(util::TFDataTypeToNGraphElementType(dtype, &type));

  auto ng_input_shape = ng_input.get_shape();
  int64 result = 1;
  for (auto dim : ng_input_shape) {
    result *= dim;
  }

  // make a scalar with value equals to result
  auto ng_result = ConstructNgNode<opset::Constant>(
      op->name(), type, ng::Shape(0), std::vector<int64>({result}));

  SaveNgOp(ng_op_map, op->name(), ng_result);
  return Status::OK();
}

static Status TranslateSliceOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_begin, ng_size;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_begin, ng_size));

  std::vector<int64> begin_vec;
  std::vector<int64> size_vec;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &begin_vec));
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 2, static_input_map, &size_vec));

  if (begin_vec.size() != size_vec.size())
    return errors::InvalidArgument(
        "Cannot translate slice op: size of begin = ", begin_vec.size(),
        ", size of size_vec = ", size_vec.size(), ". Expected them to match.");

  OVTF_VLOG(3) << "Begin input for Slice: " << ng::join(begin_vec);
  OVTF_VLOG(3) << "Size input for Slice: " << ng::join(size_vec);

  std::vector<int64> end_vec(begin_vec.size());
  const auto ng_input_shape = ng_input.get_shape();
  stringstream err_stream;
  string err_msg;
  for (size_t i = 0; i < size_vec.size(); i++) {
    if (size_vec[i] != -1) {
      end_vec[i] = begin_vec[i] + size_vec[i];
    } else {
      // support -1 for size_vec, to the end of the tensor
      end_vec[i] = ng_input_shape[i];
    }

    // check for this condition: 0 <= begin[i] <= begin[i] + size[i] <= Di
    if (0 > begin_vec[i])
      err_stream << "lower < 0: " << begin_vec[i]
                 << ". It should have been positive.\n";
    if (begin_vec[i] > end_vec[i])
      err_stream << "upper < lower: upper = " << end_vec[i]
                 << ", lower = " << begin_vec[i] << "\n";
    if (begin_vec[i] > ng_input_shape[i])
      err_stream << "dim < upper: dim = " << ng_input_shape[i]
                 << ", upper = " << end_vec[i] << "\n";

    err_msg = err_stream.str();
    if (!err_msg.empty())
      return errors::InvalidArgument("Cannot translate slice op at position ",
                                     i, " of ", size_vec.size(),
                                     ". The reasons are:\n", err_msg);
  }

  auto begin = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{begin_vec.size()}, begin_vec);
  auto end = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{end_vec.size()}, end_vec);

  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::StridedSlice>(op->name(), ng_input, begin,
                                                end, std::vector<int64_t>{},
                                                std::vector<int64_t>{}));
  return Status::OK();
}

static Status TranslateSoftmaxOp(const Node* op,
                                 const std::vector<const Tensor*>&,
                                 Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  auto rank = ng_input.get_partial_shape().rank().get_length();
  if (rank < 1) {
    return errors::InvalidArgument("TF Softmax logits must be >=1 dimension");
  }

  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::Softmax>(op->name(), ng_input, rank - 1));
  return Status::OK();
}

// TODO: Change the translation back to unary softplus
// after resolving mish fusion issue
static Status TranslateSoftPlusOp(const Node* op,
                                  const std::vector<const Tensor*>&,
                                  Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_inp;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_inp));
  auto exp = ConstructNgNode<opset::Exp>(op->name(), ng_inp);
  auto add_const = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::f32, ng::Shape{1}, 1);

  auto add = ConstructNgNode<opset::Add>(op->name(), exp, add_const);
  auto ng_output = ConstructNgNode<opset::Log>(op->name(), add);

  SaveNgOp(ng_op_map, op->name(), ng_output);
  return Status::OK();
}

// Translate SpaceToDepthOp
static Status TranslateSpaceToDepthOp(const Node* op,
                                      const std::vector<const Tensor*>&,
                                      Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  // Get the attributes
  int64 block_size;
  std::string tf_data_format;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "block_size", &block_size));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "data_format", &tf_data_format));

  if (tf_data_format != "NHWC" && tf_data_format != "NCHW") {
    return errors::InvalidArgument(
        "DepthToSpace data format is neither NHWC nor NCHW");
  }

  bool is_nhwc = (tf_data_format == "NHWC");

  NHWCtoNCHW(op->name(), is_nhwc, ng_input);
  auto ng_mode = opset::SpaceToDepth::SpaceToDepthMode::BLOCKS_FIRST;
  auto space_to_depth = ConstructNgNode<opset::SpaceToDepth>(
      op->name(), ng_input, ng_mode, block_size);
  NCHWtoNHWC(op->name(), is_nhwc, space_to_depth);
  SaveNgOp(ng_op_map, op->name(), space_to_depth);
  return Status::OK();
}

static Status TranslateSplitOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 1, ng_input));
  // num_split : The number of ways to split. Must evenly divide
  // value.shape[split_dim]
  int32 num_split;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "num_split", &num_split));

  auto rank = ng_input.get_partial_shape().rank().get_length();

  std::vector<int> split_dim_vec;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 0, static_input_map, &split_dim_vec));
  int split_dim = split_dim_vec[0] + (split_dim_vec[0] < 0 ? (int64)rank : 0);
  auto ng_split_dim = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::u64, ng::Shape{}, split_dim);
  auto ng_split = make_shared<opset::Split>(ng_input, ng_split_dim, num_split);

  for (int i = 0; i < num_split; ++i) {
    auto out = ng_split->output(i);
    Builder::SetTracingInfo(op->name(), out);
    SaveNgOp(ng_op_map, op->name(), out);
  }
  return Status::OK();
}

static Status TranslateSplitVOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_split_length, ng_split_dim;

  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  ng::Shape shape = ng_input.get_shape();
  int rank = shape.size();

  std::vector<int64> split_dim_vec;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 2, static_input_map, &split_dim_vec));
  // there should be at least one element specified as axis and not more than
  // one as axis is 0-D
  if (split_dim_vec.size() != 1) {
    return errors::InvalidArgument(
        "split_dim_tensor must have "
        "exactly one element.");
  }
  TF_RETURN_IF_ERROR(CheckAxisDimInRange(split_dim_vec, rank));
  int split_dim = split_dim_vec[0] + (split_dim_vec[0] < 0 ? (int64)rank : 0);
  ng_split_dim = ConstructNgNode<opset::Constant>(op->name(), ng::element::i32,
                                                  ng::Shape{}, split_dim);

  std::vector<int> split_lengths_vec;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 1, static_input_map, &split_lengths_vec));

  // length: Length of size_splits
  int length = 0;
  int idx = -1;

  // Find out the total length of the splits and locate -1 's index, if any
  bool has_one_neg = false;
  for (size_t i = 0; i < split_lengths_vec.size(); ++i) {
    if (split_lengths_vec[i] != -1) {
      length += split_lengths_vec[i];
    } else {
      if (has_one_neg) {
        return errors::InvalidArgument("size_splits can only have one -1");
      } else {
        idx = i;
        has_one_neg = true;
      }
    }
  }

  // Size splits must sum to the dimension of value along split_dim
  if (idx > 0) {
    split_lengths_vec[idx] = shape[split_dim] - length;
  }

  if ((!has_one_neg && length != shape[split_dim]) ||
      (has_one_neg && split_lengths_vec[idx] < 0)) {
    return errors::InvalidArgument(
        "The length of size_splits must sum to the value of the dimension "
        "along split_dim");
  }

  ng_split_length = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i32, ng::Shape{split_lengths_vec.size()},
      split_lengths_vec);

  if (split_lengths_vec.size() != 1) {
    auto ng_split = make_shared<opset::VariadicSplit>(ng_input, ng_split_dim,
                                                      ng_split_length);
    for (size_t i = 0; i < split_lengths_vec.size(); ++i) {
      auto out = ng_split->output(i);
      Builder::SetTracingInfo(op->name(), out);
      SaveNgOp(ng_op_map, op->name(), out);
    }
  } else {
    SaveNgOp(ng_op_map, op->name(), ng_input);
  }

  return Status::OK();
}

static Status TranslateSquareOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  return TranslateUnaryOp(
      op, static_input_map, ng_op_map, [&op](ng::Output<ng::Node> n) {
        return ConstructNgNode<opset::Multiply>(op->name(), n, n);
      });
}

static Status TranslateSqueezeOp(const Node* op,
                                 const std::vector<const Tensor*>&,
                                 Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));
  size_t input_dims = ng_input.get_shape().size();

  std::vector<int32> tf_axis;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "squeeze_dims", &tf_axis));

  // If input dimension is negative, make it positive
  for (size_t i = 0; i < tf_axis.size(); i++) {
    tf_axis[i] = tf_axis[i] < 0 ? (int32)(input_dims) + tf_axis[i] : tf_axis[i];
  }

  if (input_dims > 0 && ng_input.get_shape()[0] == 0) {
    SaveNgOp(ng_op_map, op->name(),
             ConstructNgNode<opset::Constant>(
                 op->name(), ng_input.get_element_type(), ngraph::Shape{0},
                 std::vector<int>({0})));
  } else {
    auto ng_const = ConstructNgNode<opset::Constant>(
        op->name(), ng::element::i32, ng::Shape{tf_axis.size()}, tf_axis);

    SaveNgOp(ng_op_map, op->name(),
             ConstructNgNode<opset::Squeeze>(op->name(), ng_input, ng_const));
  }
  return Status::OK();
}

static Status TranslateStridedSliceOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  int32 begin_mask, end_mask, new_axis_mask, shrink_axis_mask, ellipsis_mask;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "begin_mask", &begin_mask));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "end_mask", &end_mask));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "new_axis_mask", &new_axis_mask));
  TF_RETURN_IF_ERROR(
      GetNodeAttr(op->attrs(), "shrink_axis_mask", &shrink_axis_mask));
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "ellipsis_mask", &ellipsis_mask));

  OVTF_VLOG(5) << "strided slice attributes: "
               << "  begin mask: " << begin_mask << "  end mask: " << end_mask
               << "  new axis mask: " << new_axis_mask
               << "  shrink axis mask: " << shrink_axis_mask
               << "  ellipsis mask: " << ellipsis_mask;

  std::vector<int64> begin_vec;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &begin_vec));
  std::vector<int64> end_vec;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 2, static_input_map, &end_vec));
  std::vector<int64> stride_vec;
  TF_RETURN_IF_ERROR(
      GetStaticInputVector(op, 3, static_input_map, &stride_vec));

  auto begin = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{begin_vec.size()}, begin_vec);
  auto end = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{end_vec.size()}, end_vec);
  auto strides = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{stride_vec.size()}, stride_vec);

  auto mask_to_vec = [](int32 mask) {
    auto length = sizeof(mask) * CHAR_BIT;
    std::vector<int64_t> vec(length, 0);
    if (mask == 0) {
      return vec;
    }
    for (auto i = 0; i < length; ++i) {
      if ((unsigned char)(mask >> i & 0x01) == 1) {
        vec[i] = 1;
      }
    }
    return vec;
  };

  SaveNgOp(
      ng_op_map, op->name(),
      ConstructNgNode<opset::StridedSlice>(
          op->name(), ng_input, begin, end, strides, mask_to_vec(begin_mask),
          mask_to_vec(end_mask), mask_to_vec(new_axis_mask),
          mask_to_vec(shrink_axis_mask), mask_to_vec(ellipsis_mask)));
  return Status::OK();
}

static Status TranslateTileOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_multiples;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_multiples));

  std::vector<int64> multiples;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &multiples));

  auto ng_repeats = ConstructNgNode<opset::Constant>(
      op->name(), ng::element::i64, ng::Shape{multiples.size()}, multiples);
  SaveNgOp(ng_op_map, op->name(),
           ConstructNgNode<opset::Tile>(op->name(), ng_input, ng_repeats));
  return Status::OK();
}

// Translate TopKV2 Op using ngraph core op TopK
static Status TranslateTopKV2Op(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ngraph::Node> ng_input;

  TF_RETURN_IF_ERROR(ValidateInputCount(op, 2));
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));

  // axis along which to compute top k indices
  int64 k_axis = ng_input.get_shape().size() - 1;

  // scalar input tensor specifying how many max/min elts should be computed
  // CPU backend only supports element type i64
  std::vector<int64> ng_k_vec;
  TF_RETURN_IF_ERROR(GetStaticInputVector(op, 1, static_input_map, &ng_k_vec));
  auto ng_k = ConstructNgNode<opset::Constant>(op->name(), ng::element::i64,
                                               ng::Shape{}, ng_k_vec[0]);

  std::string mode = "max";

  std::string sort = "value";
  bool sorted = true;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "sorted", &sorted));
  if (!sorted) {
    sort = "index";
  }

  if (ng_k_vec[0] == 0 || ng_input.get_shape()[0] == 0) {
    SaveNgOp(ng_op_map, op->name(),
             ConstructNgNode<opset::Constant>(
                 op->name(), ng_input.get_element_type(), ngraph::Shape{0},
                 std::vector<int>({0})));

    SaveNgOp(ng_op_map, op->name(),
             ConstructNgNode<opset::Constant>(op->name(), ng::element::i32,
                                              ngraph::Shape{0},
                                              std::vector<int>({0})));
  } else {
    auto ng_result =
        std::make_shared<opset::TopK>(ng_input, ng_k, k_axis, mode, sort);

    ng::Output<ng::Node> ng_values = ng_result->output(0);
    Builder::SetTracingInfo(op->name(), ng_values);
    ng::Output<ng::Node> ng_indices = ng_result->output(1);
    Builder::SetTracingInfo(op->name(), ng_indices);

    SaveNgOp(ng_op_map, op->name(), ng_values);
    SaveNgOp(ng_op_map, op->name(), ng_indices);
  }

  return Status::OK();
}

static Status TranslateTransposeOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input, ng_permutation;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input, ng_permutation));
  SaveNgOp(ng_op_map, op->name(), ConstructNgNode<opset::Transpose>(
                                      op->name(), ng_input, ng_permutation));
  return Status::OK();
}

static Status TranslateUnpackOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  TF_RETURN_IF_ERROR(ValidateInputCount(op, 1));

  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, op, 0, ng_input));
  int32 tf_axis;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "axis", &tf_axis));
  int32 num_outputs;
  TF_RETURN_IF_ERROR(GetNodeAttr(op->attrs(), "num", &num_outputs));

  auto rank = ng_input.get_partial_shape().rank().get_length();
  // convert the negative unpack axis value to positive value
  if (tf_axis < 0) {
    tf_axis = rank + tf_axis;
  }
  for (int i = 0; i < num_outputs; ++i) {
    std::vector<int64_t> begin(rank, 0);
    std::vector<int64_t> end(rank, 0);
    begin[tf_axis] = i;
    end[tf_axis] = i + 1;
    auto ng_begin = ConstructNgNode<opset::Constant>(
        op->name(), ng::element::i64, ng::Shape{begin.size()}, begin);
    auto ng_end = ConstructNgNode<opset::Constant>(op->name(), ng::element::i64,
                                                   ng::Shape{end.size()}, end);
    std::vector<int64_t> begin_mask(rank, 1);
    begin_mask[tf_axis] = 0;
    std::vector<int64_t> end_mask(rank, 1);
    end_mask[tf_axis] = 0;
    std::vector<int64_t> new_axis_mask(rank, 0);
    std::vector<int64_t> shrink_axis_mask(rank, 0);
    auto slice = ConstructNgNode<opset::StridedSlice>(
        op->name(), ng_input, ng_begin, ng_end, begin_mask, end_mask,
        new_axis_mask, shrink_axis_mask);
    auto squeeze_axis = ConstructNgNode<opset::Constant>(
        op->name(), ng::element::i32, ng::Shape{}, tf_axis);
    auto squeeze =
        ConstructNgNode<opset::Squeeze>(op->name(), slice, squeeze_axis);
    SaveNgOp(ng_op_map, op->name(), squeeze);
  }
  return Status::OK();
}

static Status TranslateXdivyOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ngraph::Node> ng_x, ng_y;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_x, ng_y));
  auto zero =
      ConstructNgNode<opset::Constant>(op->name(), ng_x.get_element_type(),
                                       ngraph::Shape{}, std::vector<int>({0}));
  auto x_is_zero = ConstructNgNode<opset::Equal>(op->name(), ng_x, zero);
  auto ng_xdivy = ConstructNgNode<opset::Divide>(op->name(), ng_x, ng_y);
  SaveNgOp(ng_op_map, op->name(), ConstructNgNode<opset::Select>(
                                      op->name(), x_is_zero, ng_x, ng_xdivy));
  return Status::OK();
}

static Status TranslateSelectOp(const Node* op,
                                const std::vector<const Tensor*>&,
                                Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input1, ng_input2, ng_input3;
  TF_RETURN_IF_ERROR(
      GetInputNodes(ng_op_map, op, ng_input1, ng_input2, ng_input3));
  auto ng_select = ConstructNgNode<opset::Select>(op->name(), ng_input1,
                                                  ng_input2, ng_input3);
  SaveNgOp(ng_op_map, op->name(), ng_select);
  return Status::OK();
}

static Status TranslateWhereOp(
    const Node* op, const std::vector<const Tensor*>& static_input_map,
    Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_cond;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_cond));
  auto non_zero = ConstructNgNode<opset::NonZero>(op->name(), ng_cond);
  auto transpose_order = ConstructNgNode<opset::Constant>(
      op->name(), ngraph::element::i64, ngraph::Shape{2},
      std::vector<int64_t>({1, 0}));
  SaveNgOp(ng_op_map, op->name(), ConstructNgNode<opset::Transpose>(
                                      op->name(), non_zero, transpose_order));
  return Status::OK();
}

static Status TranslateZerosLikeOp(const Node* op,
                                   const std::vector<const Tensor*>&,
                                   Builder::OpMap& ng_op_map) {
  ng::Output<ng::Node> ng_input;
  TF_RETURN_IF_ERROR(GetInputNodes(ng_op_map, op, ng_input));

  ng::Shape input_shape = ng_input.get_shape();
  std::vector<std::string> const_values(ng::shape_size(input_shape), "0");
  auto ng_result = ConstructNgNode<opset::Constant>(
      op->name(), ng_input.get_element_type(), input_shape, const_values);
  SaveNgOp(ng_op_map, op->name(), ng_result);
  return Status::OK();
}

const static std::map<
    const string,
    const function<Status(const Node*, const std::vector<const Tensor*>&,
                          Builder::OpMap&)>>
    TRANSLATE_OP_MAP{
        {"Abs", TranslateUnaryOp<opset::Abs>},
        {"Acos", TranslateUnaryOp<opset::Acos>},
        {"Acosh", TranslateUnaryOp<opset::Acosh>},
        {"Add", TranslateBinaryOp<opset::Add>},
        {"AddN", TranslateAddNOp},
        {"AddV2", TranslateBinaryOp<opset::Add>},
        {"Any", TranslateDirectReduceOp<opset::ReduceLogicalOr>},
        {"All", TranslateDirectReduceOp<opset::ReduceLogicalAnd>},
        {"ArgMax", TranslateArgMaxOp},
        {"ArgMin", TranslateArgMinOp},
        {"Asin", TranslateUnaryOp<opset::Asin>},
        {"Asinh", TranslateUnaryOp<opset::Asinh>},
        {"Atan", TranslateUnaryOp<opset::Atan>},
        {"Atanh", TranslateUnaryOp<opset::Atanh>},
        {"AvgPool", TranslateAvgPoolOp<2>},
        {"AvgPool3D", TranslateAvgPoolOp<3>},
        {"BatchToSpaceND", TranslateBatchNDAndSpaceNDOp},
        {"BiasAdd", TranslateBiasAddOp},
        {"Cast", TranslateCastOp},
        {"Ceil", TranslateUnaryOp<opset::Ceiling>},
        {"ConcatV2", TranslateConcatV2Op},
        {"Const", TranslateConstOp},
        {"Conv2D", TranslateConv2DOp},
        {"Conv2DBackpropInput", TranslateConv2DBackpropInputOp},
        {"Conv3D", TranslateConv3DOp},
        {"Conv3DBackpropInputV2", TranslateConv3DBackpropInputV2Op},
        {"Cos", TranslateUnaryOp<opset::Cos>},
        {"Cosh", TranslateUnaryOp<opset::Cosh>},
        {"CropAndResize", TranslateCropAndResizeOp},
        {"Cumsum", TranslateCumsumOp},
        {"DepthToSpace", TranslateDepthToSpaceOp},
        {"DepthwiseConv2dNative", TranslateDepthwiseConv2dNativeOp},
        {"Elu", TranslateEluOp},
        {"Equal", TranslateBinaryOp<opset::Equal>},
        {"Exp", TranslateUnaryOp<opset::Exp>},
        {"ExpandDims", TranslateExpandDimsOp},
        {"FakeQuantWithMinMaxVars", TranslateFakeQuantWithMinMaxVarsOp},
        {"Fill", TranslateFillOp},
        {"Floor", TranslateUnaryOp<opset::Floor>},
        {"FloorDiv", TranslateFloorDivOp},
        {"FloorMod", TranslateBinaryOp<opset::FloorMod>},
        {"FusedBatchNorm", TranslateFusedBatchNormOp},
        {"FusedBatchNormV2", TranslateFusedBatchNormOp},
        {"FusedBatchNormV3", TranslateFusedBatchNormOp},
        {"Gather", TranslateGatherOp},
        {"GatherV2", TranslateGatherV2Op},
        {"GatherNd", TranslateGatherNdOp},
        {"_FusedBatchNormEx", TranslateFusedBatchNormOp},
        {"_FusedConv2D", TranslateFusedConv2DOp},
        {"_FusedDepthwiseConv2dNative", TranslateFusedDepthwiseConv2dNativeOp},
        {"_FusedMatMul", TranslateFusedMatMulOp},
        {"Greater", TranslateBinaryOp<opset::Greater>},
        {"GreaterEqual", TranslateBinaryOp<opset::GreaterEqual>},
        {"Identity", TranslateIdentityOp},
        {"IsFinite", TranslateIsFiniteOp},
        {"L2Loss", TranslateL2LossOp},
        {"LogSoftmax", TranslateLogSoftmaxOp},
        {"LeakyRelu", TranslateLeakyReluOp},
        {"Less", TranslateBinaryOp<opset::Less>},
        {"LessEqual", TranslateBinaryOp<opset::LessEqual>},
        {"Log", TranslateUnaryOp<opset::Log>},
        {"Log1p", TranslateLog1pOp},
        {"LogicalAnd", TranslateBinaryOp<opset::LogicalAnd>},
        {"LogicalNot", TranslateUnaryOp<opset::LogicalNot>},
        {"LogicalOr", TranslateBinaryOp<opset::LogicalOr>},
        {"LRN", TranslateLRNOp},
        {"MatMul", TranslateMatMulOp},
        {"Max", TranslateDirectReduceOp<opset::ReduceMax>},
        {"Maximum", TranslateBinaryOp<opset::Maximum>},
        {"MaxPool", TranslateMaxPoolOp<2>},
        {"MaxPool3D", TranslateMaxPoolOp<3>},
        {"NonMaxSuppressionV2", TranslateNonMaxSuppressionV2Op},
        {"NonMaxSuppressionV3", TranslateNonMaxSuppressionV3Op},
        {"Mean", TranslateDirectReduceOp<opset::ReduceMean>},
        {"Min", TranslateDirectReduceOp<opset::ReduceMin>},
        {"Minimum", TranslateBinaryOp<opset::Minimum>},
        {"MirrorPad", TranslatePadOp},
        {"Mul", TranslateBinaryOp<opset::Multiply>},
        {"Mod", TranslateBinaryOp<opset::Mod>},
        {"Neg", TranslateUnaryOp<opset::Negative>},
        {"NotEqual", TranslateBinaryOp<opset::NotEqual>},
        // Do nothing! NoOps sometimes get placed on nGraph for bureaucratic
        // reasons, but they have no data flow inputs or outputs.
        {"NoOp", [](const Node*, const std::vector<const Tensor*>&,
                    Builder::OpMap&) { return Status::OK(); }},
        {"OneHot", TranslateOneHotOp},
        {"Pack", TranslatePackOp},
        {"Pad", TranslatePadOp},
        {"PadV2", TranslatePadOp},
        {"Pow", TranslateBinaryOp<opset::Power>},
        // PreventGradient is just Identity in dataflow terms, so reuse that.
        {"PreventGradient", TranslateIdentityOp},
        {"Prod", TranslateDirectReduceOp<opset::ReduceProd>},
        {"Range", TranslateRangeOp},
        {"Rank", TranslateRankOp},
        {"RealDiv", TranslateBinaryOp<opset::Divide>},
        {"Reciprocal", TranslateReciprocalOp},
        {"Relu", TranslateUnaryOp<opset::Relu>},
        {"Relu6", TranslateRelu6Op},
        {"Reshape", TranslateReshapeOp},
        {"Round", TranslateRoundOp},
        {"ResizeBilinear", TranslateResizeBilinearOp},
        {"ResizeNearestNeighbor", TranslateResizeNearestNeighborOp},
        {"Reverse", TranslateReverseOp},
        {"ReverseV2", TranslateReverseOp},
        {"Rsqrt", TranslateRsqrtOp},
        {"ScatterNd", TranslateScatterNdOp},
        {"Select", TranslateSelectOp},
        {"SelectV2", TranslateSelectOp},
        {"Shape", TranslateShapeOp},
        {"Sigmoid", TranslateUnaryOp<opset::Sigmoid>},
        {"Sin", TranslateUnaryOp<opset::Sin>},
        {"Sinh", TranslateUnaryOp<opset::Sinh>},
        {"Size", TranslateSizeOp},
        {"Sign", TranslateUnaryOp<opset::Sign>},
        {"Slice", TranslateSliceOp},
        {"Snapshot", TranslateIdentityOp},
        {"Softmax", TranslateSoftmaxOp},
        {"Softplus", TranslateSoftPlusOp},
        {"SpaceToBatchND", TranslateBatchNDAndSpaceNDOp},
        {"SpaceToDepth", TranslateSpaceToDepthOp},
        {"Split", TranslateSplitOp},
        {"SplitV", TranslateSplitVOp},
        {"Sqrt", TranslateUnaryOp<opset::Sqrt>},
        {"Square", TranslateSquareOp},
        {"SquaredDifference", TranslateBinaryOp<opset::SquaredDifference>},
        {"Squeeze", TranslateSqueezeOp},
        {"StridedSlice", TranslateStridedSliceOp},
        {"Sub", TranslateBinaryOp<opset::Subtract>},
        {"Sum", TranslateDirectReduceOp<opset::ReduceSum>},
        {"Tan", TranslateUnaryOp<opset::Tan>},
        {"Tanh", TranslateUnaryOp<opset::Tanh>},
        {"Tile", TranslateTileOp},
        {"TopKV2", TranslateTopKV2Op},
        {"Transpose", TranslateTransposeOp},
        {"Unpack", TranslateUnpackOp},
        {"Where", TranslateWhereOp},
        {"Xdivy", TranslateXdivyOp},
        {"ZerosLike", TranslateZerosLikeOp}};

Status Builder::TranslateGraph(
    const std::vector<TensorShape>& inputs,
    const std::vector<const Tensor*>& static_input_map,
    const Graph* input_graph, const string name,
    shared_ptr<ng::Function>& ng_function) {
  ng::ResultVector ng_result_list;
  std::vector<Tensor> tf_input_tensors;
  TranslateGraph(inputs, static_input_map, input_graph, name, ng_function,
                 ng_result_list, tf_input_tensors);
  return Status::OK();
}

Status Builder::TranslateGraph(
    const std::vector<TensorShape>& inputs,
    const std::vector<const Tensor*>& static_input_map,
    const Graph* input_graph, const string name,
    shared_ptr<ng::Function>& ng_function, ng::ResultVector& ng_result_list,
    const std::vector<Tensor>& tf_input_tensors) {
  //
  // We will visit ops in topological order.
  //
  // ought to be `const Node*`, but GetReversePostOrder doesn't use `const`

  vector<Node*> ordered;
  GetReversePostOrder(*input_graph, &ordered, NodeComparatorName());

  //
  // Split ops into params, retvals, and all others.
  //
  vector<const Node*> tf_params;
  vector<const Node*> tf_ret_vals;
  vector<const Node*> tf_ops;

  for (const auto n : ordered) {
    if (n->IsSink() || n->IsSource()) {
      continue;
    }

    if (n->IsControlFlow()) {
      return errors::Unimplemented(
          "Encountered a control flow op in the openvino_tensorflow: ",
          n->DebugString());
    }

    if (n->IsArg()) {
      tf_params.push_back(n);
    } else if (n->IsRetval()) {
      tf_ret_vals.push_back(n);
    } else {
      tf_ops.push_back(n);
    }
  }

  //
  // The op map holds a mapping from TensorFlow op names (strings) to
  // vector of generated nGraph Output<Node>.
  //
  Builder::OpMap ng_op_map;

  //
  // Populate the parameter list, and also put parameters into the op map.
  //
  ng::ParameterVector ng_parameter_list(tf_params.size());
  ng::ParameterVector ng_func_parameter_list;
  ng_func_parameter_list.reserve(tf_params.size());

  for (auto parm : tf_params) {
    DataType dtype;
    if (GetNodeAttr(parm->attrs(), "T", &dtype) != Status::OK()) {
      return errors::InvalidArgument("No data type defined for _Arg");
    }
    int index;
    if (GetNodeAttr(parm->attrs(), "index", &index) != Status::OK()) {
      return errors::InvalidArgument("No index defined for _Arg");
    }

    ng::element::Type ng_et;
    TF_RETURN_IF_ERROR(util::TFDataTypeToNGraphElementType(dtype, &ng_et));

    ng::Shape ng_shape;
    TF_RETURN_IF_ERROR(
        util::TFTensorShapeToNGraphShape(inputs[index], &ng_shape));

    string prov_tag;
    GetNodeAttr(parm->attrs(), "_prov_tag", &prov_tag);
    auto ng_param =
        ConstructNgNode<opset::Parameter>(prov_tag, ng_et, ng_shape);

    auto ng_shape_check = [ng_shape]() {
      if (ng_shape.size() > 0) {
        for (auto val : ng_shape) {
          if (val == 0) return true;
        }
      }
      return false;
    };

    bool is_variable = false;
    if (util::GetEnv("OPENVINO_TF_CONVERT_VARIABLES_TO_CONSTANTS") != "0" &&
        !tf_input_tensors.empty()) {
      try {
        GetNodeAttr(parm->attrs(), "_is_variable", &is_variable);
      } catch (const std::exception&) {
        OVTF_VLOG(1) << "Parameter " << parm->name() << " is not a variable";
      }
    }

    if (ng_shape_check()) {
      std::vector<std::string> constant_values(ng::shape_size(ng_shape), "0");
      auto ng_const_input = ConstructNgNode<opset::Constant>(
          prov_tag, ng_et, ng_shape, constant_values);
      SaveNgOp(ng_op_map, parm->name(), ng_const_input);
    } else {
      if (is_variable) {
        ng::Output<ng::Node> ng_const_input;
        const Tensor input_tensor = tf_input_tensors[index];
        OVTF_VLOG(1) << "Converting " << parm->name() << " to constant";
        switch (dtype) {
          case DT_FLOAT:
            MakeConstOpForParam<float>(input_tensor, prov_tag, ng_et, ng_shape,
                                       ng_const_input);
            break;
          case DT_DOUBLE:
            MakeConstOpForParam<double>(input_tensor, prov_tag, ng_et, ng_shape,
                                        ng_const_input);
            break;
          case DT_INT8:
            MakeConstOpForParam<int8>(input_tensor, prov_tag, ng_et, ng_shape,
                                      ng_const_input);
            break;
          case DT_INT16:
            MakeConstOpForParam<int16>(input_tensor, prov_tag, ng_et, ng_shape,
                                       ng_const_input);
            break;
          case DT_INT32:
            MakeConstOpForParam<int32>(input_tensor, prov_tag, ng_et, ng_shape,
                                       ng_const_input);
            break;
          case DT_INT64:
            MakeConstOpForParam<int64>(input_tensor, prov_tag, ng_et, ng_shape,
                                       ng_const_input);
            break;
          case DT_UINT8:
            MakeConstOpForParam<uint8>(input_tensor, prov_tag, ng_et, ng_shape,
                                       ng_const_input);
            break;
          case DT_UINT16:
            MakeConstOpForParam<uint16>(input_tensor, prov_tag, ng_et, ng_shape,
                                        ng_const_input);
            break;
          case DT_UINT32:
            MakeConstOpForParam<uint32>(input_tensor, prov_tag, ng_et, ng_shape,
                                        ng_const_input);
            break;
          case DT_UINT64:
            MakeConstOpForParam<uint64>(input_tensor, prov_tag, ng_et, ng_shape,
                                        ng_const_input);
            break;
          case DT_BOOL:
            MakeConstOpForParam<bool>(input_tensor, prov_tag, ng_et, ng_shape,
                                      ng_const_input);
            break;
          default:
            return errors::Internal("Tensor has element type ",
                                    DataType_Name(dtype),
                                    "; don't know how to convert");
        }

        SaveNgOp(ng_op_map, parm->name(), ng_const_input);
      } else
        SaveNgOp(ng_op_map, parm->name(), ng_param);
    }
    ng_parameter_list[index] =
        ngraph::as_type_ptr<opset::Parameter>(ng_param.get_node_shared_ptr());
  }

  //
  // Now create the nGraph ops from TensorFlow ops.
  //
  for (auto op : tf_ops) {
    OVTF_VLOG(2) << "Constructing op " << op->name() << " which is "
                 << op->type_string();

    const function<Status(const Node*, const std::vector<const Tensor*>&,
                          Builder::OpMap&)>* op_fun;

    try {
      op_fun = &(TRANSLATE_OP_MAP.at(op->type_string()));
    } catch (const std::out_of_range&) {
      // -----------------------------
      // Catch-all for unsupported ops
      // -----------------------------
      OVTF_VLOG(3) << "No translation handler registered for op: " << op->name()
                   << " (" << op->type_string() << ")";
      OVTF_VLOG(3) << op->def().DebugString();
      return errors::InvalidArgument(
          "No translation handler registered for op: ", op->name(), " (",
          op->type_string(), ")\n", op->def().DebugString());
    }

    try {
      TF_RETURN_IF_ERROR((*op_fun)(op, static_input_map, ng_op_map));
    } catch (const std::exception& e) {
      return errors::Internal("Unhandled exception in op handler: ", op->name(),
                              " (", op->type_string(), ")\n",
                              op->def().DebugString(), "\n", "what(): ",
                              e.what());
    }
  }

  //
  // Populate the result list.
  //
  ng_result_list.resize(tf_ret_vals.size());
  ng::ResultVector ng_func_result_list;
  ng_func_result_list.reserve(tf_params.size());

  for (auto n : tf_ret_vals) {
    // Make sure that this _Retval only has one input node.
    if (n->num_inputs() != 1) {
      return errors::InvalidArgument("_Retval has ", n->num_inputs(),
                                     " inputs, should have 1");
    }

    int index;
    if (GetNodeAttr(n->attrs(), "index", &index) != Status::OK()) {
      return errors::InvalidArgument("No index defined for _Retval");
    }

    ng::Output<ng::Node> result;
    TF_RETURN_IF_ERROR(GetInputNode(ng_op_map, n, 0, result));
    auto ng_result = ConstructNgNode<opset::Result>(n->name(), result);
    ng_result_list[index] =
        ngraph::as_type_ptr<opset::Result>(ng_result.get_node_shared_ptr());
  }

  auto param_dim_check = [ng_parameter_list](int i) {
    auto param_shape_list = ng_parameter_list[i]->get_shape();
    for (auto dim : param_shape_list) {
      if (dim == 0) return true;
    }
    return false;
  };

  for (int i = 0; i < ng_parameter_list.size(); i++) {
    if (!(ng_parameter_list[i]->get_shape().size() > 0 && param_dim_check(i))) {
      ng_func_parameter_list.push_back(ng_parameter_list[i]);
    }
  }

  auto result_dim_check = [ng_result_list](int i) {
    auto res_shape_list = ng_result_list[i]->get_shape();
    for (auto dim : res_shape_list) {
      if (dim == 0) return true;
    }
    return false;
  };

  for (int i = 0; i < ng_result_list.size(); i++) {
    if (ng_result_list[i]->is_dynamic() ||
        !(ng_result_list[i]->get_shape().size() > 0 && result_dim_check(i))) {
      ng_func_result_list.push_back(ng_result_list[i]);
    }
  }

  //
  // Create the nGraph function.
  //
  try {
    ng_function = make_shared<ng::Function>(ng_func_result_list,
                                            ng_func_parameter_list, name);
  } catch (const std::exception& exp) {
    return errors::Internal("Failed to create nGraph Function for " + name +
                            ": " + string(exp.what()));
  }

  //
  // Apply additional passes on the nGraph function here.
  //
  {
    ngraph::pass::Manager passes;
    if (util::GetEnv("OPENVINO_TF_CONSTANT_FOLDING") == "1") {
      passes.register_pass<ngraph::pass::ConstantFolding>();
    }
    if (util::GetEnv("OPENVINO_TF_TRANSPOSE_SINKING") != "0") {
      passes.register_pass<pass::TransposeSinking>();
    }
    passes.run_passes(ng_function);
  }
  OVTF_VLOG(5) << "Done with passes";
  //
  // Request row-major layout on results.
  //
  for (auto result : ng_function->get_results()) {
    result->set_needs_default_layout(true);
  }
  OVTF_VLOG(5) << "Done with translations";
  return Status::OK();
}

}  // namespace openvino_tensorflow
}  // namespace tensorflow
