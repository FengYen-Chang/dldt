// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "ie_layers.h"
#include "ie_layer_validators.hpp"
#include "debug.h"
#include "xml_parse_utils.h"
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <cmath>
#include <limits>
#include <ie_iextension.h>
#include <ie_format_parser.h>

#include <details/ie_exception.hpp>

namespace InferenceEngine {

using namespace details;
using std::vector;
using std::string;
using std::map;

template <typename T, typename P>
inline bool one_of(T val, P item) { return val == item; }
template <typename T, typename P, typename... Args>
inline bool one_of(T val, P item, Args... item_others) {
    return val == item || one_of(val, item_others...);
}

void CNNLayer::validateLayer() {
    try {
        LayerValidator::Ptr validator = LayerValidators::getInstance()->getValidator(type);
        validator->parseParams(this);
        validator->checkParams(this);
        InOutDims shapes;
        getInOutShapes(this, shapes);
        validator->checkShapes(this, shapes.inDims);
    } catch(InferenceEngineException ie_e) {
        THROW_IE_EXCEPTION << "Error of validate layer: " << this->name
                           << " with type: " << this->type << ". "
                           << ie_e.what();
    }
}

struct WeightableParams {
    std::vector<size_t>  _kernel;
    size_t _outputs = 0lu;
    size_t _groups = 1lu;
    bool _isKernelFromInput = false;

    WeightableParams(size_t outputs, bool isKernelFromInput, size_t groups = 0, const std::vector<size_t>& kernel = {}) :
                _kernel(kernel),
                _outputs(outputs),
                _groups(groups),
                _isKernelFromInput(isKernelFromInput) {}
};

void checkWeightable(const std::map<std::string, Blob::Ptr>& blobs,
                     const vector<SizeVector>& inShapes,
                     WeightableParams params,
                     const SizeVector& numDims) {
    if (inShapes.size() != 1)
        THROW_IE_EXCEPTION << "Number of inputs (" << inShapes.size() << ") is not equal to expected ones (1)";
    SizeVector firstInputShape = inShapes[0];
    size_t inputSize = firstInputShape.size();

    bool isOK = false;
    for (auto dim : numDims) {
        if (inputSize == dim) {
            isOK = true;
            break;
        }
    }
    if (!isOK) {
        THROW_IE_EXCEPTION << "Input shape " << details::dumpVec(firstInputShape)
                           << " has unexpected size, supported sizes: " << details::dumpVec(numDims);
    }

    if (firstInputShape.empty()) THROW_IE_EXCEPTION << "Input shape can't be empty";

    size_t IC, OC;
    std::vector<size_t> kernel;
    IC = firstInputShape[1];
    if (params._isKernelFromInput) {
        for (int i = 1; i <= inputSize - 2; i++)
            kernel.push_back(firstInputShape[inputSize - i]);
    } else {
        for (auto k : params._kernel) {
            kernel.push_back(k);
        }
    }
    OC = params._outputs;

    auto it = blobs.find("weights");
    if (it != blobs.end()) {  // TODO: return with fixing shape infer tests: THROW_IE_EXCEPTION << "Invalid blobs: no weights";
        auto weights = it->second;
        if (weights == nullptr || weights->dims().empty()) THROW_IE_EXCEPTION << "Weights can't be empty";

        auto weightsSize = details::product(weights->dims());
        size_t expectedWeightsSize = OC * IC;
        for (auto k : kernel) {
            expectedWeightsSize *= k;
        }
        if (params._groups) expectedWeightsSize /= params._groups;
        if (expectedWeightsSize != weightsSize) {
            std::string ker_str;
            for (int i = 0; i < params._kernel.size(); i++) {
                if (!ker_str.empty())
                    ker_str += "x";
                ker_str += std::to_string(kernel[i]);
            }
            THROW_IE_EXCEPTION << "New shapes " << details::dumpVec(firstInputShape) << " make Kernels(" << ker_str <<
                               "), Channels(" << IC << "), Output depth(" << OC << "), Groups("
                               << params._groups << ") not matching weights size: "
                               << expectedWeightsSize << " vs " << weightsSize;
        }
    }

    it = blobs.find("biases");
    if (it != blobs.end()) {
        auto biases = it->second;
        if (biases == nullptr || biases->dims().empty()) THROW_IE_EXCEPTION << "Biases can't be empty";
        auto biasesSize = details::product(biases->dims());
        if (OC != biasesSize) {
            THROW_IE_EXCEPTION << "Number of outputs (" << OC << ") don't match biases size: " << biasesSize;
        }
    }
}

void checkDims(const std::vector<SizeVector>& shapes, const vector<int>& expected_shape_size) {
    for (auto i : shapes) {
        if (i.empty()) {
            THROW_IE_EXCEPTION << " Failed with invalid shapes: dimension is empty";
        }
        auto iter = std::find(expected_shape_size.begin(), expected_shape_size.end(), i.size());
        if (iter == expected_shape_size.end()) {
            THROW_IE_EXCEPTION << " Failed with invalid shapes: dimension is invalid";
        }
    }
}

void checkNumOfInput(const std::vector<SizeVector>& inShapes, const vector<int>& expected_num_of_shapes) {
    bool shape_was_found = false;
    for (const auto& i : expected_num_of_shapes) {
        if (inShapes.size() == i) {
            shape_was_found = true;
        }
    }
    if (!shape_was_found) {
        THROW_IE_EXCEPTION << "Number of inputs (" << inShapes.size() << ") is not equal to expected ones";
    }
}

LayerValidators* LayerValidators::getInstance() {
    if (!_instance) {
        _instance = new LayerValidators();
    }
    return _instance;
}

LayerValidator::Ptr LayerValidators::getValidator(const std::string& type) {
    if (_validators.find(type) == _validators.end()) {
        return std::make_shared<GeneralValidator>(type);
    }
    return _validators[type];
}

void LayerValidators::addImpl(const std::string& type, const LayerValidator::Ptr& validator) {
    _validators[type] = validator;
}

LayerValidators* LayerValidators::_instance = nullptr;

GeneralValidator::GeneralValidator(const std::string& _type) : LayerValidator(_type) {}

void FullyConnectedValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<FullyConnectedLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of FullyConnectedLayer class";
    }
    casted->_out_num = casted->GetParamAsUInt("out-size");
}

void FullyConnectedValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const FullyConnectedLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of FullyConnectedLayer class";
    }
    unsigned int _out_num = casted->GetParamAsUInt("out-size");
}

void FullyConnectedValidator::checkCorrespondence(const CNNLayer* layer,
                                                  const std::map<std::string, Blob::Ptr>& blobs,
                                                  const vector<SizeVector>& inShapes) const {
    const auto casted = dynamic_cast<const FullyConnectedLayer*>(layer);
    if (!casted) THROW_IE_EXCEPTION << "Layer is not instance of FullyConnected layer class";
    checkWeightable(blobs, inShapes, {casted->_out_num, true, 1}, {2, 4, 5});
}

FullyConnectedValidator::FullyConnectedValidator(const std::string& _type) : LayerValidator(_type) {}

void FullyConnectedValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void CropValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<CropLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of CropLayer class";
    }
    if (casted->axis.empty()) {
        auto getArray = [](std::string param, vector<int>& array) {
            std::istringstream stream(param);
            std::string str;
            while (getline(stream, str, ',')) {
                int val = std::stoi(str);
                array.push_back(val);
            }
        };
        getArray(layer->GetParamAsString("axis"), casted->axis);
        if (casted->params.find("offset") != casted->params.end()) {
            getArray(layer->GetParamAsString("offset"), casted->offset);
        }
        if (casted->params.find("dim") != casted->params.end()) {
            getArray(layer->GetParamAsString("dim"), casted->dim);
        }
        if (casted->params.find("crop_begin") != casted->params.end()) {
            getArray(layer->GetParamAsString("crop_begin"), casted->offset);
        }
    }
}

void CropValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const CropLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of CropLayer class";
    }
    if (casted->axis.size() != casted->offset.size()) {
        THROW_IE_EXCEPTION << "Incorrect format of the Crop layer: number of axis doesn't match number of offset - ("
                           << casted->axis.size() << " vs. " << casted->offset.size() << ")";
    }
}

CropValidator::CropValidator(const std::string& _type) : LayerValidator(_type) {}

void CropValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const CropLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of CropLayer class";
    }
    size_t numInputs = inShapes.size();
    checkNumOfInput(inShapes, {1, 2});

    auto firstShape = inShapes[0];
    size_t shapeSize = firstShape.size();
    for (size_t i = 0; i < casted->axis.size(); i++) {
        int axis = casted->axis[i];
        int offset = casted->offset[i];
        if (shapeSize <= axis)
            THROW_IE_EXCEPTION << "Crop axis(" << casted->axis[i]
                               << ") should be less the number of dimensions of first input ("
                               << firstShape.size() << ")";
        if (numInputs == 2) {
            if (casted->params.find("crop_begin") != casted->params.end()) {
                THROW_IE_EXCEPTION
                        << "Incorrect format of the Crop layer: `crop_begin` and `crop_end` attributes are valid for single input only";
            }
            auto secondShape = inShapes[1];
            if (secondShape.size() <= axis)
                THROW_IE_EXCEPTION << "Crop axis(" << axis
                                   << ") should be less the number of dimensions of second input ("
                                   << secondShape.size() << ")";
            size_t newSize = secondShape[axis];
            if (firstShape[axis] < static_cast<size_t>(offset + newSize)) {
                THROW_IE_EXCEPTION << "Incorrect crop data! Offset(" << offset << ") + result size of output("
                                   << newSize << ") should be less then input size(" << firstShape[axis]
                                   << ") for axis(" << axis << ")";
            }
        } else if (!casted->dim.empty()) {
            int dim = casted->dim[i];
            if (firstShape[axis] < static_cast<size_t>(offset + dim)) {
                THROW_IE_EXCEPTION << "Incorrect crop data! Offset(" << offset << ") + result size of output("
                                   << dim << ") should be less then input size(" << firstShape[axis]
                                   << ") for axis(" << axis << ")";
            }
        }
    }
}

ConvolutionValidator::ConvolutionValidator(const std::string& _type) : LayerValidator(_type) {}

