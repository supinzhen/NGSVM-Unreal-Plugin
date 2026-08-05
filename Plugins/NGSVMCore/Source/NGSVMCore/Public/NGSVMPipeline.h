// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "NNE.h"
#include "NNERuntimeRunSync.h"

class NGSVMCORE_API INGSVMPipeline
{

public:
    virtual ~INGSVMPipeline() = default;

    /**
     * Initializes the pipeline with NNE model instance and desired resolution parameters.
     */
    virtual bool Init(
        TSharedRef<UE::NNE::IModelInstanceRunSync> InModelInstance,
        int32 InputWidth,
        int32 InputHeight
    ) = 0;

    /**
     * Preprocesses raw input frame pixel data and populates input binding buffers.
     */
    virtual bool Preprocess(const TArray<FColor>& InputPixels) = 0;

    /**
     * Postprocesses output bindings after inference.
     * Returns true if mask data is copied to OutMaskBuffer.
     */
    virtual bool Postprocess(TArray<uint8>& OutMaskBuffer) = 0;

    /**
     * Retrieves the underlying model instance.
     */
    virtual TSharedPtr<UE::NNE::IModelInstanceRunSync> GetModelInstance() = 0;

    /**
     * Get CPU bindings for model execution.
     */
    virtual const TArray<UE::NNE::FTensorBindingCPU>& GetInputBindings() const = 0;
    virtual const TArray<UE::NNE::FTensorBindingCPU>& GetOutputBindings() const = 0;
};
