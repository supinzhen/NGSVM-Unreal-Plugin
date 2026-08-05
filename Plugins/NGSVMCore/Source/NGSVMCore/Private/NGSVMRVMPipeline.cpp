// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMRVMPipeline.h"
#include "NNE.h"
#include "Math/Float16.h"

bool FNGSVMRVMPipeline::Init(
    TSharedRef<UE::NNE::IModelInstanceRunSync> InModelInstance,
    int32 InputWidth,
    int32 InputHeight
)
{
    ModelInstance = InModelInstance;
    ModelInputWidth = InputWidth;
    ModelInputHeight = InputHeight;

    auto InputDescs = ModelInstance->GetInputTensorDescs();
    auto OutputDescs = ModelInstance->GetOutputTensorDescs();

    // 1. Resolve concrete input shapes
    // RVM models use symbolic dimensions for input sizes and recurrent states. We must determine
    // the concrete shapes and pass them to the model instance before execution.
    TArray<UE::NNE::FTensorShape> InputTensorShapes;
    for (int32 i = 0; i < InputDescs.Num(); ++i)
    {
        TArray<uint32> ConcreteDims;
        TConstArrayView<int32> SymbolicDims = InputDescs[i].GetShape().GetData();
        FString TensorName = InputDescs[i].GetName().ToLower();

        if (SymbolicDims.Num() == 4)
        {
            // Recurrent state tensors (r1, r2, r3, r4) have scaled down resolutions
            uint32 Scale = 1;
            if (TensorName.Contains(TEXT("r1"))) Scale = 2;
            else if (TensorName.Contains(TEXT("r2"))) Scale = 4;
            else if (TensorName.Contains(TEXT("r3"))) Scale = 8;
            else if (TensorName.Contains(TEXT("r4"))) Scale = 16;

            ConcreteDims.Add(1); // Batch size (N) is always 1

            int32 Channels = SymbolicDims[1];
            if (Channels <= 0)
            {
                // If the channel size is symbolic (<= 0), map it to the corresponding output recurrent state tensor channel count
                Channels = 1;
                if (TensorName.Contains(TEXT("r")) && TensorName.Contains(TEXT("i")))
                {
                    FString TargetOutName = TensorName.Replace(TEXT("i"), TEXT("o"));
                    for (const auto& OutDesc : OutputDescs)
                    {
                        if (OutDesc.GetName().ToLower() == TargetOutName)
                        {
                            TConstArrayView<int32> OutSymbolicDims = OutDesc.GetShape().GetData();
                            if (OutSymbolicDims.Num() == 4 && OutSymbolicDims[1] > 0)
                            {
                                Channels = OutSymbolicDims[1];
                            }
                            break;
                        }
                    }
                }
            }
            ConcreteDims.Add((uint32)Channels);
            ConcreteDims.Add((uint32)(ModelInputHeight / Scale));
            ConcreteDims.Add((uint32)(ModelInputWidth / Scale));
        }
        else
        {
            // Preserve other non-spatial tensors or resolve symbolic dimensions to default 1
            for (int32 Dim : SymbolicDims)
            {
                ConcreteDims.Add(Dim <= 0 ? 1 : (uint32)Dim);
            }
        }
        InputTensorShapes.Add(UE::NNE::FTensorShape::Make(ConcreteDims));
    }

    // Set resolved concrete input shapes back to the NNE model instance
    if (ModelInstance->SetInputTensorShapes(InputTensorShapes) != UE::NNE::EResultStatus::Ok)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to set input tensor shapes in FNGSVMRVMPipeline"));
        return false;
    }

    // 2. Resolve concrete output shapes
    // Calculate and populate the concrete shapes of the outputs based on model descriptors
    TArray<UE::NNE::FTensorShape> OutputTensorShapes;
    for (int32 i = 0; i < OutputDescs.Num(); ++i)
    {
        TArray<uint32> ConcreteDims;
        TConstArrayView<int32> SymbolicDims = OutputDescs[i].GetShape().GetData();
        FString TensorName = OutputDescs[i].GetName().ToLower();

        if (SymbolicDims.Num() == 4)
        {
            // Resolve downscaled spatial dimensions for output recurrent state tensors
            uint32 Scale = 1;
            if (TensorName.Contains(TEXT("r1"))) Scale = 2;
            else if (TensorName.Contains(TEXT("r2"))) Scale = 4;
            else if (TensorName.Contains(TEXT("r3"))) Scale = 8;
            else if (TensorName.Contains(TEXT("r4"))) Scale = 16;

            ConcreteDims.Add(1);

            int32 Channels = SymbolicDims[1];
            ConcreteDims.Add(Channels <= 0 ? 1 : (uint32)Channels);

            ConcreteDims.Add((uint32)(ModelInputHeight / Scale));
            ConcreteDims.Add((uint32)(ModelInputWidth / Scale));
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
    // Allocate contiguous byte buffers in CPU memory to hold input/output tensor data
    int NumInputs = InputTensorShapes.Num();
    InputData.SetNum(NumInputs);
    InputBindings.SetNum(NumInputs);

    for (int i = 0; i < NumInputs; ++i)
    {
        int InputSize = (int)InputTensorShapes[i].Volume();
        int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(InputDescs[i].GetDataType());
        int32 TotalBytes = InputSize * ElementSize;

        InputData[i].SetNumZeroed(TotalBytes);

        // Certain RVM model scale inputs (like downsample ratio) need to be initialized to 1.0f
        if (InputSize == 1)
        {
            if (ElementSize == 4)
            {
                *(float*)(InputData[i].GetData()) = 1.0f;
            }
            else if (ElementSize == 2)
            {
                *(FFloat16*)(InputData[i].GetData()) = FFloat16(1.0f);
            }
        }

        // Set memory address and byte size in NNE CPU bindings
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

    // 4. Resolve Output Alpha/Foreground Tensor Indices and Recurrent State Mappings
    // Find the output index that corresponds to the matte/alpha channel
    AlphaOutputIndex = 1;
    for (int32 i = 0; i < OutputDescs.Num(); ++i)
    {
        FString Name = OutputDescs[i].GetName().ToLower();
        if (Name.Contains(TEXT("pha")) || Name.Contains(TEXT("alpha")))
        {
            AlphaOutputIndex = i;
            break;
        }
    }

    // Establish input-to-output recurrence mappings. Output states (e.g., r1o) from the current frame
    // must be copied to input states (e.g., r1i) of the next frame.
    StateMappings.Empty();
    for (int32 OutIdx = 0; OutIdx < OutputDescs.Num(); ++OutIdx)
    {
        FString OutName = OutputDescs[OutIdx].GetName().ToLower();
        if (OutName.Contains(TEXT("r")) && OutName.Contains(TEXT("o")))
        {
            FString TargetInName = OutName.Replace(TEXT("o"), TEXT("i"));
            for (int32 InIdx = 0; InIdx < InputDescs.Num(); ++InIdx)
            {
                if (InputDescs[InIdx].GetName().ToLower() == TargetInName)
                {
                    StateMappings.Add(TPair<int32, int32>(OutIdx, InIdx));
                    break;
                }
            }
        }
    }

    return true;
}

bool FNGSVMRVMPipeline::Preprocess(const TArray<FColor>& InputPixels)
{
    if (InputPixels.Num() != ModelInputWidth * ModelInputHeight)
    {
        return false;
    }

    auto InputDescs = ModelInstance->GetInputTensorDescs();
    ENNETensorDataType DataType = InputDescs[0].GetDataType();
    int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(DataType);
    int32 NumPixels = ModelInputWidth * ModelInputHeight;

    // Convert input pixels from interleaved RGBA format to planar NCHW format
    // Normalize color channels to range [0.0f, 1.0f]
    if (ElementSize == 4) // FP32 model execution path
    {
        float* FloatData = (float*)InputData[0].GetData();
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const FColor& Pixel = InputPixels[i];
            FloatData[i] = Pixel.R / 255.0f;
            FloatData[NumPixels + i] = Pixel.G / 255.0f;
            FloatData[2 * NumPixels + i] = Pixel.B / 255.0f;
        }
    }
    else if (ElementSize == 2) // FP16 model execution path
    {
        FFloat16* HalfData = (FFloat16*)InputData[0].GetData();
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const FColor& Pixel = InputPixels[i];
            HalfData[i] = FFloat16(Pixel.R / 255.0f);
            HalfData[NumPixels + i] = FFloat16(Pixel.G / 255.0f);
            HalfData[2 * NumPixels + i] = FFloat16(Pixel.B / 255.0f);
        }
    }

    return true;
}