void ConvolutionValidator::parseParams(CNNLayer* layer) {
    auto convLayer = dynamic_cast<ConvolutionLayer*>(layer);
    if (!convLayer) {
        THROW_IE_EXCEPTION << "Layer is not instance of ConvolutionLayer class";
    }
    convLayer->_out_depth = convLayer->GetParamAsUInt("output");

    convLayer->_kernel.clear();
    convLayer->_stride.clear();
    convLayer->_padding.clear();
    convLayer->_pads_end.clear();
    convLayer->_dilation.clear();

    vector<unsigned int> kernels = convLayer->GetParamAsUInts("kernel", {});
    if (kernels.empty()) {
        // IR_v == 2
        convLayer->_kernel.insert(X_AXIS, convLayer->GetParamAsUInt("kernel-x"));
        convLayer->_kernel.insert(Y_AXIS, convLayer->GetParamAsUInt("kernel-y"));

        convLayer->_stride.insert(X_AXIS, convLayer->GetParamAsUInt("stride-x", 1u));
        convLayer->_stride.insert(Y_AXIS, convLayer->GetParamAsUInt("stride-y", 1u));
        // TODO: maybe just throw exception, why do we change IR?
        if (0 == convLayer->_stride[X_AXIS]) {
            convLayer->_stride[X_AXIS] = 1u;
            LogError("Warning! in layer %s: Stride x is 0, setting to 1 ", convLayer->name.c_str());
        }
        if (0 == convLayer->_stride[Y_AXIS]) {
            convLayer->_stride[Y_AXIS] = 1u;
            LogError("Warning! in layer %s: Stride y is 0, setting to 1", convLayer->name.c_str());
        }

        convLayer->_padding.insert(X_AXIS, convLayer->GetParamAsUInt("pad-x", 0u));
        convLayer->_padding.insert(Y_AXIS, convLayer->GetParamAsUInt("pad-y", 0u));

        convLayer->_pads_end.insert(X_AXIS, convLayer->GetParamAsUInt("pad-r", convLayer->_padding[X_AXIS]));
        convLayer->_pads_end.insert(Y_AXIS, convLayer->GetParamAsUInt("pad-b", convLayer->_padding[Y_AXIS]));

        convLayer->_dilation.insert(X_AXIS, convLayer->GetParamAsUInt("dilation-x", 1u));
        convLayer->_dilation.insert(Y_AXIS, convLayer->GetParamAsUInt("dilation-y", 1u));
    } else {
        // IR_v > 2
        for (int i = 1; i <= kernels.size(); i++) {
            convLayer->_kernel.insert(i - 1, kernels[kernels.size() - i]);
        }

        vector<unsigned int> default_0 = vector<unsigned int> (convLayer->_kernel.size(), 0u);
        vector<unsigned int> default_1 = vector<unsigned int> (convLayer->_kernel.size(), 1u);

        vector<unsigned int> strides = convLayer->GetParamAsUInts("strides", default_1);
        for (int i = 1; i <= strides.size(); i++) {
            if (strides[strides.size() - i] == 0) {
                THROW_IE_EXCEPTION << "Stride could not be 0.\nIn layer " << convLayer->name;
            }
            convLayer->_stride.insert(i - 1, strides[strides.size() - i]);
        }

        vector<unsigned int> pads_begin = convLayer->GetParamAsUInts("pads_begin", default_0);
        for (int i = 1; i <= pads_begin.size(); i++) {
            convLayer->_padding.insert(i - 1, pads_begin[pads_begin.size() - i]);
        }

        vector<unsigned int> pads_end = convLayer->GetParamAsUInts("pads_end", pads_begin);
        for (int i = 1; i <= pads_end.size(); i++) {
            convLayer->_pads_end.insert(i - 1, pads_end[pads_end.size() - i]);
        }

        vector<unsigned int> dilations = convLayer->GetParamAsUInts("dilations", default_1);
        for (int i = 1; i <= dilations.size(); i++) {
            convLayer->_dilation.insert(i - 1, dilations[dilations.size() - i]);
        }
    }

    convLayer->_auto_pad = convLayer->GetParamAsString("auto_pad", "");
    convLayer->_group = convLayer->GetParamAsUInt("group", 1u);
}

void ConvolutionValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const ConvolutionLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ConvolutionLayer class";
    }
    casted->GetParamAsUInt("output");

    vector<unsigned int> kernels = casted->GetParamAsUInts("kernel", {});
    if (kernels.empty()) {
        // IR_v == 2
        casted->GetParamAsUInt("kernel-x");
        casted->GetParamAsUInt("kernel-y");
        casted->GetParamAsUInt("stride-x", 1u);
        casted->GetParamAsUInt("stride-y", 1u);
        casted->GetParamAsUInt("pad-x", 0u);
        casted->GetParamAsUInt("pad-y", 0u);
        casted->GetParamAsUInt("pad-r", casted->_padding[X_AXIS]);
        casted->GetParamAsUInt("pad-b", casted->_padding[Y_AXIS]);
        casted->GetParamAsUInt("dilation-x", 1u);
        casted->GetParamAsUInt("dilation-y", 1u);
    } else {
        // IR_v > 2
        vector<unsigned int> default_0 = vector<unsigned int> (casted->_kernel.size(), 0u);
        vector<unsigned int> default_1 = vector<unsigned int> (casted->_kernel.size(), 1u);
        casted->GetParamAsUInts("strides", default_1);
        casted->GetParamAsUInts("pads_begin", default_0);
        casted->GetParamAsUInts("pads_end", default_0);
        casted->GetParamAsUInts("dilations", default_1);
    }
    casted->GetParamAsString("auto_pad", "");
    casted->GetParamAsUInt("group", 1);
}

void ConvolutionValidator::checkCorrespondence(const CNNLayer* layer,
                                               const std::map<std::string, Blob::Ptr>& blobs,
                                               const vector<SizeVector>& inShapes) const {
    auto convLayer = dynamic_cast<const ConvolutionLayer*>(layer);
    if (!convLayer)
        THROW_IE_EXCEPTION << "Layer is not instance of Convolution layer class";

    std::vector<size_t> krn;
    for (int i = 0; i < convLayer->_kernel.size(); i++)
        krn.push_back(convLayer->_kernel[i]);
    checkWeightable(blobs, inShapes, {convLayer->_out_depth, false, convLayer->_group, krn},
                    {4, 5});
}

void ConvolutionValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void DeconvolutionValidator::parseParams(CNNLayer* layer) {
    auto deconvLayer = dynamic_cast<DeconvolutionLayer*>(layer);
    if (!deconvLayer) {
        THROW_IE_EXCEPTION << "Layer is not instance of DeconvolutionLayer class";
    }
    ConvolutionValidator::parseParams(layer);
}

void DeconvolutionValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const ConvolutionLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ConvolutionLayer class";
    }
    casted->GetParamAsUInt("output");

    vector<unsigned int> kernels = casted->GetParamAsUInts("kernel", {});
    if (kernels.empty()) {
        // IR_v == 2
        casted->GetParamAsUInt("kernel-x");
        casted->GetParamAsUInt("kernel-y");
        casted->GetParamAsUInt("stride-x", 1u);
        casted->GetParamAsUInt("stride-y", 1u);
        casted->GetParamAsUInt("pad-x", 0u);
        casted->GetParamAsUInt("pad-y", 0u);
        casted->GetParamAsUInt("pad-r", casted->_padding[X_AXIS]);
        casted->GetParamAsUInt("pad-b", casted->_padding[Y_AXIS]);
        casted->GetParamAsUInt("dilation-x", 1u);
        casted->GetParamAsUInt("dilation-y", 1u);
    } else {
        // IR_v > 2
        vector<unsigned int> default_0 = vector<unsigned int> (casted->_kernel.size(), 0u);
        vector<unsigned int> default_1 = vector<unsigned int> (casted->_kernel.size(), 1u);
        casted->GetParamAsUInts("strides", default_1);
        casted->GetParamAsUInts("pads_begin", default_0);
        casted->GetParamAsUInts("pads_end", default_0);
        casted->GetParamAsUInts("dilations", default_1);
    }
    casted->GetParamAsString("auto_pad", "");
    casted->GetParamAsUInt("group", 1);
}

DeconvolutionValidator::DeconvolutionValidator(const std::string& _type) : ConvolutionValidator(_type) {}

void DeconvolutionValidator::checkCorrespondence(const CNNLayer* layer,
                                                 const std::map<std::string, Blob::Ptr>& blobs,
                                                 const vector<SizeVector>& inShapes) const {
    auto deconv_layer = dynamic_cast<const DeconvolutionLayer*>(layer);
    if (!deconv_layer)
        THROW_IE_EXCEPTION << "Layer is not instance of Deconvolution layer class";

    std::vector<size_t> krn;
    for (int i = 0; i < deconv_layer->_kernel.size(); i++)
        krn.push_back(deconv_layer->_kernel[i]);
    checkWeightable(blobs, inShapes, {deconv_layer->_out_depth, false, deconv_layer->_group, krn},
                    {4, 5});
}

void DeconvolutionValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

PoolingValidator::PoolingValidator(const std::string& _type) : LayerValidator(_type) {}

void PoolingValidator::parseParams(CNNLayer* layer) {
    auto poolLayer = dynamic_cast<PoolingLayer*>(layer);
    if (!poolLayer) {
        THROW_IE_EXCEPTION << "Layer is not instance of PoolingLayer class";
    }

    poolLayer->_kernel.clear();
    poolLayer->_stride.clear();
    poolLayer->_padding.clear();
    poolLayer->_pads_end.clear();

    poolLayer->_auto_pad = poolLayer->GetParamAsString("auto_pad", "");

    vector<unsigned int> kernels = poolLayer->GetParamAsUInts("kernel", {});
    if (kernels.empty()) {
        int kernel_x = poolLayer->GetParamAsInt("kernel-x", -1);
        /** Pooling as custom layer */
        if (kernel_x == -1) {
            try {
                unsigned int kernel_size = poolLayer->GetParamAsUInt("kernel_size");
                unsigned int kernel_w = poolLayer->GetParamAsUInt("kernel_w", 0u);
                unsigned int kernel_h = poolLayer->GetParamAsUInt("kernel_h", 0u);
                poolLayer->_kernel.insert(X_AXIS, kernel_w == 0u ? kernel_size : kernel_w);
                poolLayer->_kernel.insert(Y_AXIS, kernel_h == 0u ? kernel_size : kernel_h);

                unsigned int stride = poolLayer->GetParamAsUInt("stride", 1u);
                unsigned int stride_w = poolLayer->GetParamAsUInt("stride_w", 0u);
                unsigned int stride_h = poolLayer->GetParamAsUInt("stride_h", 0u);
                poolLayer->_stride.insert(X_AXIS, stride_w == 0u ? stride : stride_w);
                poolLayer->_stride.insert(Y_AXIS, stride_h == 0u ? stride : stride_h);

                unsigned int pad = poolLayer->GetParamAsUInt("pad", 0u);
                unsigned int pad_w = poolLayer->GetParamAsUInt("pad_w", 0u);
                unsigned int pad_h = poolLayer->GetParamAsUInt("pad_h", 0u);

                poolLayer->_padding.insert(X_AXIS, pad_w == 0u ? pad : pad_w);
                poolLayer->_padding.insert(Y_AXIS, pad_h == 0u ? pad : pad_h);

                poolLayer->_pads_end.insert(X_AXIS, 0u);
                poolLayer->_pads_end.insert(Y_AXIS, 0u);
            } catch (...) {
            }

            std::string alg = poolLayer->GetParamAsString("pool", "caffe.PoolingParameter.MAX");
            poolLayer->_type = alg == "caffe.PoolingParameter.MAX" ? PoolingLayer::MAX : PoolingLayer::AVG;
        } else  /** Default behavior */ {
            poolLayer->_kernel.insert(X_AXIS, poolLayer->GetParamAsUInt("kernel-x"));
            poolLayer->_kernel.insert(Y_AXIS, poolLayer->GetParamAsUInt("kernel-y"));

            poolLayer->_stride.insert(X_AXIS, poolLayer->GetParamAsUInt("stride-x", 1u));
            poolLayer->_stride.insert(Y_AXIS, poolLayer->GetParamAsUInt("stride-y", 1u));
            // TODO: maybe just throw exception, why do we change IR?
            if (0 == poolLayer->_stride[X_AXIS]) {
                poolLayer->_stride[X_AXIS] = 1u;
                LogError("Warning! in layer %s: Stride x is 0, setting to 1 ", poolLayer->name.c_str());
            }
            if (0 == poolLayer->_stride[Y_AXIS]) {
                poolLayer->_stride[Y_AXIS] = 1u;
                LogError("Warning! in layer %s: Stride y is 0, setting to 1", poolLayer->name.c_str());
            }

            poolLayer->_padding.insert(X_AXIS, poolLayer->GetParamAsUInt("pad-x", 0u));
            poolLayer->_padding.insert(Y_AXIS, poolLayer->GetParamAsUInt("pad-y", 0u));

            poolLayer->_pads_end.insert(X_AXIS, poolLayer->GetParamAsUInt("pad-r", poolLayer->_padding[X_AXIS]));
            poolLayer->_pads_end.insert(Y_AXIS, poolLayer->GetParamAsUInt("pad-b", poolLayer->_padding[Y_AXIS]));

            // TODO: All kind of pool methods
            poolLayer->_exclude_pad = poolLayer->GetParamsAsBool("exclude-pad", false);
            std::string alg = poolLayer->GetParamAsString("pool-method", "max");
            poolLayer->_type = alg == "avg" ? PoolingLayer::AVG : PoolingLayer::MAX;
            if (alg != "max" && alg != "avg") {
                THROW_IE_EXCEPTION << "Layer with type `" << _type << "` has incorrect pool-type!";
            }
        }
    } else {
        for (int i = 1; i <= kernels.size(); i++) {
            poolLayer->_kernel.insert(i - 1, kernels[kernels.size() - i]);
        }

        vector<unsigned int> default_0 = vector<unsigned int> (poolLayer->_kernel.size(), 0u);
        vector<unsigned int> default_1 = vector<unsigned int> (poolLayer->_kernel.size(), 1u);

        vector<unsigned int> strides = poolLayer->GetParamAsUInts("strides", default_1);
        for (int i = 1; i <= strides.size(); i++) {
            if (strides[strides.size() - i] == 0) {
                THROW_IE_EXCEPTION << "Stride could not be 0.\nIn layer " << poolLayer->name;
            }
            poolLayer->_stride.insert(i - 1, strides[strides.size() - i]);
        }

        vector<unsigned int> pads_begin = poolLayer->GetParamAsUInts("pads_begin", default_0);
        for (int i = 1; i <= pads_begin.size(); i++) {
            poolLayer->_padding.insert(i - 1, pads_begin[pads_begin.size() - i]);
        }

        vector<unsigned int> pads_end = poolLayer->GetParamAsUInts("pads_end", pads_begin);
        for (int i = 1; i <= pads_end.size(); i++) {
            poolLayer->_pads_end.insert(i - 1, pads_end[pads_end.size() - i]);
        }

        poolLayer->_exclude_pad = poolLayer->GetParamsAsBool("exclude-pad", false);
        std::string alg = poolLayer->GetParamAsString("pool-method", "max");
        poolLayer->_type = alg == "avg" ? PoolingLayer::AVG : PoolingLayer::MAX;
        if (alg != "max" && alg != "avg") {
            THROW_IE_EXCEPTION << "Layer with type `" << _type << "` has incorrect pad-type!";
        }
    }
    // TODO: checks for presence of all required attributes, and that there's no extraneous parameters only.
}

