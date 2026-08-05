// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "NGSVMPipeline.h"

/**
 * Modnet implementation of the NGSVM pipeline.
 * MODNet is a feed-forward neural network for portrait matting.
 * Unlike RVM, it does not require maintaining recurrent states across frames.
 */
class NGSVMCORE_API FNGSVMModnetPipeline : public INGSVMPipeline
{
public:
    virtual ~FNGSVMModnetPipeline() = default;

    /**
     * Resolves the concrete shapes of input/output tensors and allocates CPU memory bindings.
     */
    virtual bool Init(
        TSharedRef<UE::NNE::IModelInstanceRunSync> InModelInstance,
        int32 InputWidth,
        int32 InputHeight
    ) override;

    /**
     * Preprocesses the input pixels by converting interleaved RGBA to planar NCHW float/half format.
     * Applies MODNet normalization: (Pixel / 255.0 - 0.5) / 0.5
     */
    virtual bool Preprocess(const TArray<FColor>& InputPixels) override;

    /**
     * Extracts the matting alpha mask from the output tensor.
     */
    virtual bool Postprocess(TArray<uint8>& OutMaskBuffer) override;

    virtual TSharedPtr<UE::NNE::IModelInstanceRunSync> GetModelInstance() override { return ModelInstance; }

    virtual const TArray<UE::NNE::FTensorBindingCPU>& GetInputBindings() const override { return InputBindings; }
    virtual const TArray<UE::NNE::FTensorBindingCPU>& GetOutputBindings() const override { return OutputBindings; }

private:
    // Reference to the active NNE model instance run interface
    TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance;

    // Model execution resolution dimensions
    int32 InputWidth = 512;
    int32 InputHeight = 512;

    // CPU data buffers holding raw input and output tensors
    TArray<TArray<uint8>> InputData;
    TArray<TArray<uint8>> OutputData;

    // NNE input and output CPU bindings pointing to the buffers above
    TArray<UE::NNE::FTensorBindingCPU> InputBindings;
    TArray<UE::NNE::FTensorBindingCPU> OutputBindings;

    // Index of the output tensor containing alpha/matte mask
    int32 AlphaOutputIndex = 0;
};