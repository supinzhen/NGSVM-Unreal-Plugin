// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Passes/CompositePassBase.h"
#include "NGSVMSettings.h"
#include "NGSVMManager.h"
#include "Tickable.h"
#include "RHIResources.h"
#include "NGSVMCompositePass.generated.h"

class INGSVMPipeline;
class UTextureRenderTarget2D;
class FRHIGPUTextureReadback;

/**
 * Thread-safe shared state for the keyed (alpha-composited) RHI texture.
 * Using TSharedPtr ensures the struct stays alive for in-flight render commands
 * even if the owning UNGSVMCompositePass UObject has been garbage-collected.
 */
struct FNGSVMKeyedTextureState
{
	/** Written on Render Thread inside ENQUEUE_RENDER_COMMAND(UpdateKeyedTexture). */
	FTextureRHIRef TextureRHI;
};

/**
 * UNGSVMCompositePass
 *
 * A self-contained Pass for UE 5.7+ CompositeActor (Composite / CompositeCore plugin system).
 * Appears directly in CompositeActor -> Layer Passes dropdown menu ("NGSVM Matting Pass").
 */
UCLASS(BlueprintType, Blueprintable, EditInlineNew, CollapseCategories, meta = (DisplayName = "NGSVM Matting Pass"))
class NGSVMCOMPOSURE_API UNGSVMCompositePass : public UCompositePassBase, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UNGSVMCompositePass(const FObjectInitializer &ObjectInitializer = FObjectInitializer::Get());
	virtual ~UNGSVMCompositePass();

	/** AI model to use for matting inference. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	ENGSVMModelType ModelType = ENGSVMModelType::rvm_mobilenetv3_fp32;

	/** Execution device for NNE inference. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	ENNEExecutionDevice ExecutionDevice = ENNEExecutionDevice::CPU;

	/** Target inference FPS (0 = uncapped). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM", meta = (ClampMin = "0.0"))
	float TargetInferenceFPS = 30.0f;

	/** Automatically compute Inference Width and Height based on input image, rounded up to multiples of 32. */
	UPROPERTY()
	bool bAutoComputeResolution = true;

	/**
	 * Fraction of the auto-detected source resolution to run inference at. The AI matting result
	 * is scaled back up to the source resolution proportionally (aspect ratio preserved, not
	 * stretched). Lower values trade matting quality for performance. Only used when Auto Compute
	 * Resolution is enabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	ENGSVMResolutionScale ResolutionScale = ENGSVMResolutionScale::Full;


	/** Inference resolution width (recommend multiples of 32). */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "NGSVM", meta = (ClampMin = "64"))
	int32 InferenceWidth = 512;

	/** Inference resolution height (recommend multiples of 32).  */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "NGSVM", meta = (ClampMin = "64"))
	int32 InferenceHeight = 512;



	/** If true, the pass outputs the video with the alpha mask applied, delaying the video by 1 frame to perfectly sync with the AI mask. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	bool bApplyMaskToOutput = true;


	/**
	 * Render Target that receives the grayscale alpha matte output (format: RTF_R8).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	UTextureRenderTarget2D *OutputMaskRT = nullptr;

	/** Returns true if the NNE pipeline has been initialized. */
	UFUNCTION(BlueprintPure, Category = "NGSVM")
	bool IsPipelineReady() const { return Pipeline.IsValid(); }

	/** Reinitialize the NNE pipeline. */
	UFUNCTION(BlueprintCallable, Category = "NGSVM")
	void ReinitializePipeline();
	/** Original input texture resolution (before downscaling for inference) */
	mutable int32 OriginalInputWidth = 0;
	mutable int32 OriginalInputHeight = 0;
	//~ Begin UObject interface
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif
	//~ End UObject interface

	//~ Begin UCompositePassBase interface
	virtual bool GetProxy(const UE::CompositeCore::FPassInputDecl &InputDecl, FSceneRenderingBulkObjectAllocator &InFrameAllocator, FCompositeCorePassProxy *&OutProxy) const override;
	//~ End UCompositePassBase interface

	//~ Begin FTickableGameObject interface (Game Thread only – safe for Readback consumption)
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UNGSVMCompositePass, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return Pipeline.IsValid() || bReadbackPending; }
	virtual bool IsTickableInEditor() const override { return true; }
	//~ End FTickableGameObject interface

protected:
	virtual void BeginDestroy() override;

private:
	TSharedPtr<INGSVMPipeline> Pipeline;
	bool InitPipeline();

	TUniquePtr<FRHIGPUTextureReadback> AsyncReadback;
	mutable bool bReadbackPending = false;
	mutable int32 ReadbackSrcWidth = 0;
	mutable int32 ReadbackSrcHeight = 0;

	mutable bool bInferenceRunning = false;
	mutable float TimeSinceLastInference = 0.0f;

	/** Tracks IsEnabled() across ticks so Tick() can detect a disabled->enabled transition
	 *  and force a pipeline reinit, picking up any Model Type / Execution Device / resolution
	 *  changes made while the pass was disabled (ProcessFrameInternal never runs while
	 *  disabled, so those changes would otherwise be silently ignored until something else
	 *  happened to trigger a reinit). */
	bool bWasEnabledLastTick = false;

	void EnsureOutputRT();
	static void ResizeBilinear(const TArray<FColor> &Src, int32 SrcW, int32 SrcH, TArray<FColor> &Out, int32 DstW, int32 DstH);
	static void ResizeBilinearGrayscale(const TArray<uint8> &Src, int32 SrcW, int32 SrcH, TArray<uint8> &Out, int32 DstW, int32 DstH);

	void UploadMaskToRT(const TArray<uint8> &MaskData);
	void UploadKeyedImage(const TArray<FColor> &KeyedData, int32 Width, int32 Height);

	void ProcessFrameInternal();

	/** Game-thread owning UTexture2D for pixel upload. */
	UPROPERTY(Transient)
	UTexture2D *KeyedTexture = nullptr;

public:
	// Called by Render Thread from within the proxy's Add() method
	void EnqueueReadback_RenderThread(class FRDGBuilder &GraphBuilder, class FRDGTexture *InputTexture) const;

	/**
	 * Shared, ref-counted state accessed by both the Game Thread (UploadKeyedImage)
	 * and the Render Thread (FCompositePassNGSVMProxy::Add). Replaces the previous
	 * raw FTextureRHIRef KeyedTextureRHI to eliminate use-after-free crashes.
	 */
	TSharedPtr<FNGSVMKeyedTextureState> KeyedTextureState;
};

