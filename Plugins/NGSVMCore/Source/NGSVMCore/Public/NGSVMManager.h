// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NGSVMPipeline.h"
#include "NGSVMSettings.h"
// Full definition (not just a forward declaration) is required here: TUniquePtr<FRHIGPUTextureReadback>
// below needs a complete type wherever the compiler generates this class's implicit
// constructor/destructor, including inside UHT-generated .gen.cpp files -- relying on some other
// unity-build-neighboring .cpp to have already included this first is fragile and broke as soon as
// this class got compiled under a different Unity Build grouping.
#include "RHIGPUReadback.h"
#include "NGSVMManager.generated.h"

class INGSVMPipeline;

/**
 * Execution device enum for Neural Network Engine (NNE) inference.
 * No CUDA option -- the stock UE NNERuntimeORT plugin only ever registers "NNERuntimeORTDml" and
 * "NNERuntimeORTCpu" (see NNERuntimeORTModule.cpp::StartupModule()); there is no CUDA execution
 * provider available without integrating a separate third-party ONNX Runtime build.
 */
UENUM(BlueprintType)
enum class ENNEExecutionDevice : uint8
{
	CPU UMETA(DisplayName = "CPU (ORT)"),
	GPU_DirectML UMETA(DisplayName = "GPU (DirectML)")
};

/** Fraction of the input source resolution to run AI matting inference at. */
UENUM(BlueprintType)
enum class ENGSVMResolutionScale : uint8
{
	Full UMETA(DisplayName = "1x"),
	Half UMETA(DisplayName = "1/2"),
	Quarter UMETA(DisplayName = "1/4"),
	Eighth UMETA(DisplayName = "1/8"),
};

