// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMModelLoader.h"
#include "NGSVMManager.h"
#include "NGSVMSettings.h"
#include "NNERuntimeCPU.h"
#include "NNERuntimeGPU.h"

TSoftObjectPtr<UNNEModelData> FNGSVMModelLoader::GetModelSoftPath(ENGSVMModelType ModelType)
{
    const UNGSVMSettings* Settings = GetDefault<UNGSVMSettings>();
    if (!Settings)
    {
        return nullptr;
    }

    // Select the soft object pointer (Soft Reference) model path based on the enum type
    switch (ModelType)
    {
    case ENGSVMModelType::rvm_mobilenetv3_fp16: return Settings->rvm_mobilenetv3_fp16;
    case ENGSVMModelType::rvm_mobilenetv3_fp32: return Settings->rvm_mobilenetv3_fp32;
    case ENGSVMModelType::rvm_resnet50_fp16:    return Settings->rvm_resnet50_fp16;
    case ENGSVMModelType::rvm_resnet50_fp32:    return Settings->rvm_resnet50_fp32;
    case ENGSVMModelType::modnet:                return Settings->modnet;
    default:                                     return nullptr;
    }
}

UNNEModelData* FNGSVMModelLoader::LoadModelData(ENGSVMModelType ModelType)
{
    TSoftObjectPtr<UNNEModelData> SelectedModelPtr = GetModelSoftPath(ModelType);
    const FString ModelName = StaticEnum<ENGSVMModelType>()->GetDisplayNameTextByValue((int64)ModelType).ToString();

    // Check if the soft reference path is valid
    if (SelectedModelPtr.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("Model path for %s is not configured in Project Settings"), *ModelName);
        return nullptr;
    }

    UE_LOG(LogTemp, Display, TEXT("Loading configured model: %s"), *ModelName);

    // Synchronously load the model asset as a UNNEModelData pointer
    UNNEModelData* ModelData = SelectedModelPtr.LoadSynchronous();
    if (!ModelData)
    {
        UE_LOG(LogTemp, Warning, TEXT("Settings Model Load (Sync) Failed: %s"), *ModelName);
    }
    else
    {
        UE_LOG(LogTemp, Display, TEXT("Settings Model Load (Sync) Success: %s"), *ModelName);
    }

    return ModelData;
}

TSharedPtr<UE::NNE::IModelInstanceRunSync> FNGSVMModelLoader::CreateModelInstance(
    UNNEModelData* ModelData,
    ENNEExecutionDevice ExecutionDevice
)
{
    if (!ModelData)
    {
        return nullptr;
    }

    TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance = nullptr;

    // Select the appropriate NNE runtime based on the specified execution device (CPU or GPU) for initialization
    if (ExecutionDevice == ENNEExecutionDevice::CPU)
    {
        // Get the NNE interface for ONNX Runtime (CPU version)
        TWeakInterfacePtr<INNERuntimeCPU> Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
        if (!Runtime.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("CPU Runtime NNERuntimeORTCpu not found."));
            return nullptr;
        }

        // Create the CPU model instance
        TSharedPtr<UE::NNE::IModelCPU> Model = Runtime->CreateModelCPU(ModelData);
        if (!Model.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("CPU Model creation failed"));
            return nullptr;
        }

        ModelInstance = Model->CreateModelInstanceCPU();
    }
    else
    {
        // Only DirectML is available (see ENNEExecutionDevice's comment) -- ExecutionDevice being
        // non-CPU here always means GPU_DirectML.
        TWeakInterfacePtr<INNERuntimeGPU> Runtime = UE::NNE::GetRuntime<INNERuntimeGPU>(TEXT("NNERuntimeORTDml"));
        if (!Runtime.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("GPU Runtime NNERuntimeORTDml not found. Please check if the plugin is enabled."));
            return nullptr;
        }

        // Create the GPU model instance
        TSharedPtr<UE::NNE::IModelGPU> Model = Runtime->CreateModelGPU(ModelData);
        if (!Model.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("GPU Model creation failed"));
            return nullptr;
        }

        ModelInstance = Model->CreateModelInstanceGPU();
    }

    return ModelInstance;
}
