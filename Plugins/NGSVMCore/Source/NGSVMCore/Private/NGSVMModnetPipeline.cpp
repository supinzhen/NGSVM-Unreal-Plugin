// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMModnetPipeline.h"
#include "NNE.h"
#include "Math/Float16.h"

bool FNGSVMModnetPipeline::Init(
    TSharedRef<UE::NNE::IModelInstanceRunSync> InModelInstance,
    int32 InInputWidth,
    int32 InInputHeight
)
{
    ModelInstance = InModelInstance;
    InputWidth = InInputWidth;
    InputHeight = InInputHeight;

    auto InputDescs = ModelInstance->GetInputTensorDescs();
    auto OutputDescs = ModelInstance->GetOutputTensorDescs();

    // 1. Resolve concrete input shapes
    TArray<UE::NNE::FTensorShape> InputTensorShapes;
    for (int32 i = 0; i < InputDescs.Num(); ++i)
    {
        TArray<uint32> ConcreteDims;
        TConstArrayView<int32> SymbolicDims = InputDescs[i].GetShape().GetData();
        
        if (SymbolicDims.Num() == 4)
        {
            ConcreteDims.Add(1); // Batch size (N) = 1
            int32 Channels = SymbolicDims[1];
            ConcreteDims.Add(Channels <= 0 ? 3 : (uint32)Channels); // Usually 3 channels (RGB)
            ConcreteDims.Add((uint32)InputHeight);
            ConcreteDims.Add((uint32)InputWidth);
        }
        else
        {
            for (int32 Dim : SymbolicDims)
            {
                ConcreteDims.Add(Dim <= 0 ? 1 : (uint32)Dim);
            }
        }
        InputTensorShapes.Add(UE::NNE::FTensorShape::Make(ConcreteDims));
    }

    if (ModelInstance->SetInputTensorShapes(InputTensorShapes) != UE::NNE::EResultStatus::Ok)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to set input tensor shapes in FNGSVMModnetPipeline"));
        return false;
    }

    // 2. Resolve concrete output shapes
    TArray<UE::NNE::FTensorShape> OutputTensorShapes;
    for (int32 i = 0; i < OutputDescs.Num(); ++i)
    {
        TArray<uint32> ConcreteDims;
        TConstArrayView<int32> SymbolicDims = OutputDescs[i].GetShape().GetData();

        if (SymbolicDims.Num() == 4)
        {
            ConcreteDims.Add(1);
            int32 Channels = SymbolicDims[1];
            ConcreteDims.Add(Channels <= 0 ? 1 : (uint32)Channels); // Usually 1 channel for Alpha
            ConcreteDims.Add((uint32)InputHeight);
            ConcreteDims.Add((uint32)InputWidth);
        }
        else
        {
            for (int32 Dim : SymbolicDims)
            {
                ConcreteDims.Add(Dim <= 0 ? 1 : (uint32)Dim);
            }
        }
        OutputTensorShapes.Add(UE::NNE::FTensorShape::Make(ConcreteDims));
    }

    // 3. Allocate data buffers and set up CPU tensor bindings
    int NumInputs = InputTensorShapes.Num();
    InputData.SetNum(NumInputs);
    InputBindings.SetNum(NumInputs);

    for (int i = 0; i < NumInputs; ++i)
    {
        int InputSize = (int)InputTensorShapes[i].Volume();
        int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(InputDescs[i].GetDataType());
        int32 TotalBytes = InputSize * ElementSize;

        InputData[i].SetNumZeroed(TotalBytes);
        InputBindings[i].Data = InputData[i].GetData();
        InputBindings[i].SizeInBytes = TotalBytes;
    }

    int NumOutputs = OutputTensorShapes.Num();
    OutputData.SetNum(NumOutputs);
    OutputBindings.SetNum(NumOutputs);

    for (int i = 0; i < NumOutputs; ++i)
    {
        int OutputSize = (int)OutputTensorShapes[i].Volume();
        int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(OutputDescs[i].GetDataType());
        int32 TotalBytes = OutputSize * ElementSize;

        OutputData[i].SetNumZeroed(TotalBytes);
        OutputBindings[i].Data = OutputData[i].GetData();
        OutputBindings[i].SizeInBytes = TotalBytes;
    }

    // 4. Resolve Output Alpha Tensor Index
    AlphaOutputIndex = 0;
    for (int32 i = 0; i < OutputDescs.Num(); ++i)
    {
        FString Name = OutputDescs[i].GetName().ToLower();
        if (Name.Contains(TEXT("pha")) || Name.Contains(TEXT("alpha")) || Name.Contains(TEXT("matte")))
        {
            AlphaOutputIndex = i;
            break;
        }
    }

    return true;
}

