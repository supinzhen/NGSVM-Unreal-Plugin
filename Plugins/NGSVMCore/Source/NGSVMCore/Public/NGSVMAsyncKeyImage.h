// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "NGSVMSettings.h"
#include "NGSVMManager.h" // for ENNEExecutionDevice / ENGSVMResolutionScale enums
#include "Containers/Ticker.h"
// Full definition (not just a forward declaration) is required here: TUniquePtr<FRHIGPUTextureReadback>
// below needs a complete type wherever the compiler generates this class's implicit
// constructor/destructor, including inside UHT-generated .gen.cpp files -- relying on some other
// unity-build-neighboring .cpp to have already included this first is fragile and broke as soon as
// this class got compiled under a different Unity Build grouping (see NGSVMManager.h for the same fix).
#include "RHIGPUReadback.h"
#include "NGSVMAsyncKeyImage.generated.h"

class INGSVMPipeline;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNGSVMKeyImageResultDelegate, UTexture2D*, ResultTexture, UTexture2D*, AlphaMatte);

/**
 * Async version of UNGSVMFunctionLibrary::NGSVM_KeyImage_asTexture2D. Nothing in this class blocks
 * the Game Thread: reading the source texture's pixels uses the same async
 * FRHIGPUTextureReadback + polling technique as the per-frame NGSVM Composure passes (Lock/Unlock
 * only ever run inside a render command, never synchronously here); the model asset is pre-warmed
 * via FStreamableManager::RequestAsyncLoad() before FNGSVMModelLoader::LoadModelData() (which
 * loads synchronously) is called, so that call resolves to an already-cached asset instead of
 * blocking on disk I/O; and the NNE inference call is likewise dispatched off the calling stack.
 */
UCLASS()
class NGSVMCORE_API UNGSVMAsyncKeyImage : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Called on the Game Thread once the keyed texture (and standalone alpha matte) are ready. */
	UPROPERTY(BlueprintAssignable, Category = "NGSVM")
	FNGSVMKeyImageResultDelegate OnSuccess;

	/** Called on the Game Thread if keying failed; ResultTexture is the original texture,
	 *  unchanged, and AlphaMatte is null. */
	UPROPERTY(BlueprintAssignable, Category = "NGSVM")
	FNGSVMKeyImageResultDelegate OnFailure;

	/**
	 * Runs NGSVM AI matting on a single static texture asynchronously. ResultTexture's alpha
	 * channel is the computed matte (straight, non-premultiplied alpha) with the RGB channels
	 * being the original image, untouched; AlphaMatte is a second, standalone PF_G8 texture
	 * containing just the grayscale matte. Unlike
	 * UNGSVMFunctionLibrary::NGSVM_KeyImage_asTexture2D, this does not block the Game Thread
	 * while inference runs.
	 */
	UFUNCTION(BlueprintCallable, Category = "NGSVM", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UNGSVMAsyncKeyImage* NGSVM_KeyImageAsync(const UObject* WorldContextObject, UTexture2D* OriginalTexture, ENGSVMModelType ModelType, ENNEExecutionDevice Device, ENGSVMResolutionScale Scale);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UTexture2D> OriginalTexture = nullptr;

	ENGSVMModelType ModelType = ENGSVMModelType::rvm_mobilenetv3_fp32;
	ENNEExecutionDevice ExecutionDevice = ENNEExecutionDevice::CPU;
	ENGSVMResolutionScale ResolutionScale = ENGSVMResolutionScale::Full;

	TUniquePtr<FRHIGPUTextureReadback> AsyncReadback;
	FTSTicker::FDelegateHandle PollTickerHandle;
	int32 PendingSrcWidth = 0;
	int32 PendingSrcHeight = 0;

	// Native-resolution pixels read back from OriginalTexture, held here (rather than passed by
	// parameter) so they survive the async model-preload hop in ContinueWithPixels().
	TArray<FColor> PendingPixels;

	// Keeps the async model asset preload alive/cancellable; reset once it completes.
	TSharedPtr<struct FStreamableHandle> ModelLoadHandle;

	// ── Diagnostics (temporary -- to find where a reported stall actually happens) ─────────────
	double DebugStartTime = 0.0;
	static void LogStep(double StartTime, const TCHAR* Step);

	// Broadcasts OnFailure with the original texture (AlphaMatte=null) and marks this action
	// ready to be destroyed.
	void Fail(const TCHAR* Reason);

	// Broadcasts OnSuccess with ResultTexture/AlphaMatte and marks this action ready to be destroyed.
	void Succeed(UTexture2D* ResultTexture, UTexture2D* AlphaMatte);

	// Kicks off the async GPU readback of OriginalTexture's pixels (PendingSrcWidth/Height must
	// already be set). Never blocks the Game Thread.
	void BeginAsyncReadback();

	// Registered with FTSTicker by BeginAsyncReadback(); polled once per frame until the readback
	// is ready. Returns true to keep polling, false to stop (readback consumed or action gone).
	bool PollReadback(float DeltaTime);

	// Stores SrcPixels into PendingPixels and kicks off an async pre-load of the model asset (if
	// not already resident in memory), then calls RunPipelineWithLoadedModel() once it's ready.
	void ContinueWithPixels(TArray<FColor>&& SrcPixels, int32 SrcWidth, int32 SrcHeight);

	// Model load -> inference -> upload, using PendingPixels/PendingSrcWidth/PendingSrcHeight.
	// Only called once the model asset is guaranteed to already be loaded/cached, so the
	// synchronous FNGSVMModelLoader::LoadModelData() call inside resolves near-instantly.
	void RunPipelineWithLoadedModel();
};
