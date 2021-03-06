// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018-2019, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.
#include "test_precomp.hpp"

#ifdef HAVE_INF_ENGINE
#include <opencv2/core/utils/filesystem.hpp>

#include <inference_engine.hpp>
#include <ie_icnn_network.hpp>
#include <ie_extension.h>

namespace opencv_test { namespace {

static void initDLDTDataPath()
{
#ifndef WINRT
    static bool initialized = false;
    if (!initialized)
    {
        const char* dldtTestDataPath = getenv("INTEL_CVSDK_DIR");
        if (dldtTestDataPath)
            cvtest::addDataSearchPath(cv::utils::fs::join(dldtTestDataPath, "deployment_tools"));
        initialized = true;
    }
#endif
}

using namespace cv;
using namespace cv::dnn;
using namespace InferenceEngine;

static inline void genData(const std::vector<size_t>& dims, Mat& m, Blob::Ptr& dataPtr)
{
    std::vector<int> reversedDims(dims.begin(), dims.end());
    std::reverse(reversedDims.begin(), reversedDims.end());

    m.create(reversedDims, CV_32F);
    randu(m, -1, 1);

    dataPtr = make_shared_blob<float>(Precision::FP32, dims, (float*)m.data);
}

void runIE(Target target, const std::string& xmlPath, const std::string& binPath,
           std::map<std::string, cv::Mat>& inputsMap, std::map<std::string, cv::Mat>& outputsMap)
{
    CNNNetReader reader;
    reader.ReadNetwork(xmlPath);
    reader.ReadWeights(binPath);

    CNNNetwork net = reader.getNetwork();

    InferenceEnginePluginPtr enginePtr;
    InferencePlugin plugin;
    ExecutableNetwork netExec;
    InferRequest infRequest;
    try
    {
        auto dispatcher = InferenceEngine::PluginDispatcher({""});
        switch (target)
        {
            case DNN_TARGET_CPU:
                enginePtr = dispatcher.getSuitablePlugin(TargetDevice::eCPU);
                break;
            case DNN_TARGET_OPENCL:
            case DNN_TARGET_OPENCL_FP16:
                enginePtr = dispatcher.getSuitablePlugin(TargetDevice::eGPU);
                break;
            case DNN_TARGET_MYRIAD:
                enginePtr = dispatcher.getSuitablePlugin(TargetDevice::eMYRIAD);
                break;
            case DNN_TARGET_FPGA:
                enginePtr = dispatcher.getPluginByDevice("HETERO:FPGA,CPU");
                break;
            default:
                CV_Error(Error::StsNotImplemented, "Unknown target");
        };

        if (target == DNN_TARGET_CPU || target == DNN_TARGET_FPGA)
        {
            std::string suffixes[] = {"_avx2", "_sse4", ""};
            bool haveFeature[] = {
                checkHardwareSupport(CPU_AVX2),
                checkHardwareSupport(CPU_SSE4_2),
                true
            };
            for (int i = 0; i < 3; ++i)
            {
                if (!haveFeature[i])
                    continue;
#ifdef _WIN32
                std::string libName = "cpu_extension" + suffixes[i] + ".dll";
#else
                std::string libName = "libcpu_extension" + suffixes[i] + ".so";
#endif  // _WIN32
                try
                {
                    IExtensionPtr extension = make_so_pointer<IExtension>(libName);
                    enginePtr->AddExtension(extension, 0);
                    break;
                }
                catch(...) {}
            }
            // Some of networks can work without a library of extra layers.
        }
        plugin = InferencePlugin(enginePtr);

        netExec = plugin.LoadNetwork(net, {});
        infRequest = netExec.CreateInferRequest();
    }
    catch (const std::exception& ex)
    {
        CV_Error(Error::StsAssert, format("Failed to initialize Inference Engine backend: %s", ex.what()));
    }

    // Fill input blobs.
    inputsMap.clear();
    BlobMap inputBlobs;
    for (auto& it : net.getInputsInfo())
    {
        genData(it.second->getDims(), inputsMap[it.first], inputBlobs[it.first]);
    }
    infRequest.SetInput(inputBlobs);

    // Fill output blobs.
    outputsMap.clear();
    BlobMap outputBlobs;
    for (auto& it : net.getOutputsInfo())
    {
        genData(it.second->dims, outputsMap[it.first], outputBlobs[it.first]);
    }
    infRequest.SetOutput(outputBlobs);

    infRequest.Infer();
}

std::vector<String> getOutputsNames(const Net& net)
{
    std::vector<String> names;
    if (names.empty())
    {
        std::vector<int> outLayers = net.getUnconnectedOutLayers();
        std::vector<String> layersNames = net.getLayerNames();
        names.resize(outLayers.size());
        for (size_t i = 0; i < outLayers.size(); ++i)
            names[i] = layersNames[outLayers[i] - 1];
    }
    return names;
}

void runCV(Target target, const std::string& xmlPath, const std::string& binPath,
           const std::map<std::string, cv::Mat>& inputsMap,
           std::map<std::string, cv::Mat>& outputsMap)
{
    Net net = readNet(xmlPath, binPath);
    for (auto& it : inputsMap)
        net.setInput(it.second, it.first);
    net.setPreferableTarget(target);

    std::vector<String> outNames = getOutputsNames(net);
    std::vector<Mat> outs;
    net.forward(outs, outNames);

    outputsMap.clear();
    EXPECT_EQ(outs.size(), outNames.size());
    for (int i = 0; i < outs.size(); ++i)
    {
        EXPECT_TRUE(outputsMap.insert({outNames[i], outs[i]}).second);
    }
}

typedef TestWithParam<tuple<Target, String> > DNNTestOpenVINO;
TEST_P(DNNTestOpenVINO, models)
{
    Target target = (dnn::Target)(int)get<0>(GetParam());
    std::string modelName = get<1>(GetParam());
    std::string precision = (target == DNN_TARGET_OPENCL_FP16 || target == DNN_TARGET_MYRIAD) ? "FP16" : "FP32";

#ifdef INF_ENGINE_RELEASE
#if INF_ENGINE_RELEASE <= 2018050000
    std::string prefix = utils::fs::join("intel_models",
                         utils::fs::join(modelName,
                         utils::fs::join(precision, modelName)));
#endif
#endif

    initDLDTDataPath();
    std::string xmlPath = findDataFile(prefix + ".xml");
    std::string binPath = findDataFile(prefix + ".bin");

    std::map<std::string, cv::Mat> inputsMap;
    std::map<std::string, cv::Mat> ieOutputsMap, cvOutputsMap;
    // Single Myriad device cannot be shared across multiple processes.
    if (target == DNN_TARGET_MYRIAD)
        resetMyriadDevice();
    runIE(target, xmlPath, binPath, inputsMap, ieOutputsMap);
    runCV(target, xmlPath, binPath, inputsMap, cvOutputsMap);

    EXPECT_EQ(ieOutputsMap.size(), cvOutputsMap.size());
    for (auto& srcIt : ieOutputsMap)
    {
        auto dstIt = cvOutputsMap.find(srcIt.first);
        CV_Assert(dstIt != cvOutputsMap.end());
        double normInf = cvtest::norm(srcIt.second, dstIt->second, cv::NORM_INF);
        EXPECT_EQ(normInf, 0);
    }
}

INSTANTIATE_TEST_CASE_P(/**/,
    DNNTestOpenVINO,
    Combine(testing::ValuesIn(getAvailableTargets(DNN_BACKEND_INFERENCE_ENGINE)),
            testing::Values(
              "age-gender-recognition-retail-0013",
              "face-person-detection-retail-0002",
              "head-pose-estimation-adas-0001",
              "person-detection-retail-0002",
              "vehicle-detection-adas-0002"
            ))
);

}}
#endif  // HAVE_INF_ENGINE
