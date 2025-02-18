/*******************************************************************************
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

#ifndef OPENVINO_TF_BRIDGE_DEFAULT_OPSET_H_
#define OPENVINO_TF_BRIDGE_DEFAULT_OPSET_H_
#pragma once

#include "ngraph/opsets/opset7.hpp"

namespace tensorflow {
namespace openvino_tensorflow {

namespace opset = ngraph::opset7;
namespace default_opset = ngraph::opset7;

}  // namespace openvino_tensorflow
}  // namespace tensorflow

#endif  // OPENVINO_TF_BRIDGE_DEFAULT_OPSET_H_