bool FNGSVMModnetPipeline::Preprocess(const TArray<FColor>& InputPixels)
{
    if (InputPixels.Num() != InputWidth * InputHeight)
    {
        return false;
    }

    auto InputDescs = ModelInstance->GetInputTensorDescs();
    ENNETensorDataType DataType = InputDescs[0].GetDataType();
    int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(DataType);
    int32 NumPixels = InputWidth * InputHeight;

    // Convert input pixels from interleaved RGBA format to planar NCHW format
    // Normalize color channels: (val / 255.0f - 0.5f) / 0.5f => range [-1.0, 1.0]
    if (ElementSize == 4) // FP32
    {
        float* FloatData = (float*)InputData[0].GetData();
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const FColor& Pixel = InputPixels[i];
            FloatData[i] = (Pixel.R / 255.0f - 0.5f) / 0.5f;
            FloatData[NumPixels + i] = (Pixel.G / 255.0f - 0.5f) / 0.5f;
            FloatData[2 * NumPixels + i] = (Pixel.B / 255.0f - 0.5f) / 0.5f;
        }
    }
    else if (ElementSize == 2) // FP16
    {
        FFloat16* HalfData = (FFloat16*)InputData[0].GetData();
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const FColor& Pixel = InputPixels[i];
            HalfData[i] = FFloat16((Pixel.R / 255.0f - 0.5f) / 0.5f);
            HalfData[NumPixels + i] = FFloat16((Pixel.G / 255.0f - 0.5f) / 0.5f);
            HalfData[2 * NumPixels + i] = FFloat16((Pixel.B / 255.0f - 0.5f) / 0.5f);
        }
    }

    return true;
}

bool FNGSVMModnetPipeline::Postprocess(TArray<uint8>& OutMaskBuffer)
{
    // Process output alpha mask: retrieve raw output data for the alpha channel
    const TArray<uint8>& RawData = OutputData[AlphaOutputIndex];
    int32 Width = InputWidth;
    int32 Height = InputHeight;

    auto OutputDescs = ModelInstance->GetOutputTensorDescs();
    ENNETensorDataType DataType = OutputDescs[AlphaOutputIndex].GetDataType();
    int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(DataType);

    if (RawData.Num() < Width * Height * ElementSize)
    {
        UE_LOG(LogTemp, Error, TEXT("Output buffer size mismatch in FNGSVMModnetPipeline"));
        return false;
    }

    OutMaskBuffer.SetNum(Width * Height);

    // Convert output floats/halves [0.0f, 1.0f] back to 8-bit grayscale pixels [0, 255]
    if (ElementSize == 4) // FP32 alpha mask conversion
    {
        const float* FloatPtr = (const float*)RawData.GetData();
        for (int32 i = 0; i < Width * Height; ++i)
        {
            float Val = FloatPtr[i];
            OutMaskBuffer[i] = (uint8)FMath::Clamp(Val * 255.0f, 0.0f, 255.0f);
        }
    }
    else if (ElementSize == 2) // FP16 alpha mask conversion
    {
        const FFloat16* HalfPtr = (const FFloat16*)RawData.GetData();
        for (int32 i = 0; i < Width * Height; ++i)
        {
            float Val = HalfPtr[i].GetFloat();
            OutMaskBuffer[i] = (uint8)FMath::Clamp(Val * 255.0f, 0.0f, 255.0f);
        }
    }

    return true;
}