void PoolingValidator::checkParams(const CNNLayer* layer) {
    // TODO: check that values belong to the scope of the definition according to spec
}

void PoolingValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
}

void BatchNormalizationValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<BatchNormalizationLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of BatchNormalizationLayer class";
    }
    casted->epsilon = casted->GetParamAsFloat("epsilon");
}

void BatchNormalizationValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const BatchNormalizationLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of BatchNormalizationLayer class";
    }
    float epsilon = casted->GetParamAsFloat("epsilon");
    if (epsilon < 0) {
        THROW_IE_EXCEPTION << "The value of BatchNormalization layer epsilon parameter is invalid";
    }
}

BatchNormalizationValidator::BatchNormalizationValidator(const std::string& _type) : LayerValidator(_type) {}

void BatchNormalizationValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void PowerValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<PowerLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of PowerLayer class";
    }
    casted->offset = casted->GetParamAsFloat("shift");
    casted->power = casted->GetParamAsFloat("power");
    casted->scale = casted->GetParamAsFloat("scale");
}

void PowerValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

PowerValidator::PowerValidator(const std::string& _type) : LayerValidator(_type) {}

void PowerValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void PReLUValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<PReLULayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of PReLULayer class";
    }
    casted->_channel_shared = casted->GetParamsAsBool("channel_shared", false);
}

void PReLUValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

PReLUValidator::PReLUValidator(const std::string& _type) : LayerValidator(_type) {}

void PReLUValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void ScaleShiftValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ScaleShiftLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ScaleShiftLayer class";
    }
    if (!casted->params.empty()) {
        casted->_broadcast = casted->GetParamAsUInt("broadcast", 2);
    }
}

void ScaleShiftValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

ScaleShiftValidator::ScaleShiftValidator(const std::string& _type) : LayerValidator(_type) {}

void ScaleShiftValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void TileValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<TileLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of TileLayer class";
    }
    casted->axis = casted->GetParamAsInt("axis", -1);
    casted->tiles = casted->GetParamAsInt("tiles", -1);
}

void TileValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const TileLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of TileLayer class";
    }
    int axis = casted->GetParamAsInt("axis", -1);
    int tiles = casted->GetParamAsInt("tiles", -1);
    if (axis < 0 && tiles < 0) {
        THROW_IE_EXCEPTION << "The value of Tile layer parameters is invalid";
    }
}

TileValidator::TileValidator(const std::string& _type) : LayerValidator(_type) {}

void TileValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

ReshapeValidator::ReshapeValidator(const std::string& _type) : LayerValidator(_type) {}

void ReshapeValidator::parseParams(CNNLayer *layer) {
    auto casted = dynamic_cast<ReshapeLayer *>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ReshapeLayer class";
    }
    casted->shape.clear();
    if (!casted->params.empty()) {
        if (casted->type == "Flatten") {
            casted->num_axes = casted->GetParamAsInt("end_axis", -1);
            casted->axis = casted->axis = casted->GetParamAsInt("axis", 0);
        } else {
            casted->shape = casted->GetParamAsInts("dim", {});
        }
    }
}

void ReshapeValidator::checkParams(const CNNLayer *layer) {
    auto casted = dynamic_cast<const ReshapeLayer *>(layer);
    if (!casted)
        THROW_IE_EXCEPTION << "Layer is not instance of ReshapeLayer class";
    size_t num = 0;
    for (int dim : casted->shape) {
        if (dim < -1)
            THROW_IE_EXCEPTION << "Invalid value of Reshape mask (dim attribute):" << dim
                               << ". Supported values: 0, -1, >0";
        if (dim == -1) num++;
    }
    if (num > 1) THROW_IE_EXCEPTION << "Invalid Reshape mask (dim attribute): at most one dimension can be `-1`";
}

void EltwiseValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<EltwiseLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of EltwiseLayer class";
    }
    // TODO: fix this onece we switched to IR v2.x also enable dedicated unit tests
    // @details: need to remove sum
    std::string op = casted->GetParamAsString("operation", "sum");
    // TODO: remove empty value case in IRv2.x
    if (op == "sum" || op == "") {
        casted->_operation = EltwiseLayer::Sum;
    } else if (op == "mul" || op == "prod") {
        casted->_operation = EltwiseLayer::Prod;
    } else if (op == "max") {
        casted->_operation = EltwiseLayer::Max;
    } else if (op == "sub") {
        casted->_operation = EltwiseLayer::Sub;
    } else if (op == "div") {
        casted->_operation = EltwiseLayer::Div;
    } else if (op == "min") {
        casted->_operation = EltwiseLayer::Min;
    } else if (op == "squared_diff") {
        casted->_operation = EltwiseLayer::Squared_diff;
    } else if (op == "equal") {
        casted->_operation = EltwiseLayer::Equal;
    } else if (op == "not_equal") {
        casted->_operation = EltwiseLayer::Not_equal;
    } else if (op == "less") {
        casted->_operation = EltwiseLayer::Less;
    } else if (op == "less_equal") {
        casted->_operation = EltwiseLayer::Less_equal;
    } else if (op == "greater") {
        casted->_operation = EltwiseLayer::Greater;
    } else if (op == "greater_equal") {
        casted->_operation = EltwiseLayer::Greater_equal;
    } else if (op == "logical_and") {
        casted->_operation = EltwiseLayer::Logical_AND;
    } else if (op == "logical_or") {
        casted->_operation = EltwiseLayer::Logical_OR;
    } else if (op == "logical_xor") {
        casted->_operation = EltwiseLayer::Logical_XOR;
    } else if (op == "floor_mod") {
        casted->_operation = EltwiseLayer::Floor_mod;
    } else if (op == "pow") {
        casted->_operation = EltwiseLayer::Pow;
    } else {
        THROW_IE_EXCEPTION << "Unsupported element wise operation: " << op;
    }

    auto getArray = [](std::string param, vector<float>& array) {
        std::istringstream stream(param);
        std::string str;
        while (getline(stream, str, ',')) {
            float val = std::stof(str);
            array.push_back(val);
        }
    };
    getArray(casted->GetParamAsString("coeff", ""), casted->coeff);
}

void EltwiseValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const EltwiseLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of EltwiseLayer class";
    }
}

void EltwiseValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    if (inShapes.empty()) {
        THROW_IE_EXCEPTION << "Number of inputs (" << inShapes.size() <<
                           ") of Eltwise layer is zero";
    }
}

EltwiseValidator::EltwiseValidator(const std::string& _type) : LayerValidator(_type) {}

void ClampValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ClampLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ClampLayer class";
    }
    casted->min_value = casted->GetParamAsFloat("min");
    casted->max_value = casted->GetParamAsFloat("max");
}


ClampValidator::ClampValidator(const std::string& _type) : LayerValidator(_type) {}

void ClampValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void ReLUValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ReLULayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ReLULayer class";
    }
    if (!casted->params.empty()) {
        casted->negative_slope = casted->GetParamAsFloat("negative_slope");
    }
}

void ReLUValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const ReLULayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ReLULayer class";
    }
    if (!casted->params.empty()) {
        float negative_slope = casted->GetParamAsFloat("negative_slope");
        if (negative_slope < 0) {
            THROW_IE_EXCEPTION << "The value of ReLU layer negative_slope parameter is invalid";
        }
    }
}

ReLUValidator::ReLUValidator(const std::string& _type) : LayerValidator(_type) {}

void ReLUValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
}

void MVNValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<MVNLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of MVNLayer class";
    }
    casted->across_channels = casted->GetParamAsInt("across_channels", 0);
    casted->normalize = casted->GetParamAsInt("normalize_variance", 1);
}

void MVNValidator::checkParams(const CNNLayer* layer) {
}

MVNValidator::MVNValidator(const std::string& _type) : LayerValidator(_type) {}

void MVNValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void GRNValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<GRNLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of GRNLayer class";
    }
    casted->bias = casted->GetParamAsFloat("bias", 0.f);
}

void GRNValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

GRNValidator::GRNValidator(const std::string& _type) : LayerValidator(_type) {}

void GRNValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void SoftMaxValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<SoftMaxLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of SoftMaxLayer class";
    }
    casted->axis = casted->GetParamAsInt("axis", 1);
}

void SoftMaxValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const SoftMaxLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of SoftMaxLayer class";
    }
    int axis = casted->GetParamAsInt("axis", 1);
    if (axis < 0) {
        THROW_IE_EXCEPTION << "The value of SoftMax layer axis parameter is invalid";
    }
}

SoftMaxValidator::SoftMaxValidator(const std::string& _type) : LayerValidator(_type) {}

void SoftMaxValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

void NormValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<NormLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of NormLayer class";
    }
    casted->_size = casted->GetParamAsUInt("local_size", 0);
    casted->_size += casted->GetParamAsUInt("local-size", 0);
    casted->_k = casted->GetParamAsUInt("k", 1);
    casted->_alpha = casted->GetParamAsFloat("alpha");
    casted->_beta = casted->GetParamAsFloat("beta");
    casted->_isAcrossMaps = CaselessEq<std::string>()(casted->GetParamAsString("region"), "across");
}

void NormValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const NormLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of NormLayer class";
    }
    float _alpha = casted->GetParamAsFloat("alpha");
    float _beta = casted->GetParamAsFloat("beta");
    if (_alpha < 0 && _beta < 0) {
        THROW_IE_EXCEPTION << "The value of Norm layer alpha or beta parameters is invalid";
    }
}

NormValidator::NormValidator(const std::string& _type) : LayerValidator(_type) {}

void NormValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

SplitValidator::SplitValidator(const std::string& _type) : LayerValidator(_type) {}

void SplitValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<SplitLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of SplitLayer class";
    }
    casted->_axis = casted->GetParamAsUInt("axis", 1);

    std::string out_sizes;
    for (auto& i : layer->outData) {
        if (!out_sizes.empty())
            out_sizes += ",";
        if (static_cast<int>(i->getTensorDesc().getDims().size()) <= casted->_axis) {
            THROW_IE_EXCEPTION << "Internal error - dimensions are empty";
        }
        out_sizes += std::to_string(i->getTensorDesc().getDims()[casted->_axis]);
    }
    if (!out_sizes.empty())
        casted->params["out_sizes"] = out_sizes;
}

void SplitValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
    std::vector<int> out_sizes = layer->GetParamAsInts("out_sizes", {});
    if (out_sizes.empty()) {
        THROW_IE_EXCEPTION << "Value of out_sizes attribute is empty";
    }
}

void SplitValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const SplitLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of SplitLayer class";
    }
    checkNumOfInput(inShapes, {1});
    auto version = BaseCreator::version_;
    if (version > 3) {
        std::vector<int> out_sizes = layer->GetParamAsInts("out_sizes", {});
        size_t sum(0);
        for (const auto& size : out_sizes)
            sum += size;
        if (inShapes.empty() || inShapes[0].size() <= casted->_axis)
            THROW_IE_EXCEPTION << "Layer has incorrect input shapes!";
        if (sum != inShapes[0][casted->_axis]) {
            THROW_IE_EXCEPTION << "The sum of the dimensions on the axis(" << casted->_axis
                               << ") is not equal out_sizes: " << details::dumpVec(out_sizes);
        }
    }
}

ConcatValidator::ConcatValidator(const std::string& _type) : LayerValidator(_type) {}

void ConcatValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ConcatLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of ConcatLayer class";
    }
    casted->_axis = casted->GetParamAsUInt("axis", 1);
}

void ConcatValidator::checkParams(const CNNLayer* layer) {
}

void ConcatValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    if (inShapes.empty())
        THROW_IE_EXCEPTION << "Inputs are empty";

    auto casted = dynamic_cast<const ConcatLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Invalid Concat layer.";
    }

    auto firstShape = inShapes[0];
    size_t firstShapeSize = firstShape.size();
    size_t axis = casted->_axis;
    if (axis >= firstShapeSize)
        THROW_IE_EXCEPTION << "Concat axis(" << axis
                           << ") should be less the number of current input dimensions ("
                           << firstShapeSize << ")";

    for (size_t i = 1; i < inShapes.size(); i++) {
        auto shape = inShapes[i];
        if (shape.size() != firstShapeSize)
            THROW_IE_EXCEPTION << "Invalid inputs for Concat layer: number of dimensions must match: "
                               << firstShapeSize << " vs " << shape.size();
        bool eq_part1 = std::equal(firstShape.begin(), firstShape.begin() + axis,
                                   shape.begin());
        bool eq_part2 = std::equal(firstShape.begin() + axis + 1, firstShape.end(),
                                   shape.begin() + axis + 1);
        if (!(eq_part1 && eq_part2))
            THROW_IE_EXCEPTION << "Invalid inputs for Concat layer: dimensions should match in all "
                               << "positions except axis (" << axis << ") : [" << dumpVec(firstShape) << "] vs ["
                               << dumpVec(shape) <<"]";
    }
}

GemmValidator::GemmValidator(const std::string& _type) : LayerValidator(_type) {}

void GemmValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<GemmLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of GemmLayer class";
    }
    casted->alpha = casted->GetParamAsFloat("alpha", 1);
    casted->beta = casted->GetParamAsFloat("beta", 1);
    casted->transpose_a = casted->GetParamsAsBool("transpose_a", false);
    casted->transpose_b = casted->GetParamsAsBool("transpose_b", false);
}

void GemmValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void GemmValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const GemmLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of GemmLayer class";
    }

    size_t numInputs = inShapes.size();
    checkNumOfInput(inShapes, {2, 3});

    auto dims0 = inShapes[0];
    auto dims1 = inShapes[1];
    if (dims0.size() < 2 || dims1.size() < 2) {
        THROW_IE_EXCEPTION << "Gemm input shapes must have at least 2 dimensions";
    }

    unsigned long xAxis = dims0.size() - 1;
    unsigned long yAxis = dims0.size() - 2;
    if (dims0[xAxis] != dims1[yAxis])
        THROW_IE_EXCEPTION << "Gemm input0 x dimension must be equal to input1 y dimension ("
                           << dims0[xAxis] << " vs " << dims1[yAxis] << ")";

    if (inShapes.size() == 3) {
        auto dims2 = inShapes[2];
        if (dims2.size() < 2) {
            THROW_IE_EXCEPTION << "Gemm input shapes must have at least 2 dimensions";
        }

        if (dims2[xAxis] != dims1[xAxis])
            THROW_IE_EXCEPTION << "Gemm input2 x dimension must be equal to input1 x dimension ("
                               << dims2[xAxis] << " vs " << dims1[xAxis] << ")";

        if (dims2[yAxis] != dims0[yAxis])
            THROW_IE_EXCEPTION << "Gemm input2 y dimension must be equal to input0 y dimension ("
                               << dims2[yAxis] << " vs " << dims0[yAxis] << ")";
    }
}

PadValidator::PadValidator(const std::string& _type) : LayerValidator(_type) {}

void PadValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<PadLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of PadLayer class";
    }
    std::vector<uint32_t> pads_begin = casted->GetParamAsUInts("pads_begin");
    std::vector<uint32_t> pads_end = casted->GetParamAsUInts("pads_end");

    casted->pads_begin.clear();
    for (size_t i = 0; i < pads_begin.size(); i++) {
        casted->pads_begin.insert(i, pads_begin[i]);
    }

    casted->pads_end.clear();
    for (size_t i = 0; i < pads_end.size(); i++) {
        casted->pads_end.insert(i, pads_end[i]);
    }

    casted->pad_value = casted->GetParamAsFloat("pad_value", 0.0f);

    std::string mode = casted->GetParamAsString("pad_mode", "constant");
    if (mode == "constant") {
        casted->pad_mode = PadLayer::Constant;
    } else if (mode == "edge") {
        casted->pad_mode = PadLayer::Edge;
    } else if (mode == "reflect") {
        casted->pad_mode = PadLayer::Reflect;
    } else if (mode == "symmetric") {
        casted->pad_mode = PadLayer::Symmetric;
    } else {
        THROW_IE_EXCEPTION << layer->name << " Unsupported pad mode operation: " << mode;
    }
}

void PadValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void PadValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const PadLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of PadLayer class";
    }

    size_t numInputs = inShapes.size();
    checkNumOfInput(inShapes, {1});

    if (inShapes[0].size() != casted->pads_begin.size())
        THROW_IE_EXCEPTION << layer->name << " Dimensions count mismatch in layer " << layer->name
                           << ". Expected: " << casted->pads_begin.size() << " Got: " << inShapes[0].size();

    if (inShapes[0].size() != casted->pads_end.size())
        THROW_IE_EXCEPTION << layer->name << " Dimensions count mismatch in layer " << layer->name
                           << ". Expected: " << casted->pads_end.size() << " Got: " << inShapes[0].size();

    if (casted->pad_mode == PadLayer::Symmetric || casted->pad_mode == PadLayer::Reflect) {
        for (size_t i = 0; i < inShapes[0].size(); i++) {
            if (inShapes[0][i] < casted->pads_begin[i]) {
                THROW_IE_EXCEPTION << layer->name << " Pad can't be grater than input shape in symmetric and reflect modes."
                                   << " For dimension " << i << " pad_begin=" << casted->pads_begin[i]
                                   << " in_shape="<< inShapes[0][i];
            }
            if (inShapes[0][i] < casted->pads_end[i]) {
                THROW_IE_EXCEPTION << layer->name << " Pad can't be grater than input shape in symmetric and reflect modes."
                                   << " For dimension " << i << " pad_end=" << casted->pads_end[i]
                                   << " in_shape="<< inShapes[0][i];
            }
        }
    }
}

GatherValidator::GatherValidator(const std::string& _type) : LayerValidator(_type) {}

void GatherValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<GatherLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of GatherLayer class";
    }

    casted->axis = casted->GetParamAsInt("axis", 0);
}

void GatherValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void GatherValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const GatherLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of GatherLayer class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 2)
        THROW_IE_EXCEPTION << layer->name << " Gather can take only 2 inputs, but actually it has: " << numInputs;

    if (casted->axis > 0 && inShapes[0].size() < (1 + casted->axis))
        THROW_IE_EXCEPTION << layer->name << " Incorrect input dictionary dimensions " << inShapes[0].size()
                           << " and axis number " << casted->axis;
    else if (casted->axis < 0 && (static_cast<int>(inShapes[0].size()) + casted->axis) < 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect input dictionary dimensions " << inShapes[0].size()
                           << " and axis number " << casted->axis;
}

StridedSliceValidator::StridedSliceValidator(const std::string& _type) : LayerValidator(_type) {}

void StridedSliceValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<StridedSliceLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of StridedSlice class";
    }

    casted->begin_mask = layer->GetParamAsString("begin_mask", "");
    casted->end_mask = layer->GetParamAsString("end_mask", "");
    casted->ellipsis_mask = layer->GetParamAsString("ellipsis_mask", "");
    casted->new_axis_mask = layer->GetParamAsString("new_axis_mask", "");
    casted->shrink_axis_mask = layer->GetParamAsString("shrink_axis_mask", "");
}

void StridedSliceValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void StridedSliceValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const StridedSliceLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of StridedSliceLayer class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs > 4)
        THROW_IE_EXCEPTION << layer->name << " StridedSlice can take up to 4 inputs, but actually it has: " << numInputs;

    size_t ellipsis_mask_counter = 0;
    for (size_t i = 0; i < casted->ellipsis_mask.size(); ++i) {
        if (casted->ellipsis_mask[i] == '1')
            ellipsis_mask_counter++;
    }
    if (ellipsis_mask_counter > 1)
        THROW_IE_EXCEPTION << layer->name << " 'Ellipsis_mask' must be a power of two (only one ellipsis)!";
}


