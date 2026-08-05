// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "NNEModelData.h"
#include "NGSVMSettings.generated.h"

UENUM(BlueprintType)
enum class ENGSVMModelType : uint8
{
	rvm_mobilenetv3_fp16 UMETA(DisplayName = "RVM MobileNet V3 FP16"),
	rvm_mobilenetv3_fp32 UMETA(DisplayName = "RVM MobileNet V3 FP32"),
	rvm_resnet50_fp16    UMETA(DisplayName = "RVM ResNet50 FP16"),
	rvm_resnet50_fp32    UMETA(DisplayName = "RVM ResNet50 FP32"),
	modnet				 UMETA(DisplayName = "Modnet")
};

/**
 * Settings for the NGSVM AI Matting plugin.
 */
UCLASS(Config = Plugins, DefaultConfig, Meta = (DisplayName = "NGSVM Core"))
class NGSVMCORE_API UNGSVMSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UNGSVMSettings();



	// Soft path reference to the RVM (Robust Video Matting) model data (.uasset)
	UPROPERTY(Config, EditAnywhere, Category = "AI Models", Meta = (AllowedClasses = "/Script/NNE.NNEModelData"))
	TSoftObjectPtr<UNNEModelData> rvm_mobilenetv3_fp16;

	// Soft path reference to the RVM (Robust Video Matting) model data (.uasset)
	UPROPERTY(Config, EditAnywhere, Category = "AI Models", Meta = (AllowedClasses = "/Script/NNE.NNEModelData"))
	TSoftObjectPtr<UNNEModelData> rvm_mobilenetv3_fp32;

	// Soft path reference to the RVM (Robust Video Matting) model data (.uasset)
	UPROPERTY(Config, EditAnywhere, Category = "AI Models", Meta = (AllowedClasses = "/Script/NNE.NNEModelData"))
	TSoftObjectPtr<UNNEModelData> rvm_resnet50_fp16;

	// Soft path reference to the RVM (Robust Video Matting) model data (.uasset)
	UPROPERTY(Config, EditAnywhere, Category = "AI Models", Meta = (AllowedClasses = "/Script/NNE.NNEModelData"))
	TSoftObjectPtr<UNNEModelData> rvm_resnet50_fp32;

	// Soft path reference to the second AI model data (.uasset)
	UPROPERTY(Config, EditAnywhere, Category = "AI Models", Meta = (AllowedClasses = "/Script/NNE.NNEModelData"))
	TSoftObjectPtr<UNNEModelData> modnet;
};
