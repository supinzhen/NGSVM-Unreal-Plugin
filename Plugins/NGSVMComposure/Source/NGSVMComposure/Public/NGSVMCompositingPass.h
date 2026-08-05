// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "CompositingElements/CompositingElementPasses.h"
#include "NGSVMSettings.h"
#include "NGSVMManager.h" // for ENNEExecutionDevice / ENGSVMResolutionScale enums
#include "NGSVMCompositingPass.generated.h"

class INGSVMPipeline;
class UTextureRenderTarget2D;
class FRHIGPUTextureReadback;

/**
 * UNGSVMCompositingPass
 *
 * A self-contained Composure Transform Pass that runs NGSVM AI matting inference
 * directly inside the Composure pipeline. Completely independent from UNGSVMManager —
 * manages its own NNE pipeline, async GPU readback, and output render targets.
 *
 * Settings and behavior mirror UNGSVMCompositePass (the CompositeCore version): same
 * auto-computed inference resolution / ResolutionScale, same aspect-preserving resize
 * with edge-clamped margins, same premultiplied-alpha keyed output.
 *
 * Usage:
 *   1. Add this pass to a CameraElement's Transform Passes list in the Composure Graph.
 *   2. Configure ModelType, ExecutionDevice, ResolutionScale.
 *   3. The pass outputs the alpha matte into OutputMaskRT (assign a Render Target asset),
 *      and, when Apply Mask To Output is enabled, replaces the pass's output texture with
 *      a native-resolution premultiplied-alpha keyed image (1 frame latency).
 */
UCLASS(BlueprintType, Blueprintable, EditInlineNew, meta = (DisplayName = "NGSVM Legacy Composure Pass"))
class NGSVMCOMPOSURE_API UNGSVMCompositingPass : public UCompositingElementTransform
{
	GENERATED_BODY()

public:
	UNGSVMCompositingPass();

	// ── Configuration ──────────────────────────────────────────────────────────

	/** AI model to use for matting inference. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	ENGSVMModelType ModelType = ENGSVMModelType::rvm_mobilenetv3_fp32;

	/** Execution device for NNE inference. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	ENNEExecutionDevice ExecutionDevice = ENNEExecutionDevice::CPU;

	/** Target inference FPS (0 = uncapped, runs every Composure frame). */
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
	 * Assign a UTextureRenderTarget2D asset here. The pass will auto-resize it
	 * to match InferenceWidth x InferenceHeight.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	UTextureRenderTarget2D* OutputMaskRT = nullptr;

	// ── Status ─────────────────────────────────────────────────────────────────

	/** Returns true if the NNE pipeline has been successfully initialized. */
	UFUNCTION(BlueprintPure, Category = "NGSVM")
	bool IsPipelineReady() const { return Pipeline.IsValid(); }

	/** Manually re-initialize the NNE pipeline (e.g., after changing model settings). */
	UFUNCTION(BlueprintCallable, Category = "NGSVM")
	void ReinitializePipeline();

	// ── UCompositingElementTransform interface ─────────────────────────────────

	/**
	 * Called by Composure each frame. Receives the element's current intermediate
	 * texture, enqueues async GPU readback, and (when the previous readback completes)
	 * submits NNE inference. Writes the matte into OutputMaskRT, and, when
	 * Apply Mask To Output is enabled, returns a premultiplied-alpha keyed texture
	 * instead of passing Input through unchanged.
	 */
	virtual UTexture* ApplyTransform_Implementation(
		UTexture* Input,
		UComposurePostProcessingPassProxy* PostProcessProxy,
		ACameraActor* TargetCamera) override;

	/** Re-enabling the pass forces a pipeline reinit, so any Model Type / Execution Device /
	 *  resolution changes made while it was disabled take effect immediately instead of
	 *  silently being ignored until some other trigger (e.g. an auto-resolution change). */
	virtual void OnEnabled_Implementation() override;

protected:
	virtual void BeginDestroy() override;

private:
	// ── NNE Pipeline ───────────────────────────────────────────────────────────

	TSharedPtr<INGSVMPipeline> Pipeline;

	bool InitPipeline();

	// ── Async GPU Readback ─────────────────────────────────────────────────────

	/** Non-blocking GPU→CPU texture readback. Replaced every frame. */
	TUniquePtr<FRHIGPUTextureReadback> AsyncReadback;
	bool bReadbackPending = false;

	/** Dimensions of the currently pending readback. */
	int32 ReadbackSrcWidth = 0;
	int32 ReadbackSrcHeight = 0;

	// ── Inference State ────────────────────────────────────────────────────────

	/** Prevents overlapping inference tasks. */
	bool bInferenceRunning = false;

	/** FPS throttle accumulator. */
	float TimeSinceLastInference = 0.0f;

	// ── Helpers ────────────────────────────────────────────────────────────────

	/** Ensures OutputMaskRT exists and matches InferenceWidth x InferenceHeight. */
	void EnsureOutputRT();

	/** Game-thread continuation once a captured frame has been locked/copied/unlocked off
	 *  the readback on the render thread: resizes for inference, runs Preprocess(), and
	 *  kicks off inference. CapturedWidth/Height are the native resolution the frame was
	 *  captured at (not the possibly-since-changed live InferenceWidth/InferenceHeight). */
	void ProcessCapturedFrame(TArray<FColor>&& SrcPixels, int32 CapturedWidth, int32 CapturedHeight);

	/** CPU bilinear resize -- aspect-preserving, same algorithm as UNGSVMCompositePass. */
	static void ResizeBilinear(
		const TArray<FColor>& Src, int32 SrcW, int32 SrcH,
		TArray<FColor>& Out, int32 DstW, int32 DstH);

	/** Same as ResizeBilinear, for the single-channel (grayscale) mask buffer. */
	static void ResizeBilinearGrayscale(
		const TArray<uint8>& Src, int32 SrcW, int32 SrcH,
		TArray<uint8>& Out, int32 DstW, int32 DstH);

	/** Uploads alpha mask bytes to OutputMaskRT via RHI. Width/Height are the resolution the
	 *  mask was actually computed at (captured when inference started), not the live
	 *  InferenceWidth/InferenceHeight members -- those can change out from under an in-flight
	 *  async inference if Auto Compute Resolution re-triggers before it completes. */
	void UploadMaskToRT(const TArray<uint8>& MaskData, int32 Width, int32 Height);

	/** Uploads the native-resolution keyed (masked) image to KeyedTexture via RHI. */
	void UploadKeyedImage(const TArray<FColor>& KeyedData, int32 Width, int32 Height);

	/** Game-thread owning render target for the keyed (masked) output, returned from
	 *  ApplyTransform_Implementation in place of Input when Apply Mask To Output is enabled.
	 *  Must be a UTextureRenderTarget2D (not a plain UTexture2D) -- downstream Composure passes
	 *  (e.g. Multi Pass Chroma Keyer) Cast<UTextureRenderTarget2D> their Input and silently no-op
	 *  if that fails, breaking the chain. */
	UPROPERTY(Transient)
	UTextureRenderTarget2D* KeyedTexture = nullptr;
};