ShuffleChannelsValidator::ShuffleChannelsValidator(const std::string& _type) : LayerValidator(_type) {}

void ShuffleChannelsValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ShuffleChannelsLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of ShuffleChannels class";
    }

    casted->axis = casted->GetParamAsInt("axis", 1);
    casted->group = casted->GetParamAsUInt("group", 1);
}

void ShuffleChannelsValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ShuffleChannelsValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const ShuffleChannelsLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of ShuffleChannels class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 1)
        THROW_IE_EXCEPTION << layer->name << " ShuffleChannels can take only 1 input, but actually it has: " << numInputs;

    if (casted->axis > 0 && inShapes[0].size() < (1 + casted->axis))
        THROW_IE_EXCEPTION << layer->name << "I ncorrect input tensor dimensions " << inShapes[0].size()
        << " and axis number " << casted->axis;
    else if (casted->axis < 0 && (static_cast<int>(inShapes[0].size()) + casted->axis) < 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect input dictionary dimensions " << inShapes[0].size()
        << " and axis number " << casted->axis;

    int axis = casted->axis;
    if (axis < 0)
        axis += inShapes[0].size();

    if (inShapes[0][axis] % casted->group)
        THROW_IE_EXCEPTION << layer->name << " Group parameter must evenly divide the channel dimension!";

    size_t dataLength = 1;
    for (size_t i = axis + 1; i < inShapes[0].size(); i++)
        dataLength *= inShapes[0][i];

    if (dataLength == 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect input parameters dimension!";
}


DepthToSpaceValidator::DepthToSpaceValidator(const std::string& _type) : LayerValidator(_type) {}

void DepthToSpaceValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<DepthToSpaceLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of DepthToSpace class";
    }

    casted->block_size = casted->GetParamAsUInt("block_size", 1);
}

void DepthToSpaceValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void DepthToSpaceValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const DepthToSpaceLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of DepthToSpace class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 1)
        THROW_IE_EXCEPTION << layer->name << " DepthToSpace can take only 1 input, but actually it has: " << numInputs;

    if (inShapes[0].size() < 3)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of input dimensions!";

    if (casted->block_size == 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect block_size parameter is zero!";

    if (inShapes[0][inShapes[0].size() - 3] % (casted->block_size * casted->block_size))
        THROW_IE_EXCEPTION << layer->name << " block_size parameter is incompatible with input tensor Color dimension size!";
}


SpaceToDepthValidator::SpaceToDepthValidator(const std::string& _type) : LayerValidator(_type) {}

void SpaceToDepthValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<SpaceToDepthLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of SpaceToDepth class";
    }

    casted->block_size = casted->GetParamAsUInt("block_size", 1);
}

void SpaceToDepthValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void SpaceToDepthValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const SpaceToDepthLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of SpaceToDepth class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 1)
        THROW_IE_EXCEPTION << layer->name << " SpaceToDepth can take only 1 input, but actually it has: " << numInputs;

    if (inShapes[0].size() < 2)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of input dimensions!";

    if (casted->block_size == 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect block_size parameter is zero!";

    if (inShapes[0][inShapes[0].size() - 1] % casted->block_size)
        THROW_IE_EXCEPTION << layer->name << " block_size parameter is incompatible with input tensor With dimension size!";

    if (inShapes[0][inShapes[0].size() - 2] % casted->block_size)
        THROW_IE_EXCEPTION << layer->name << " block_size parameter is incompatible with input tensor Height dimension size!";
}


ReverseSequenceValidator::ReverseSequenceValidator(const std::string& _type) : LayerValidator(_type) {}

void ReverseSequenceValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ReverseSequenceLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of ReverseSequence class";
    }

    casted->seq_axis = casted->GetParamAsInt("seq_axis", 1);
    casted->batch_axis = casted->GetParamAsInt("batch_axis", 0);
}

void ReverseSequenceValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ReverseSequenceValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const ReverseSequenceLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of ReverseSequence class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 2)
        THROW_IE_EXCEPTION << layer->name << " ReverseSequence can take 2 inputs, but actually it has: " << numInputs;

    if (inShapes[1].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'seq_lengths' input dimensions!";

    if (casted->seq_axis > 0 && inShapes[0].size() < (1 + casted->seq_axis))
        THROW_IE_EXCEPTION << layer->name << "Incorrect input tensor dimensions " << inShapes[0].size()
                           << " and seq_axis number " << casted->seq_axis;
    else if (casted->seq_axis < 0 && (static_cast<int>(inShapes[0].size()) + casted->seq_axis) < 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect input dictionary dimensions " << inShapes[0].size()
                           << " and seq_axis number " << casted->seq_axis;

    if (casted->batch_axis > 0 && inShapes[0].size() < (1 + casted->batch_axis))
        THROW_IE_EXCEPTION << layer->name << "Incorrect input tensor dimensions " << inShapes[0].size()
                           << " and batch_axis number " << casted->batch_axis;
    else if (casted->batch_axis < 0 && (static_cast<int>(inShapes[0].size()) + casted->batch_axis) < 0)
        THROW_IE_EXCEPTION << layer->name << " Incorrect input dictionary dimensions " << inShapes[0].size()
                           << " and batch_axis number " << casted->batch_axis;

    int batch_axis = casted->batch_axis;
    if (batch_axis < 0)
        batch_axis += inShapes[0].size();
    if (inShapes[1][0] != inShapes[0][batch_axis])
        THROW_IE_EXCEPTION << layer->name << " Incorrect 'seq_lengths_dims' parameter dimensions!";
}


SqueezeValidator::SqueezeValidator(const std::string& _type) : LayerValidator(_type) {}

void SqueezeValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<SqueezeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Squeeze class";
    }
}

void SqueezeValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void SqueezeValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const SqueezeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Squeeze class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 2)
        THROW_IE_EXCEPTION << layer->name << " Squeeze can take 2 inputs, but actually it has: " << numInputs;

    if (inShapes[1].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'indices_to_squeeze' input dimensions!";
}


UnsqueezeValidator::UnsqueezeValidator(const std::string& _type) : LayerValidator(_type) {}

void UnsqueezeValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<UnsqueezeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Unsqueeze class";
    }
}

void UnsqueezeValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void UnsqueezeValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const UnsqueezeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Unsqueeze class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 2)
        THROW_IE_EXCEPTION << layer->name << " Unsqueeze can take 2 inputs, but actually it has: " << numInputs;

    if (inShapes[1].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'indices_to_set' input dimensions!";
}


RangeValidator::RangeValidator(const std::string& _type) : LayerValidator(_type) {}

void RangeValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<RangeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Range class";
    }
}

void RangeValidator::checkParams(const CNNLayer* layer) {}

void RangeValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const RangeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Range class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 3)
        THROW_IE_EXCEPTION << layer->name << " Range can take 3 inputs, but actually it has: " << numInputs;

    if (inShapes[0].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'start' input dimensions!";

    if (inShapes[1].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'limit' input dimensions!";

    if (inShapes[2].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'delta' input dimensions!";
}


FillValidator::FillValidator(const std::string& _type) : LayerValidator(_type) {}

void FillValidator::parseParams(CNNLayer* layer) {}

void FillValidator::checkParams(const CNNLayer* layer) {}

void FillValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    size_t numInputs = inShapes.size();
    if (numInputs != 2)
        THROW_IE_EXCEPTION << layer->name << " Fill can take 2 inputs, but actually it has: " << numInputs;

    if (inShapes[0].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'fill_dims' input dimensions!";

    if (inShapes[1].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'fill_value' input dimensions!";
}


ExpandValidator::ExpandValidator(const std::string& _type) : LayerValidator(_type) {}

void ExpandValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<ExpandLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Expand class";
    }
}

void ExpandValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ExpandValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const ExpandLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << layer->name << " Layer is not instance of Expand class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 2)
        THROW_IE_EXCEPTION << layer->name << " Expand can take 2 inputs, but actually it has: " << numInputs;

    if (inShapes[1].size() != 1)
        THROW_IE_EXCEPTION << layer->name << " Incorrect number of 'shape' input dimensions!";
}

/****************************************/
/*** RNN specific validators ************/
/****************************************/

static RNNCellBase::CellType cell_type_from(string type_name) {
    const vector<string> to_remove {"Cell", "Sequence"};
    for (auto &sub : to_remove) {
        auto idx = type_name.find(sub);
        if (idx != string::npos)
            type_name.erase(idx);
    }

    if (!one_of(type_name, "LSTM", "RNN", "GRU"))
        THROW_IE_EXCEPTION << "Unknown RNN cell type " << type_name << ". "
                           << "Expected one of [ LSTM | RNN | GRU ].";

    return type_name == "LSTM" ? RNNSequenceLayer::LSTM :
           type_name == "GRU"  ? RNNSequenceLayer::GRU :
           type_name == "RNN"  ? RNNSequenceLayer::RNN :
           RNNSequenceLayer::LSTM;
}

static RNNSequenceLayer::Direction direction_from(string direction_name) {
    if (!one_of(direction_name, "Forward", "Backward", "Bidirectional"))
        THROW_IE_EXCEPTION << "Unknown RNN direction type " << direction_name << ". "
                           << "Expected one of [ Forward | Backward | Bidirectional ].";

    return direction_name == "Forward" ? RNNSequenceLayer::FWD :
           direction_name == "Backward" ? RNNSequenceLayer::BWD :
           direction_name == "Bidirecttional" ? RNNSequenceLayer::BDR :
           RNNSequenceLayer::FWD;
}

template<>
std::vector<std::string>
RNNBaseValidator<RNNSequenceLayer::LSTM>::def_acts {"sigmoid", "tanh", "tanh"};
template<>
std::vector<float>
RNNBaseValidator<RNNSequenceLayer::LSTM>::def_alpha {0, 0, 0};
template<>
std::vector<float>
RNNBaseValidator<RNNSequenceLayer::LSTM>::def_beta {0, 0, 0};
template<>
size_t
RNNBaseValidator<RNNSequenceLayer::LSTM>::G = 4;
template<>
size_t
RNNBaseValidator<RNNSequenceLayer::LSTM>::NS = 2;

template<>
std::vector<std::string>
RNNBaseValidator<RNNSequenceLayer::GRU>::def_acts {"sigmoid", "tanh"};
template<>
std::vector<float>
RNNBaseValidator<RNNSequenceLayer::GRU>::def_alpha {0, 0};
template<>
std::vector<float>
RNNBaseValidator<RNNSequenceLayer::GRU>::def_beta {0, 0};
template<>
size_t
RNNBaseValidator<RNNSequenceLayer::GRU>::G = 3;
template<>
size_t
RNNBaseValidator<RNNSequenceLayer::GRU>::NS = 1;

template<>
std::vector<std::string>
RNNBaseValidator<RNNSequenceLayer::RNN>::def_acts {"tanh"};
template<>
std::vector<float>
RNNBaseValidator<RNNSequenceLayer::RNN>::def_alpha {0};
template<>
std::vector<float>
RNNBaseValidator<RNNSequenceLayer::RNN>::def_beta {0};
template<>
size_t
RNNBaseValidator<RNNSequenceLayer::RNN>::G = 1;
template<>
size_t
RNNBaseValidator<RNNSequenceLayer::RNN>::NS = 1;