/**
 * UNGSVMManager acts as the coordinator/manager component for the NGSVM AI Matting plugin.
 * It handles video frame capture, model inference dispatch to background threads,
 * latency compensation (delayed color frames), and rendering updates.
 * Can be attached to any Pawn or Actor.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class NGSVMCORE_API UNGSVMManager : public UActorComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UNGSVMManager();

protected:
	virtual void BeginPlay() override;
	virtual void BeginDestroy() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
	virtual void PostInitProperties() override;
#endif

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;

public:
	// Explicitly start the AI matting service (loads model and starts inference loop)
	UFUNCTION(BlueprintCallable, Category = "NGSVM")
	void StartNGSVMService();

	// Explicitly stop the AI matting service
	UFUNCTION(BlueprintCallable, Category = "NGSVM")
	void StopNGSVMService();

	// Check if the service is currently running
	UFUNCTION(BlueprintPure, Category = "NGSVM")
	bool IsServiceActive() const { return bServiceActive; }

public:
	// If true, start the NGSVM service automatically on BeginPlay
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	bool bAutoStart = false;

	// Optional material to draw to InputRenderTarget every frame before reading pixels (e.g., to copy camera texture to RT)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	class UMaterialInterface *InputMaterial = nullptr;

	// Source render target from webcam, media player, etc.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	class UTextureRenderTarget2D* InputRenderTarget = nullptr;

	// Select the AI model type to run for this manager instance
	UPROPERTY(EditAnywhere, Category = "NGSVM")
	ENGSVMModelType ModelType = ENGSVMModelType::rvm_mobilenetv3_fp32;

	// Select whether to run the model on CPU or GPU (DirectML or CUDA)
	UPROPERTY(EditAnywhere, Category = "NGSVM")
	ENNEExecutionDevice ExecutionDevice = ENNEExecutionDevice::CPU;

	// Inference frame rate limit (0 means no limit, run as fast as possible)
	UPROPERTY(EditAnywhere, Category = "NGSVM", meta = (ClampMin = "0.0"))
	float TargetInferenceFPS = 30.0f;

	// Fraction of InputRenderTarget's resolution to run inference at. The AI matting result is
	// scaled back up to the output resolution proportionally (aspect ratio preserved, not stretched).
	// Lower values trade matting quality for performance.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	ENGSVMResolutionScale ResolutionScale = ENGSVMResolutionScale::Full;

	// Current model input resolution. Auto-derived from InputRenderTarget's size x ResolutionScale
	// whenever InputRenderTarget is set; used directly as a fallback default otherwise.
	UPROPERTY()
	int32 InputWidth = 512;

	// See InputWidth.
	UPROPERTY()
	int32 InputHeight = 512;

	// Optional output texture width. If left at 0, it falls back to the current input render target size, or the configured model input size when no input target is set.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM", meta = (ClampMin = "0"))
	int32 OutputWidth = 0;

	// Optional output texture height. If left at 0, it falls back to the current input render target size, or the configured model input size when no input target is set.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM", meta = (ClampMin = "0"))
	int32 OutputHeight = 0;

	// Output grayscale alpha mask texture generated in real-time
	UPROPERTY(BlueprintReadOnly, Category = "NGSVM")
	class UTexture2D *OutputMaskTexture = nullptr;

	// Output synchronized color texture delayed by ColorFrameDelay frames
	UPROPERTY(BlueprintReadOnly, Category = "NGSVM")
	class UTexture2D *OutputColorTexture = nullptr;

	// Custom Render Target to write the output grayscale alpha mask into (Optional, format should be RTF_R8)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	class UTextureRenderTarget2D *OutputMaskRenderTarget = nullptr;

	// Custom Render Target to write the output synchronized color frame into (Optional, format should be RTF_RGBA8)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM")
	class UTextureRenderTarget2D *OutputColorRenderTarget = nullptr;

	// Number of frames to delay the OutputColorTexture to match AI inference latency
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM", meta = (ClampMin = "0", ClampMax = "15"))
	int32 ColorFrameDelay = 0;

private:
	// Helper variable to control editor details panel UI visibility of input resolution parameters
	UPROPERTY()
	bool bShowInputParams = true;

	// Service state flags
	bool bServiceActive = false;
	bool bIsInitialized = false;

	// Internal initialization function
	void InitializeService();

	// Tears down and recreates the pipeline (e.g. after InputWidth/InputHeight change due to
	// ResolutionScale re-evaluating against a new InputRenderTarget size).
	void ReinitializePipeline();

	// Converts a ResolutionScale enum value to its multiplier (1.0, 0.5, 0.25, 0.125).
	static float GetResolutionScaleFactor(ENGSVMResolutionScale InScale);

	// Updates editor UI visibility flag for input resolution parameters
	void UpdateInputParamsVisibility();

	// Pipeline interface instance for model execution
	TSharedPtr<INGSVMPipeline> Pipeline;

	// Model execution states
	bool bIsRunning = false;
	float TimeSinceLastInference = 0.0f;

	// Queue/history buffer for delaying original color frames to synchronize with matting output
	TArray<TArray<FColor>> ColorFrameHistory;
	TArray<FColor> CurrentDelayedColorFrame;

	// Async GPU->CPU readback of InputRenderTarget, so capturing it never forces the Game Thread
	// to stall waiting on the GPU (same pattern as UNGSVMCompositingPass). Replaced every capture.
	TUniquePtr<FRHIGPUTextureReadback> AsyncReadback;
	bool bReadbackPending = false;

	// Native resolution of the currently pending/just-consumed readback.
	int32 ReadbackSrcWidth = 0;
	int32 ReadbackSrcHeight = 0;

	// Allocates transient textures for mask and color output using the manager-configured size
	void InitOutputTexture();

	// Draws InputMaterial (if set), re-evaluates the inference resolution against
	// InputRenderTarget's current native size, and enqueues the next async GPU readback if one
	// isn't already in flight. Never blocks the Game Thread.
	void ProcessInputFrame();

	// Game-thread continuation once a captured frame has been locked/copied/unlocked off the
	// readback on the render thread: queues the delayed-color history, resizes to inference
	// resolution, runs Preprocess(), and dispatches the NNE inference call. CapturedWidth/Height
	// are the native resolution the frame was actually captured at.
	void ProcessCapturedFrame(TArray<FColor>&& SrcPixels, int32 CapturedWidth, int32 CapturedHeight);

	// Enqueues render thread updates for mask and color RHI textures.
	void UpdateTextures(const TArray<uint8> &MaskData);

	// CPU-based bilinear scaling implementation. Scales uniformly using height as the reference
	// axis, then center-crops (or pads with opaque black) width, to preserve aspect ratio.
	static void ResizeBilinear(const TArray<FColor> &SourcePixels, int32 SourceWidth, int32 SourceHeight,
							   TArray<FColor> &OutPixels, int32 TargetWidth, int32 TargetHeight);

	// Same as ResizeBilinear, for the single-channel (grayscale) mask buffer.
	static void ResizeBilinearGrayscale(const TArray<uint8> &SourcePixels, int32 SourceWidth, int32 SourceHeight,
										TArray<uint8> &OutPixels, int32 TargetWidth, int32 TargetHeight);
};