bool FNGSVMRVMPipeline::Postprocess(TArray<uint8>& OutMaskBuffer)
{
    // Temporal consistency: copy output recurrent states to input recurrent states
    // for the next frame's inference
    for (const auto& Mapping : StateMappings)
    {
        int32 OutIdx = Mapping.Key;
        int32 InIdx = Mapping.Value;
        int32 CopySize = FMath::Min(InputData[InIdx].Num(), OutputData[OutIdx].Num());
        if (CopySize > 0)
        {
            FMemory::Memcpy(InputData[InIdx].GetData(), OutputData[OutIdx].GetData(), CopySize);
        }
    }

    // Process output alpha mask: retrieve raw output data for the alpha channel
    const TArray<uint8>& RawData = OutputData[AlphaOutputIndex];
    int32 Width = ModelInputWidth;
    int32 Height = ModelInputHeight;

    auto OutputDescs = ModelInstance->GetOutputTensorDescs();
    ENNETensorDataType DataType = OutputDescs[AlphaOutputIndex].GetDataType();
    int32 ElementSize = UE::NNE::GetTensorDataTypeSizeInBytes(DataType);

    if (RawData.Num() < Width * Height * ElementSize)
    {
        UE_LOG(LogTemp, Error, TEXT("Output buffer size mismatch in FNGSVMRVMPipeline"));
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