template<RNNSequenceLayer::CellType CELL>
RNNBaseValidator<CELL>::RNNBaseValidator(const std::string& _type) : LayerValidator(_type) {}

template<RNNSequenceLayer::CellType CELL>
void RNNBaseValidator<CELL>::parseParams(CNNLayer* layer) {
    auto rnn = dynamic_cast<RNNCellBase*>(layer);
    if (!rnn)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNLayer class";

    rnn->cellType = cell_type_from(layer->type);
    rnn->hidden_size = rnn->GetParamAsInt("hidden_size");
    rnn->clip = rnn->GetParamAsFloat("clip", 0.0f);
    rnn->activations = rnn->GetParamAsStrings("activations", def_acts);
    rnn->activation_alpha = rnn->GetParamAsFloats("activation_alpha", def_alpha);
    rnn->activation_beta = rnn->GetParamAsFloats("activation_beta", def_beta);

    if (rnn->cellType == RNNCellBase::GRU) {
        auto lbr = rnn->GetParamAsBool("linear_before_reset", false);
        if (lbr) rnn->cellType = RNNCellBase::GRU_LBR;
    }
}

template<RNNSequenceLayer::CellType CELL>
void RNNBaseValidator<CELL>::checkParams(const InferenceEngine::CNNLayer *layer) {
    auto rnn = dynamic_cast<const RNNCellBase*>(layer);
    if (!rnn)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNLayer class";

    if (rnn->clip < 0.0f)
        THROW_IE_EXCEPTION << "Clip parameter should be positive";

    for (auto &act : rnn->activations)
        if (!one_of(act, "sigmoid", "tanh", "relu"))
            THROW_IE_EXCEPTION << "Unsupported activation function (" << act << ") for RNN layer.";

    int act_num_required = def_acts.size();
    if (rnn->activations.size() != act_num_required)
        THROW_IE_EXCEPTION << "Expected " << act_num_required << " activations, but provided "
                           << rnn->activations.size();

    if (rnn->activation_alpha.size() != act_num_required)
        THROW_IE_EXCEPTION << "Expected " << act_num_required << " activation alpha parameters, "
                           << "but provided " << rnn->activation_alpha.size();
    if (rnn->activation_beta.size() != act_num_required)
        THROW_IE_EXCEPTION << "Expected " << act_num_required << " activation beta parameters, "
                           << "but provided " << rnn->activation_beta.size();
}

template<RNNSequenceLayer::CellType CELL>
void RNNBaseValidator<CELL>::checkCorrespondence(const CNNLayer* layer,
         const map<string, Blob::Ptr>& blobs,
         const vector<SizeVector>& inShapes) const {
    auto rnn = dynamic_cast<const RNNCellBase*>(layer);
    if (!rnn)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNLayer class";

    if (blobs.size() != 2)
        THROW_IE_EXCEPTION << "Expected only 2 blobs with trained parameters (weights and biases), "
                           << "but provided only " << blobs.size();
    if (inShapes.empty())
        THROW_IE_EXCEPTION << "No input tensors.";

    size_t D = inShapes[0].back();
    size_t S = rnn->hidden_size;
    size_t expectetd_w_size = G*S*(D+S);
    size_t expectetd_b_size = G*S;

    if (rnn->cellType == RNNCellBase::GRU_LBR)
        expectetd_b_size = (G + 1)*S;

    auto w = blobs.find("weights");
    if (w == blobs.end())
        THROW_IE_EXCEPTION << "Weights blob is not provided";

    if (w->second->size() != expectetd_w_size)
        THROW_IE_EXCEPTION << "Weights blob has wrang size. Expected " << expectetd_w_size;

    auto b = blobs.find("biases");
    if (b == blobs.end())
        THROW_IE_EXCEPTION << "Biases blob is not provided";

    if (b->second->size() != expectetd_b_size)
        THROW_IE_EXCEPTION << "Biases blob has wrang size. Expected " << expectetd_b_size;
}

template<RNNSequenceLayer::CellType CELL>
RNNSequenceValidator<CELL>::RNNSequenceValidator(const std::string& _type) : RNNBaseValidator<CELL>(_type) {}

template<RNNSequenceLayer::CellType CELL>
void RNNSequenceValidator<CELL>::parseParams(CNNLayer* layer) {
    RNNBaseValidator<CELL>::parseParams(layer);

    auto casted = dynamic_cast<RNNSequenceLayer*>(layer);
    if (!casted)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNLayer class";

    std::string direction = layer->GetParamAsString("direction");

    casted->axis = layer->GetParamAsUInt("axis", 1);
    casted->direction = direction_from(direction);
}

template<RNNSequenceLayer::CellType CELL>
void RNNSequenceValidator<CELL>::checkParams(const InferenceEngine::CNNLayer *layer) {
    RNNBaseValidator<CELL>::checkParams(layer);

    auto casted = dynamic_cast<const RNNSequenceLayer*>(layer);
    if (!casted)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNLayer class";

    if (!one_of(casted->axis, 1, 0))
        THROW_IE_EXCEPTION << "Unsupported iteration axis for RNNSequense layer. Only 0 or 1 axis are supported.";
}

template<RNNSequenceLayer::CellType CELL>
void RNNSequenceValidator<CELL>::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto rnn = dynamic_cast<const RNNSequenceLayer*>(layer);
    if (!rnn)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNSequenceLayer class";

    if (inShapes.empty())
        THROW_IE_EXCEPTION << "No input tensors.";

    if (inShapes[0].size() != 3)
        THROW_IE_EXCEPTION << "First input data tensor should be 3D";

    size_t T_axis = rnn->axis;
    size_t N_axis = (T_axis + 1)%2;
    size_t N = inShapes[0][N_axis];
    size_t T = inShapes[0][T_axis];
    size_t D = inShapes[0].back();
    size_t S = rnn->hidden_size;
    size_t NS = RNNSequenceValidator<CELL>::NS;

    SizeVector expected_state_shape {N, S};

    if (inShapes.size() > 1) {  // has an initial state blobs
        if (inShapes.size() != 1 + NS)
            THROW_IE_EXCEPTION << "Wrong number of input tensors. Expected 1 (data) or "
                               << 1 + NS << " (data and states)";
        if (inShapes[1] != expected_state_shape)
            THROW_IE_EXCEPTION << "Wrong shape of first initial state tensors.";
//                             << " Expected " << expected_state_shape << " but provided " << inShapes[1];

        if (NS == 2 && inShapes[2] != expected_state_shape)
            THROW_IE_EXCEPTION << "Wrong shape of second initial state tensors.";
//                             << " Expected " << expected_state_shape << " but provided " << inShapes[2];
    }
}

template class details::RNNSequenceValidator<RNNSequenceLayer::RNN>;
template class details::RNNSequenceValidator<RNNSequenceLayer::GRU>;
template class details::RNNSequenceValidator<RNNSequenceLayer::LSTM>;

template<RNNSequenceLayer::CellType CELL>
RNNCellValidator<CELL>::RNNCellValidator(const std::string& _type) : RNNBaseValidator<CELL>(_type) {}

template<RNNSequenceLayer::CellType CELL>
void RNNCellValidator<CELL>::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto rnn = dynamic_cast<const RNNCellBase*>(layer);
    if (!rnn)
        THROW_IE_EXCEPTION << "Layer is not instance of RNNSequenceLayer class";

    const size_t &NS = RNNCellValidator<CELL>::NS;

    if (inShapes.size() != NS + 1)
        THROW_IE_EXCEPTION << "Wrong number of input tensors. Expected " << NS + 1;

    if (inShapes[0].size() != 2)
        THROW_IE_EXCEPTION << "First input data tensor should be 2D";

    size_t N = inShapes[0][0];
    size_t D = inShapes[0][1];
    size_t S = rnn->hidden_size;

    SizeVector expected_state_shape {N, S};

    if (inShapes[1] != expected_state_shape)
        THROW_IE_EXCEPTION << "Wrong shape of first initial state tensors.";
//                         << " Expected " << expected_state_shape << " but provided " << inShapes[1];

    if (NS == 2 && inShapes[2] != expected_state_shape)
        THROW_IE_EXCEPTION << "Wrong shape of second initial state tensors.";
//                         << " Expected " << expected_state_shape << " but provided " << inShapes[2];
}

template class details::RNNCellValidator<RNNSequenceLayer::RNN>;
template class details::RNNCellValidator<RNNSequenceLayer::GRU>;
template class details::RNNCellValidator<RNNSequenceLayer::LSTM>;

void ArgMaxValidator::checkParams(const CNNLayer* layer) {
    unsigned int top_k_ = layer->GetParamAsUInt("top_k");
}

void ArgMaxValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

ArgMaxValidator::ArgMaxValidator(const std::string& _type) : LayerValidator(_type) {
}

void CTCGreedyDecoderValidator::checkParams(const CNNLayer* layer) {
    int flag = layer->GetParamAsInt("ctc_merge_repeated", 0);
    if (flag != 0 && flag != 1) {
        THROW_IE_EXCEPTION << "CTCGreedyDecoder layer parameter ctc_merge_repeated is invalid";
    }
}

void CTCGreedyDecoderValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
}

CTCGreedyDecoderValidator::CTCGreedyDecoderValidator(const std::string& _type) : LayerValidator(_type) {
}

void DetectionOutputValidator::parseParams(CNNLayer* layer) {
    unsigned int num_classes = layer->GetParamAsUInt("num_classes");
    if (num_classes == 0) {
        THROW_IE_EXCEPTION << "num_classes parameter of DetectionOutput layer can't be equal to zero";
    }
    float _nms_threshold = layer->GetParamAsFloat("nms_threshold");
    if (_nms_threshold < 0) {
        THROW_IE_EXCEPTION << "nms_threshold parameter of DetectionOutput layer can't be less then zero";
    }
    int _keep_top_k = layer->GetParamAsUInt("keep_top_k", -1);

    if (layer->CheckParamPresence("background_label_id"))
        int _background_label_id = layer->GetParamAsUInt("background_label_id", -1);
    if (layer->CheckParamPresence("top_k"))
        int _top_k = layer->GetParamAsUInt("top_k", -1);
    if (layer->CheckParamPresence("variance_encoded_in_target"))
        bool _variance_encoded_in_target = static_cast<bool>(layer->GetParamAsUInt("variance_encoded_in_target"));
    if (layer->CheckParamPresence("num_orient_classes"))
        int _num_orient_classes = layer->GetParamAsUInt("num_orient_classes");
    if (layer->CheckParamPresence("share_location"))
        bool _share_location = static_cast<bool>(layer->GetParamAsUInt("share_location"));
    if (layer->CheckParamPresence("interpolate_orientation"))
        int _interpolate_orientation = layer->GetParamAsInt("interpolate_orientation");
    if (layer->CheckParamPresence("confidence_threshold")) {
        float _confidence_threshold = layer->GetParamAsFloat("confidence_threshold");
        if (_confidence_threshold < 0) {
            THROW_IE_EXCEPTION << "_nms_threshold parameter of DetectionOutput layer can't be less then zero";
        }
    }

    if (layer->CheckParamPresence("code_type")) {
        std::string _code_type = layer->GetParamAsString("code_type");
        std::vector<std::string> code_types = {"caffe.PriorBoxParameter.CENTER_SIZE",
                                               "caffe.PriorBoxParameter.CORNER"};
        auto it = std::find(code_types.begin(), code_types.end(), _code_type);
        if (it == code_types.end()) {
            THROW_IE_EXCEPTION << "Parameter code_type of DetectionOutput layer ";
        }
    }
}

