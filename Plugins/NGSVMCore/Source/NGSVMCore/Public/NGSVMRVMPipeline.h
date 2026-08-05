// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "NGSVMPipeline.h"

/**
 * Robust Video Matting (RVM) implementation of the NGSVM pipeline.
 * RVM contains recurrent states that need to be maintained across video frames
 * to keep temporal consistency and reduce flickering.
 */
class NGSVMCORE_API FNGSVMRVMPipeline : public INGSVMPipeline
{
public:
    virtual ~FNGSVMRVMPipeline() = default;

    /**
     * Resolves the concrete shapes of recurrent state tensors, allocates CPU memory
     * bindings, and maps input-output states.
     */
    virtual bool Init(
        TSharedRef<UE::NNE::IModelInstanceRunSync> InModelInstance,
        int32 InputWidth,
        int32 InputHeight) override;

    /**
     * Preprocesses the input pixels by converting interleaved RGBA to planar NCHW float/half format.
     */
    virtual bool Preprocess(const TArray<FColor> &InputPixels) override;

    /**
     * Feeds output recurrent states back to input recurrent states, and extracts the matting alpha mask.
     */
    virtual bool Postprocess(TArray<uint8> &OutMaskBuffer) override;

    virtual TSharedPtr<UE::NNE::IModelInstanceRunSync> GetModelInstance() override { return ModelInstance; }

    virtual const TArray<UE::NNE::FTensorBindingCPU> &GetInputBindings() const override { return InputBindings; }
    virtual const TArray<UE::NNE::FTensorBindingCPU> &GetOutputBindings() const override { return OutputBindings; }

private:
    // Reference to the active NNE model instance run interface
    TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance;

    // Model execution resolution dimensions
    int32 ModelInputWidth = 512;
    int32 ModelInputHeight = 512;

    // CPU data buffers holding raw input and output tensors
    TArray<TArray<uint8>> InputData;
    TArray<TArray<uint8>> OutputData;

    // NNE input and output CPU bindings pointing to the buffers above
    TArray<UE::NNE::FTensorBindingCPU> InputBindings;
    TArray<UE::NNE::FTensorBindingCPU> OutputBindings;

    // Index of the output tensor containing alpha/matte mask
    int32 AlphaOutputIndex = 1;

    // Recurrent state mappings pairing output state indices with input state indices (Key: Output, Value: Input)
    TArray<TPair<int32, int32>> StateMappings;
};
