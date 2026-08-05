// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "NNE.h"
#include "NNERuntimeRunSync.h"
#include "NNEModelData.h"

enum class ENGSVMModelType : uint8;
enum class ENNEExecutionDevice : uint8;

/**
 * Utility helper class for loading NNE model data and creating model execution instances.
 */
class NGSVMCORE_API FNGSVMModelLoader
{
public:
    /**
     * Resolves the soft-reference path of the model asset configured in NGSVM developer settings
     * for the given model type, without loading it. Callers that want to pre-warm the asset
     * asynchronously (e.g. via FStreamableManager) before calling LoadModelData -- which loads
     * synchronously -- can use this to get the path to request.
     */
    static TSoftObjectPtr<UNNEModelData> GetModelSoftPath(ENGSVMModelType ModelType);

    /**
     * Loads the model asset (.uasset) configured in NGSVM developer settings based on the specified model type.
     * Synchronous -- blocks the calling thread if the asset isn't already loaded/cached.
     */
    static UNNEModelData* LoadModelData(ENGSVMModelType ModelType);

    /**
     * Creates and initializes a synchronous model instance using either the CPU or GPU NNE Runtime.
     */
    static TSharedPtr<UE::NNE::IModelInstanceRunSync> CreateModelInstance(
        UNNEModelData* ModelData,
        ENNEExecutionDevice ExecutionDevice
    );
};