void DetectionOutputValidator::checkParams(const CNNLayer* layer) {
    unsigned int num_classes = layer->GetParamAsUInt("num_classes");
    if (num_classes == 0) {
        THROW_IE_EXCEPTION << "num_classes parameter of DetectionOutput layer can't be equal to zero";
    }
    float _nms_threshold = layer->GetParamAsFloat("nms_threshold");
    if (_nms_threshold < 0) {
        THROW_IE_EXCEPTION << "nms_threshold parameter of DetectionOutput layer can't be less then zero";
    }
    int _keep_top_k = layer->GetParamAsUInt("keep_top_k", -1);

    if (layer->CheckParamPresence("background_label_id"))
        int _background_label_id = layer->GetParamAsUInt("background_label_id", -1);
    if (layer->CheckParamPresence("top_k"))
        int _top_k = layer->GetParamAsUInt("top_k", -1);
    if (layer->CheckParamPresence("variance_encoded_in_target"))
        bool _variance_encoded_in_target = static_cast<bool>(layer->GetParamAsUInt("variance_encoded_in_target"));
    if (layer->CheckParamPresence("num_orient_classes"))
        int _num_orient_classes = layer->GetParamAsUInt("num_orient_classes");
    if (layer->CheckParamPresence("share_location"))
        bool _share_location = static_cast<bool>(layer->GetParamAsUInt("share_location"));
    if (layer->CheckParamPresence("interpolate_orientation"))
        int _interpolate_orientation = layer->GetParamAsInt("interpolate_orientation");
    if (layer->CheckParamPresence("confidence_threshold")) {
        float _confidence_threshold = layer->GetParamAsFloat("confidence_threshold");
        if (_confidence_threshold < 0) {
            THROW_IE_EXCEPTION << "_nms_threshold parameter of DetectionOutput layer can't be less then zero";
        }
    }
    if (layer->CheckParamPresence("code_type")) {
        std::string _code_type = layer->GetParamAsString("code_type");
        std::vector<std::string> code_types = {"caffe.PriorBoxParameter.CENTER_SIZE",
                                               "caffe.PriorBoxParameter.CORNER"};
        auto it = std::find(code_types.begin(), code_types.end(), _code_type);
        if (it == code_types.end()) {
            THROW_IE_EXCEPTION << "Parameter code_type of DetectionOutput layer ";
        }
    }
}

void DetectionOutputValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {3, 5});
}

DetectionOutputValidator::DetectionOutputValidator(const std::string& _type) : LayerValidator(_type) {
}

void InterpValidator::checkParams(const CNNLayer* layer) {
}

void InterpValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
    auto IS_ZERO = [](float value) {
        return std::fabs(value) < std::numeric_limits<float>::epsilon();
    };
    if (inShapes.size() != 2) {
        float factor = layer->GetParamAsFloat("factor", 0);
        if (factor < 0)
            THROW_IE_EXCEPTION << "factor parameter of Interp layer can't be less then zero";
        float shrink_factor = layer->GetParamAsFloat("shrink_factor", 0);
        if (shrink_factor < 0)
            THROW_IE_EXCEPTION << "shrink_factor parameter of Interp layer can't be less then zero";
        float zoom_factor = (layer->GetParamAsFloat("zoom_factor", 0));
        if (zoom_factor < 0)
            THROW_IE_EXCEPTION << "zoom_factor parameter of Interp layer can't be less then zero";
        bool noFactor = IS_ZERO(factor) && IS_ZERO(shrink_factor) && IS_ZERO(zoom_factor);

        auto height = layer->GetParamAsUInt("height", 0);
        auto width = layer->GetParamAsUInt("width", 0);

        if (noFactor && (height == 0 || width == 0)) {
            THROW_IE_EXCEPTION
                    << "Can't reshape without factor, or target resolution. "
                    << "Supported attributes: factor, shrink_factor, zoom_factor, height, width";
        }
    }
}

InterpValidator::InterpValidator(const std::string& _type) : LayerValidator(_type) {
}

void InterpValidator::parseParams(CNNLayer* layer) {
    float factor = layer->GetParamAsFloat("factor", 0);
    float shrink_factor = layer->GetParamAsFloat("shrink_factor", 0);
    float zoom_factor = layer->GetParamAsFloat("zoom_factor", 0);

    auto height = layer->GetParamAsUInt("height", 0);
    auto width = layer->GetParamAsUInt("width", 0);
}

    void PermuteValidator::checkParams(const CNNLayer* layer) {
    std::vector<unsigned int> layerOrder = layer->GetParamAsUInts("order");
}

void PermuteValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

PermuteValidator::PermuteValidator(const std::string& _type) : LayerValidator(_type) {
}

void PriorBoxValidator::checkParams(const CNNLayer* layer) {
    std::vector<unsigned int> min_sizes = layer->GetParamAsUInts("min_size", {});
    std::vector<unsigned int> max_sizes = layer->GetParamAsUInts("max_size", {});
    bool flip = static_cast<bool>(layer->GetParamAsInt("flip"));
    if (layer->CheckParamPresence("aspect_ratio"))
        const std::vector<unsigned int> aspect_ratios = layer->GetParamAsUInts("aspect_ratio", {});
    bool clip_ = static_cast<bool>(layer->GetParamAsInt("clip"));
    if (layer->CheckParamPresence("variance")) {
        float variance_ = layer->GetParamAsFloat("variance", 1.0);
        if (variance_ < 0) {
            THROW_IE_EXCEPTION << "The value of PriorBox layer variance_ parameter is invalid";
        }
    }
    float step_ = layer->GetParamAsFloat("step", 0);
    if (step_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer step_ parameter is invalid";
    }
    float offset_ = layer->GetParamAsFloat("offset");
    if (offset_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer offset_ parameter is invalid";
    }
}

void PriorBoxValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {2});
}

PriorBoxValidator::PriorBoxValidator(const std::string& _type) : LayerValidator(_type) {
}

void PriorBoxClusteredValidator::checkParams(const CNNLayer* layer) {
    std::vector<float> widths = layer->GetParamAsFloats("width", {});
    for (auto i : widths) {
        if (i < 0) {
            THROW_IE_EXCEPTION << "The value of PriorBoxClustered layer width parameter is invalid";
        }
    }
    std::vector<float> heights = layer->GetParamAsFloats("height", {});
    for (auto i : heights) {
        if (i < 0) {
            THROW_IE_EXCEPTION << "The value of PriorBoxClustered layer heights parameter is invalid";
        }
    }
    bool flip = static_cast<bool>(layer->GetParamAsInt("flip"));
    bool clip_ = static_cast<bool>(layer->GetParamAsInt("clip"));
    float offset_ = layer->GetParamAsFloat("offset");
    if (offset_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer offset_ parameter is invalid";
    }
    if (layer->CheckParamPresence("variance")) {
        float variance_ = layer->GetParamAsFloat("variance");
        if (variance_ < 0) {
            THROW_IE_EXCEPTION << "The value of PriorBox layer variance_ parameter is invalid";
        }
    }
    float step_h_ = layer->GetParamAsFloat("step_h", 0);
    if (step_h_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer step_h_ parameter is invalid";
    }
    float step_w_ = layer->GetParamAsFloat("step_w", 0);
    if (step_w_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer step_w_ parameter is invalid";
    }
    float img_h_ = layer->GetParamAsFloat("img_h", 0);
    if (img_h_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer img_h_ parameter is invalid";
    }
    float  img_w_ = layer->GetParamAsFloat("img_w", 0);
    if (img_w_ < 0) {
        THROW_IE_EXCEPTION << "The value of PriorBox layer img_w_ parameter is invalid";
    }
}

void PriorBoxClusteredValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {2});
}

PriorBoxClusteredValidator::PriorBoxClusteredValidator(const std::string& _type) : LayerValidator(_type) {
}

void ProposalValidator::checkParams(const CNNLayer* layer) {
    unsigned int post_nms_topn_ = layer->GetParamAsUInt("post_nms_topn");

    if (layer->CheckParamPresence("feat_stride"))
        unsigned int feat_stride_ = layer->GetParamAsUInt("feat_stride");
    if (layer->CheckParamPresence("base_size"))
        unsigned int base_size_ = layer->GetParamAsUInt("base_size");
    if (layer->CheckParamPresence("min_size"))
        unsigned int min_size_ = layer->GetParamAsUInt("min_size");
    if (layer->CheckParamPresence("pre_nms_topn"))
        unsigned int pre_nms_topn_ = layer->GetParamAsUInt("pre_nms_topn");
    if (layer->CheckParamPresence("nms_thresh")) {
        float nms_thresh_ = layer->GetParamAsFloat("nms_thresh");
        if (nms_thresh_ < 0) {
            THROW_IE_EXCEPTION << "The value of Proposal layer nms_thresh_ parameter is invalid";
        }
    }
}

void ProposalValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {3});
}

ProposalValidator::ProposalValidator(const std::string& _type) : LayerValidator(_type) {
}

void PSROIPoolingValidator::checkParams(const CNNLayer* layer) {
    unsigned int output_dim = layer->GetParamAsUInt("output_dim");
    unsigned int group_size = layer->GetParamAsUInt("group_size");
    if (layer->CheckParamPresence("spatial_scale")) {
        float spatial_scale_ = layer->GetParamAsFloat("spatial_scale");
        if (spatial_scale_ < 0) {
            THROW_IE_EXCEPTION << "The value of PSROIPooling layer spatial_scale_ parameter is invalid";
        }
    }
}

void PSROIPoolingValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
}

PSROIPoolingValidator::PSROIPoolingValidator(const std::string& _type) : LayerValidator(_type) {
}

void RegionYoloValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void RegionYoloValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

RegionYoloValidator::RegionYoloValidator(const std::string& _type) : LayerValidator(_type) {
}

void ReorgYoloValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ReorgYoloValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

ReorgYoloValidator::ReorgYoloValidator(const std::string& _type) : LayerValidator(_type) {
}

void ResampleValidator::checkParams(const CNNLayer* layer) {
    if (layer->CheckParamPresence("antialias")) {
        auto antialias = static_cast<size_t>(layer->GetParamAsInt("antialias"));

        if (antialias != 0 && antialias != 1) {
            THROW_IE_EXCEPTION << "The value of resample layer antialias parameter is invalid";
        }
    }
    if (layer->CheckParamPresence("type")) {
        std::string type = layer->GetParamAsString("type");
        if (type != "caffe.ResampleParameter.NEAREST" && type != "caffe.ResampleParameter.CUBIC" &&
            type != "caffe.ResampleParameter.LINEAR") {
            THROW_IE_EXCEPTION << "The value of resample layer type parameter is invalid";
        }
    }
}

void ResampleValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
}

ResampleValidator::ResampleValidator(const std::string& _type) : LayerValidator(_type) {
}

void ROIPoolingValidator::checkParams(const CNNLayer* layer) {
    unsigned int pooled_h = layer->GetParamAsUInt("pooled_h");
    unsigned int pooled_w = layer->GetParamAsUInt("pooled_w");
    float spatial_scale = layer->GetParamAsFloat("spatial_scale");
    if (spatial_scale < 0) {
        THROW_IE_EXCEPTION << "The value of ROIPooling layer spatial_scale parameter is invalid";
    }
}

void ROIPoolingValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 2});
}

ROIPoolingValidator::ROIPoolingValidator(const std::string& _type) : LayerValidator(_type) {
}

void SimplerNMSValidator::checkParams(const CNNLayer* layer) {
    unsigned int post_nms_topn_ = layer->GetParamAsUInt("post_nms_topn");

    if (layer->CheckParamPresence("min_bbox_size"))
        unsigned int min_box_size_ = layer->GetParamAsUInt("min_bbox_size");
    if (layer->CheckParamPresence("feat_stride"))
        unsigned int feat_stride_ = layer->GetParamAsUInt("feat_stride");
    if (layer->CheckParamPresence("pre_nms_topn"))
        unsigned int pre_nms_topn_ = layer->GetParamAsUInt("pre_nms_topn");
    if (layer->CheckParamPresence("iou_threshold")) {
        float iou_threshold_ = layer->GetParamAsFloat("iou_threshold");
        if (iou_threshold_ < 0) {
            THROW_IE_EXCEPTION << "The value of SimplerNMS layer iou_threshold_ parameter is invalid";
        }
    }
    if (layer->CheckParamPresence("scale"))
        std::vector<unsigned int> scale = layer->GetParamAsUInts("scale", {});
    if (layer->CheckParamPresence("cls_threshold")) {
        float cls_threshold = layer->GetParamAsFloat("cls_threshold");
        if (cls_threshold < 0) {
            THROW_IE_EXCEPTION << "The value of SimplerNMS layer cls_threshold parameter is invalid";
        }
    }
}

void SimplerNMSValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {3});
}

SimplerNMSValidator::SimplerNMSValidator(const std::string& _type) : LayerValidator(_type) {
}

void SpatialTransformerValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void SpatialTransformerValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {2});
}

SpatialTransformerValidator::SpatialTransformerValidator(const std::string& _type) : LayerValidator(_type) {
}

void UpsamplingValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void UpsamplingValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

UpsamplingValidator::UpsamplingValidator(const std::string& _type) : LayerValidator(_type) {
}

void UnpoolingValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void UnpoolingValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

UnpoolingValidator::UnpoolingValidator(const std::string& _type) : LayerValidator(_type) {
}

ActivationValidator::ActivationValidator(const std::string& _type) : LayerValidator(_type) {
}

void ActivationValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ActivationValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

ConstValidator::ConstValidator(const std::string& _type) : LayerValidator(_type) {
}

void ConstValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ConstValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {0, 1});
}

CopyValidator::CopyValidator(const std::string& _type) : LayerValidator(_type) {
}

void CopyValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void CopyValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

ELUValidator::ELUValidator(const std::string& _type) : LayerValidator(_type) {
}

void ELUValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ELUValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

InputValidator::InputValidator(const std::string& _type) : LayerValidator(_type) {
}

void InputValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void InputValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {0});
}

MemoryValidator::MemoryValidator(const std::string& _type) : LayerValidator(_type) {
}

void MemoryValidator::checkParams(const CNNLayer* layer) {
    int size = layer->GetParamAsInt("size");
    if (size != 2) {
        THROW_IE_EXCEPTION << "The value of Memory layer size parameter is invalid";
    }
}

void MemoryValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1, 0});
}

NormalizeValidator::NormalizeValidator(const std::string& _type) : LayerValidator(_type) {
}

void NormalizeValidator::checkParams(const CNNLayer* layer) {
    if (layer->CheckParamPresence("eps")) {
        float eps = layer->GetParamAsFloat("eps");
        if (eps < 0) {
            THROW_IE_EXCEPTION << "The value of Normalize layer eps parameter is invalid";
        }
    }
}

void NormalizeValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

PowerFileValidator::PowerFileValidator(const std::string& _type) : LayerValidator(_type) {
}

void PowerFileValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void PowerFileValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

ReLU6Validator::ReLU6Validator(const std::string& _type) : LayerValidator(_type) {
}

void ReLU6Validator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void ReLU6Validator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

SigmoidValidator::SigmoidValidator(const std::string& _type) : LayerValidator(_type) {
}

void SigmoidValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void SigmoidValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

TanHValidator::TanHValidator(const std::string& _type) : LayerValidator(_type) {
}

void TanHValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

QuantizeValidator::QuantizeValidator(const std::string& _type) : LayerValidator(_type) {}

void QuantizeValidator::parseParams(CNNLayer* layer) {
    auto casted = dynamic_cast<QuantizeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of QuantizeLayer class";
    }

    casted->levels = casted->GetParamAsInt("levels", 1);

    if (casted->levels <= 1) {
        THROW_IE_EXCEPTION << layer->name << ": Incorrect value for parameter levels = " << casted->levels
                           << ". Expected to be > 1.";
    }
}

void QuantizeValidator::checkParams(const CNNLayer* layer) {
    LayerValidator::checkParams(layer);
}

void QuantizeValidator::checkShapes(const CNNLayer* layer, const vector<SizeVector>& inShapes) const {
    auto casted = dynamic_cast<const QuantizeLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of QuantizeLayer class";
    }

    size_t numInputs = inShapes.size();
    if (numInputs != 5)
        THROW_IE_EXCEPTION << "Quantize can take only 5 inputs, but actually it has: " << numInputs;

    auto dims0 = inShapes[0];
    if (dims0.size() < 1) {
        THROW_IE_EXCEPTION << "Quantize input0 shape must have at least 1 dimension";
    }
}

BinaryConvolutionValidator::BinaryConvolutionValidator(const std::string& _type) : LayerValidator(_type) {}

void BinaryConvolutionValidator::parseParams(CNNLayer* layer) {
    auto binConvLayer = dynamic_cast<BinaryConvolutionLayer*>(layer);
    if (!binConvLayer) {
        THROW_IE_EXCEPTION << "Layer is not instance of BinaryConvolutionLayer class";
    }

    binConvLayer->_pad_value = binConvLayer->GetParamAsFloat("pad_value", -1.f);
    binConvLayer->_in_depth = binConvLayer->GetParamAsUInt("input");
    binConvLayer->_mode = BinaryConvolutionLayer::xnor_popcount;
    std::string mode = binConvLayer->GetParamAsString("mode", "xnor-popcount");
    if (mode != "xnor-popcount")
        THROW_IE_EXCEPTION << "Layer with type `" << _type << "` has incorrect mode!";

    binConvLayer->_out_depth = binConvLayer->GetParamAsUInt("output");

    binConvLayer->_kernel.clear();
    binConvLayer->_stride.clear();
    binConvLayer->_padding.clear();
    binConvLayer->_pads_end.clear();
    binConvLayer->_dilation.clear();

    vector<unsigned int> kernels = binConvLayer->GetParamAsUInts("kernel", {});
    if (kernels.empty()) {
        // IR_v == 2
        binConvLayer->_kernel.insert(X_AXIS, binConvLayer->GetParamAsUInt("kernel-x"));
        binConvLayer->_kernel.insert(Y_AXIS, binConvLayer->GetParamAsUInt("kernel-y"));

        binConvLayer->_stride.insert(X_AXIS, binConvLayer->GetParamAsUInt("stride-x", 1u));
        binConvLayer->_stride.insert(Y_AXIS, binConvLayer->GetParamAsUInt("stride-y", 1u));
        // TODO: maybe just throw exception, why do we change IR?
        if (0 == binConvLayer->_stride[X_AXIS]) {
            binConvLayer->_stride[X_AXIS] = 1u;
            LogError("Warning! in layer %s: Stride x is 0, setting to 1 ", binConvLayer->name.c_str());
        }
        if (0 == binConvLayer->_stride[Y_AXIS]) {
            binConvLayer->_stride[Y_AXIS] = 1u;
            LogError("Warning! in layer %s: Stride y is 0, setting to 1", binConvLayer->name.c_str());
        }

        binConvLayer->_padding.insert(X_AXIS, binConvLayer->GetParamAsUInt("pad-x", 0u));
        binConvLayer->_padding.insert(Y_AXIS, binConvLayer->GetParamAsUInt("pad-y", 0u));

        binConvLayer->_pads_end.insert(X_AXIS, binConvLayer->GetParamAsUInt("pad-r", binConvLayer->_padding[X_AXIS]));
        binConvLayer->_pads_end.insert(Y_AXIS, binConvLayer->GetParamAsUInt("pad-b", binConvLayer->_padding[Y_AXIS]));

        binConvLayer->_dilation.insert(X_AXIS, binConvLayer->GetParamAsUInt("dilation-x", 1u));
        binConvLayer->_dilation.insert(Y_AXIS, binConvLayer->GetParamAsUInt("dilation-y", 1u));
    } else {
        // IR_v > 2
        for (int i = 1; i <= kernels.size(); i++) {
            binConvLayer->_kernel.insert(i - 1, kernels[kernels.size() - i]);
        }

        vector<unsigned int> default_0 = vector<unsigned int> (binConvLayer->_kernel.size(), 0u);
        vector<unsigned int> default_1 = vector<unsigned int> (binConvLayer->_kernel.size(), 1u);

        vector<unsigned int> strides = binConvLayer->GetParamAsUInts("strides", default_1);
        for (int i = 1; i <= strides.size(); i++) {
            if (strides[strides.size() - i] == 0) {
                THROW_IE_EXCEPTION << "Stride could not be 0.\nIn layer " << binConvLayer->name;
            }
            binConvLayer->_stride.insert(i - 1, strides[strides.size() - i]);
        }

        vector<unsigned int> pads_begin = binConvLayer->GetParamAsUInts("pads_begin", default_0);
        for (int i = 1; i <= pads_begin.size(); i++) {
            binConvLayer->_padding.insert(i - 1, pads_begin[pads_begin.size() - i]);
        }

        vector<unsigned int> pads_end = binConvLayer->GetParamAsUInts("pads_end", pads_begin);
        for (int i = 1; i <= pads_end.size(); i++) {
            binConvLayer->_pads_end.insert(i - 1, pads_end[pads_end.size() - i]);
        }

        vector<unsigned int> dilations = binConvLayer->GetParamAsUInts("dilations", default_1);
        for (int i = 1; i <= dilations.size(); i++) {
            binConvLayer->_dilation.insert(i - 1, dilations[dilations.size() - i]);
        }
    }

    binConvLayer->_auto_pad = binConvLayer->GetParamAsString("auto_pad", "");
    binConvLayer->_group = binConvLayer->GetParamAsUInt("group", 1u);
}

void BinaryConvolutionValidator::checkParams(const CNNLayer* layer) {
    auto casted = dynamic_cast<const BinaryConvolutionLayer*>(layer);
    if (!casted) {
        THROW_IE_EXCEPTION << "Layer is not instance of BinaryConvolutionLayer class";
    }
}

void BinaryConvolutionValidator::checkCorrespondence(const CNNLayer* layer,
                                               const std::map<std::string, Blob::Ptr>& blobs,
                                               const vector<SizeVector>& inShapes) const {
    auto binConvLayer = dynamic_cast<const BinaryConvolutionLayer*>(layer);
    if (!binConvLayer)
        THROW_IE_EXCEPTION << "Layer is not instance of BinaryConvolutionLayer class";
}

void BinaryConvolutionValidator::checkShapes(const CNNLayer* layer, const std::vector<SizeVector>& inShapes) const {
    checkNumOfInput(inShapes, {1});
}

}  // namespace InferenceEngine
