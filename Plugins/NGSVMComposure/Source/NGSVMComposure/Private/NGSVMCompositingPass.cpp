// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMCompositingPass.h"
#include "NGSVMModelLoader.h"
#include "NGSVMPipeline.h"
#include "NGSVMRVMPipeline.h"
#include "NGSVMModnetPipeline.h"

#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Camera/CameraActor.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

// ─────────────────────────────────────────────────────────────────────────────

// Converts a ResolutionScale enum value to its multiplier (1.0, 0.5, 0.25, 0.125).
// Kept as an independent copy -- same as UNGSVMCompositePass and UNGSVMManager.
static float GetLegacyResolutionScaleFactor(ENGSVMResolutionScale InScale)
{
	switch (InScale)
	{
	case ENGSVMResolutionScale::Half:    return 0.5f;
	case ENGSVMResolutionScale::Quarter: return 0.25f;
	case ENGSVMResolutionScale::Eighth:  return 0.125f;
	default:                             return 1.0f;
	}
}

UNGSVMCompositingPass::UNGSVMCompositingPass()
{
	// Composure will call ApplyTransform_Implementation every frame when the pass
	// is enabled in the element's transform pass list.
	SetPassEnabled(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline initialization
// ─────────────────────────────────────────────────────────────────────────────

bool UNGSVMCompositingPass::InitPipeline()
{
	// 1. Load model data from settings
	UNNEModelData* ModelData = FNGSVMModelLoader::LoadModelData(ModelType);
	if (!ModelData)
	{
		UE_LOG(LogTemp, Error, TEXT("NGSVMCompositingPass: Failed to load model data."));
		return false;
	}

	// 2. Create NNE model instance (CPU or GPU)
	TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance =
		FNGSVMModelLoader::CreateModelInstance(ModelData, ExecutionDevice);
	if (!ModelInstance.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("NGSVMCompositingPass: Failed to create NNE model instance."));
		return false;
	}

	// 3. Instantiate the correct pipeline based on model type name
	FString EnumName = UEnum::GetValueAsString(ModelType).ToLower();

	if (EnumName.Contains(TEXT("rvm")))
	{
		TSharedPtr<FNGSVMRVMPipeline> RVMPipeline = MakeShared<FNGSVMRVMPipeline>();
		if (RVMPipeline->Init(ModelInstance.ToSharedRef(), InferenceWidth, InferenceHeight))
		{
			Pipeline = RVMPipeline;
		}
	}
	else if (EnumName.Contains(TEXT("modnet")))
	{
		TSharedPtr<FNGSVMModnetPipeline> MODNETPipeline = MakeShared<FNGSVMModnetPipeline>();
		if (MODNETPipeline->Init(ModelInstance.ToSharedRef(), InferenceWidth, InferenceHeight))
		{
			Pipeline = MODNETPipeline;
		}
	}

	if (!Pipeline.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("NGSVMCompositingPass: Pipeline Init() failed."));
		return false;
	}

	UE_LOG(LogTemp, Display, TEXT("NGSVMCompositingPass: Pipeline initialized successfully (%s)."),
		*UEnum::GetValueAsString(ModelType));

	EnsureOutputRT();
	return true;
}

void UNGSVMCompositingPass::ReinitializePipeline()
{
	// See UNGSVMCompositePass::ReinitializePipeline: a GPU readback copy may still be in
	// flight holding a raw AsyncReadback pointer, so flush before freeing it here.
	if (bReadbackPending && AsyncReadback)
	{
		FlushRenderingCommands();
	}

	Pipeline.Reset();
	AsyncReadback.Reset();
	bReadbackPending = false;
	bInferenceRunning = false;
	TimeSinceLastInference = 0.0f;

	InitPipeline();
}

void UNGSVMCompositingPass::OnEnabled_Implementation()
{
	// Model Type / Execution Device / resolution settings may have been changed while the
	// pass was disabled (ApplyTransform_Implementation never runs while disabled, so those
	// changes were never picked up). Force a reinit now so re-enabling always reflects the
	// current settings rather than silently continuing to run the stale pipeline.
	ReinitializePipeline();
}

// ─────────────────────────────────────────────────────────────────────────────
// UCompositingElementTransform — called by Composure every frame
// ─────────────────────────────────────────────────────────────────────────────

UTexture* UNGSVMCompositingPass::ApplyTransform_Implementation(
	UTexture* Input,
	UComposurePostProcessingPassProxy* PostProcessProxy,
	ACameraActor* TargetCamera)
{
	// ── Lazy pipeline init ─────────────────────────────────────────────────────
	if (!Pipeline.IsValid())
	{
		if (!InitPipeline())
		{
			// Return input unchanged so Composure chain isn't broken
			return Input;
		}
	}

	// ── FPS throttle ───────────────────────────────────────────────────────────
	// Composure may call this at display FPS; we don't need matting at every frame.
	const float DeltaTime = FApp::GetDeltaTime();
	TimeSinceLastInference += DeltaTime;
	const float TargetInterval = TargetInferenceFPS > 0.0f ? (1.0f / TargetInferenceFPS) : 0.0f;
	const bool bShouldInfer = (TimeSinceLastInference >= TargetInterval) && !bInferenceRunning;

	// ── Resolve source render target ───────────────────────────────────────────
	// Input coming from Composure is typically a UTextureRenderTarget2D.
	UTextureRenderTarget2D* SourceRT = Cast<UTextureRenderTarget2D>(Input);
	if (!SourceRT)
	{
		return Input;
	}

	FTextureRenderTargetResource* RTResource = SourceRT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return Input;
	}

	// ── Auto-compute inference resolution ──────────────────────────────────────
	// Same rounding as UNGSVMCompositePass/UNGSVMManager: round width and height
	// independently to a multiple of 32, from the current native source size x
	// ResolutionScale. Runs synchronously (unlike the RDG-based UNGSVMCompositePass,
	// ApplyTransform_Implementation is already on the Game Thread, so no thread-hop
	// is needed to mutate InferenceWidth/Height or reinit the pipeline).
	if (bAutoComputeResolution && SourceRT->SizeX >= 64 && SourceRT->SizeY >= 64)
	{
		const float Scale = GetLegacyResolutionScaleFactor(ResolutionScale);
		const int32 NewW = FMath::Clamp((FMath::RoundToInt(SourceRT->SizeX * Scale) + 31) & ~31, 64, 4096);
		const int32 NewH = FMath::Clamp((FMath::RoundToInt(SourceRT->SizeY * Scale) + 31) & ~31, 64, 4096);
		if (InferenceWidth != NewW || InferenceHeight != NewH)
		{
			InferenceWidth = NewW;
			InferenceHeight = NewH;
			ReinitializePipeline();
		}
	}

	// ── Step 1: If the previous async readback is ready, consume it ────────────
	// AsyncReadback->Lock()/Unlock() internally call RHICmdList.MapStagingSurface(), which
	// asserts IsInRenderingThread() -- this function is NOT guaranteed to run on the render
	// thread (e.g. the classic Composure editor's live-preview refresh calls
	// ApplyTransform_Implementation from a different thread context than gameplay Tick()),
	// so the actual Lock/Unlock must happen inside an ENQUEUE_RENDER_COMMAND, not here.
	if (bReadbackPending && AsyncReadback && AsyncReadback->IsReady())
	{
		// Claim it immediately so Step 2 below can enqueue a fresh capture this same frame,
		// same as before this was made async -- the render command below still holds the
		// object it needs via the raw pointer/TWeakObjectPtr captures.
		bReadbackPending = false;

		FRHIGPUTextureReadback* ReadbackPtr = AsyncReadback.Get();
		const int32 CopyWidth = ReadbackSrcWidth;
		const int32 CopyHeight = ReadbackSrcHeight;

		if (bShouldInfer)
		{
			TWeakObjectPtr<UNGSVMCompositingPass> WeakThis(this);
			ENQUEUE_RENDER_COMMAND(NGSVMLockReadback)(
				[ReadbackPtr, CopyWidth, CopyHeight, WeakThis](FRHICommandListImmediate&)
				{
					int32 RowPitchInPixels = 0;
					void* RawData = ReadbackPtr->Lock(RowPitchInPixels);

					TArray<FColor> SrcPixels;
					if (RawData)
					{
						SrcPixels.SetNum(CopyWidth * CopyHeight);
						const FColor* SrcRow = static_cast<const FColor*>(RawData);
						for (int32 Row = 0; Row < CopyHeight; ++Row)
						{
							FMemory::Memcpy(
								SrcPixels.GetData() + Row * CopyWidth,
								SrcRow + Row * RowPitchInPixels,
								CopyWidth * sizeof(FColor));
						}
					}
					ReadbackPtr->Unlock();

					if (SrcPixels.Num() > 0)
					{
						AsyncTask(ENamedThreads::GameThread, [WeakThis, SrcPixels = MoveTemp(SrcPixels), CopyWidth, CopyHeight]() mutable
						{
							if (UNGSVMCompositingPass* StrongThis = WeakThis.Get())
							{
								StrongThis->ProcessCapturedFrame(MoveTemp(SrcPixels), CopyWidth, CopyHeight);
							}
						});
					}
				});
		}
		else
		{
			// Not time to infer yet -- still need to Lock+Unlock on the render thread to
			// release the staging resource so it can be reused for the next capture.
			ENQUEUE_RENDER_COMMAND(NGSVMDiscardReadback)(
				[ReadbackPtr](FRHICommandListImmediate&)
				{
					int32 RowPitchInPixels = 0;
					if (ReadbackPtr->Lock(RowPitchInPixels))
					{
						ReadbackPtr->Unlock();
					}
				});
		}
	}

	// ── Step 2: Enqueue next async readback (non-blocking) ────────────────────
	if (!bReadbackPending)
	{
		if (!AsyncReadback)
		{
			AsyncReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("NGSVMCompositeReadback"));
		}

		ReadbackSrcWidth  = SourceRT->SizeX;
		ReadbackSrcHeight = SourceRT->SizeY;

		ENQUEUE_RENDER_COMMAND(NGSVMEnqueueReadback)(
			[ReadbackPtr = AsyncReadback.Get(), RTResource, Width = ReadbackSrcWidth, Height = ReadbackSrcHeight](FRHICommandListImmediate& RHICmdList)
			{
				FTextureRHIRef SrcTex = RTResource->GetTextureRHI();
				if (!SrcTex.IsValid())
				{
					return;
				}

				// Convert to a known PF_B8G8R8A8 LDR texture before reading back -- same as
				// UNGSVMCompositePass::EnqueueReadback_RenderThread. SourceRT's actual pixel
				// format isn't guaranteed to be BGRA8 (Composure intermediate render targets
				// can be HDR float formats); reinterpreting those raw bytes as FColor on the
				// CPU side produces garbage. There's no FRDGBuilder available in this classic
				// Composure callback (unlike the RDG-based UNGSVMCompositePass), so open a
				// small ad-hoc graph just for this conversion + readback.
				FRDGBuilder GraphBuilder(RHICmdList);
				FRDGTextureRef SrcRDG = RegisterExternalTexture(GraphBuilder, SrcTex, TEXT("NGSVMLegacySource"));

				// Match the source's sRGB flag on the temp texture -- SourceRT (a video/UI
				// plate render target) is typically sRGB-encoded. Sampling an sRGB source
				// implicitly linearizes on read; writing that into a non-sRGB destination
				// stores it as-is with no re-encode, making the captured colors read too dark
				// by roughly one gamma step. Propagating the flag keeps the blit gamma-neutral.
				const ETextureCreateFlags SRGBFlag = EnumHasAnyFlags(SrcTex->GetDesc().Flags, TexCreate_SRGB) ? TexCreate_SRGB : TexCreate_None;
				FRDGTextureDesc TempDesc = FRDGTextureDesc::Create2D(
					FIntPoint(Width, Height),
					PF_B8G8R8A8,
					FClearValueBinding::None,
					TexCreate_RenderTargetable | TexCreate_ShaderResource | SRGBFlag);
				FRDGTextureRef TempTexture = GraphBuilder.CreateTexture(TempDesc, TEXT("NGSVM_LegacyReadbackTemp"));

				FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
				FRDGDrawTextureInfo DrawInfo;
				AddDrawTexturePass(GraphBuilder, ShaderMap, SrcRDG, TempTexture, DrawInfo);

				AddEnqueueCopyPass(GraphBuilder, ReadbackPtr, TempTexture);

				GraphBuilder.Execute();
			});

		bReadbackPending = true;
	}

	// Return the keyed (masked) texture in place of the input when ready and requested;
	// otherwise pass Input through unchanged (matte is still in OutputMaskRT).
	if (bApplyMaskToOutput && KeyedTexture && KeyedTexture->GetResource() && KeyedTexture->GetResource()->TextureRHI)
	{
		return KeyedTexture;
	}

	return Input;
}

// ─────────────────────────────────────────────────────────────────────────────
// Game-thread continuation after a readback has been locked/copied/unlocked on the
// render thread (see the ENQUEUE_RENDER_COMMAND(NGSVMLockReadback) block above).
// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMCompositingPass::ProcessCapturedFrame(TArray<FColor>&& SrcPixels, int32 CapturedWidth, int32 CapturedHeight)
{
	if (!Pipeline.IsValid())
	{
		return;
	}

	// Resize to inference resolution if needed (aspect-preserving)
	TArray<FColor> InferencePixels;
	if (CapturedWidth == InferenceWidth && CapturedHeight == InferenceHeight)
	{
		InferencePixels = SrcPixels; // copy: SrcPixels (full native resolution) is kept below
	}
	else
	{
		ResizeBilinear(SrcPixels, CapturedWidth, CapturedHeight,
		               InferencePixels, InferenceWidth, InferenceHeight);
	}

	// Keep the FULL native-resolution color frame for the final keyed output. Only
	// InferencePixels (possibly downscaled per ResolutionScale) goes to the AI model.
	TArray<FColor> CapturedPixels = MoveTemp(SrcPixels);

	// Preprocess into pipeline input buffers
	if (!Pipeline->Preprocess(InferencePixels))
	{
		return;
	}

	bInferenceRunning = true;
	TimeSinceLastInference = 0.0f;

	TSharedPtr<INGSVMPipeline> PipelineRef = Pipeline;
	TWeakObjectPtr<UNGSVMCompositingPass> WeakThis(this);
	const bool bShouldApplyMask = bApplyMaskToOutput;
	const bool bIsGPU = (ExecutionDevice != ENNEExecutionDevice::CPU);
	const int32 StoredInferenceWidth = InferenceWidth;
	const int32 StoredInferenceHeight = InferenceHeight;

	auto RunInferenceAndUpload = [PipelineRef, WeakThis, CapturedPixels, CapturedWidth, CapturedHeight, bShouldApplyMask, StoredInferenceWidth, StoredInferenceHeight]() mutable
	{
		// RunSync: the actual NNE model inference
		TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance = PipelineRef->GetModelInstance();
		if (ModelInstance.IsValid())
		{
			if (ModelInstance->RunSync(
				PipelineRef->GetInputBindings(),
				PipelineRef->GetOutputBindings()) != UE::NNE::EResultStatus::Ok)
			{
				UE_LOG(LogTemp, Error, TEXT("NGSVMCompositingPass: NNE inference failed."));
			}
		}

		// Postprocess: read output buffers and convert to mask (at inference resolution)
		TArray<uint8> MaskBuffer;
		PipelineRef->Postprocess(MaskBuffer);

		// Merge onto the FULL-resolution captured color frame. The mask comes back at
		// inference resolution, so upscale it to match CapturedPixels first (same
		// aspect-preserving resize as the color image).
		if (bShouldApplyMask && MaskBuffer.Num() == StoredInferenceWidth * StoredInferenceHeight)
		{
			TArray<uint8> UpscaledMask;
			if (StoredInferenceWidth == CapturedWidth && StoredInferenceHeight == CapturedHeight)
			{
				UpscaledMask = MaskBuffer;
			}
			else
			{
				UNGSVMCompositingPass::ResizeBilinearGrayscale(
					MaskBuffer, StoredInferenceWidth, StoredInferenceHeight,
					UpscaledMask, CapturedWidth, CapturedHeight);
			}

			if (CapturedPixels.Num() == UpscaledMask.Num())
			{
				ParallelFor(CapturedPixels.Num(), [&](int32 i)
				{
					// Straight (non-premultiplied) alpha -- classic Composure's Media
					// Passes blend with the straight-alpha convention, unlike
					// UNGSVMCompositePass's CompositeCore "Default Unlit Alpha Composite"
					// material, which specifically needs premultiplied color.
					CapturedPixels[i].A = UpscaledMask[i];
				});
			}
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, MaskBuffer, CapturedPixels, CapturedWidth, CapturedHeight, bShouldApplyMask, StoredInferenceWidth, StoredInferenceHeight]()
		{
			if (UNGSVMCompositingPass* StrongThis = WeakThis.Get())
			{
				StrongThis->UploadMaskToRT(MaskBuffer, StoredInferenceWidth, StoredInferenceHeight);

				if (bShouldApplyMask)
				{
					StrongThis->UploadKeyedImage(CapturedPixels, CapturedWidth, CapturedHeight);
				}

				StrongThis->bInferenceRunning = false;
				StrongThis->TimeSinceLastInference = 0.0f;
			}
		});
	};

	if (bIsGPU)
	{
		ENQUEUE_RENDER_COMMAND(NGSVMRunGPUInference)(
			[RunInferenceAndUpload](FRHICommandListImmediate&) mutable
			{
				RunInferenceAndUpload();
			});
	}
	else
	{
		// CPU inference MUST run on the Game Thread
		AsyncTask(ENamedThreads::GameThread, [RunInferenceAndUpload]() mutable
		{
			RunInferenceAndUpload();
		});
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Output RT
// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMCompositingPass::EnsureOutputRT()
{
	if (!OutputMaskRT)
	{
		return;
	}

	if (OutputMaskRT->SizeX != InferenceWidth || OutputMaskRT->SizeY != InferenceHeight)
	{
		OutputMaskRT->ResizeTarget(InferenceWidth, InferenceHeight);
	}
}

void UNGSVMCompositingPass::UploadMaskToRT(const TArray<uint8>& MaskData, int32 Width, int32 Height)
{
	if (!OutputMaskRT || MaskData.Num() != Width * Height)
	{
		return;
	}

	// Resize using the resolution the mask was actually computed at (the caller's Width/
	// Height), not EnsureOutputRT()'s live InferenceWidth/InferenceHeight -- those can have
	// changed since this inference started if Auto Compute Resolution re-triggered mid-flight.
	if (OutputMaskRT->SizeX != Width || OutputMaskRT->SizeY != Height)
	{
		OutputMaskRT->ResizeTarget(Width, Height);
	}

	FTextureRenderTargetResource* RTResource = OutputMaskRT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return;
	}

	const int32 W = Width;
	const int32 H = Height;
	FUpdateTextureRegion2D Region(0, 0, 0, 0, W, H);
	TArray<uint8> PixelBuffer = MaskData; // copy for lambda capture

	ENQUEUE_RENDER_COMMAND(NGSVMUploadMask)(
		[RTResource, Region, PixelBuffer](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef DestRHI = RTResource->GetTextureRHI();
			if (DestRHI.IsValid())
			{
				GDynamicRHI->RHIUpdateTexture2D(
					RHICmdList, DestRHI, 0, Region, Region.Width, PixelBuffer.GetData());
			}
		});
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyed (masked) output
// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMCompositingPass::UploadKeyedImage(const TArray<FColor>& KeyedData, int32 Width, int32 Height)
{
	if (KeyedData.Num() != Width * Height)
	{
		return;
	}

	if (!KeyedTexture)
	{
		// Must be a UTextureRenderTarget2D (not UTexture2D::CreateTransient) so downstream
		// Composure passes that Cast<UTextureRenderTarget2D> their Input can consume it.
		KeyedTexture = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
		KeyedTexture->RenderTargetFormat = RTF_RGBA8;
		KeyedTexture->ClearColor = FLinearColor::Transparent;
		KeyedTexture->bAutoGenerateMips = false;
		KeyedTexture->InitAutoFormat(Width, Height);
		KeyedTexture->UpdateResourceImmediate(true);
	}
	else if (KeyedTexture->SizeX != Width || KeyedTexture->SizeY != Height)
	{
		KeyedTexture->ResizeTarget(Width, Height);
	}

	FTextureRenderTargetResource* RTResource = KeyedTexture->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return;
	}

	FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
	TArray<FColor> PixelBuffer = KeyedData; // copy for lambda capture

	ENQUEUE_RENDER_COMMAND(NGSVMUpdateKeyedTexture)(
		[RTResource, Region, PixelBuffer](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef DestRHI = RTResource->GetTextureRHI();
			if (DestRHI.IsValid())
			{
				GDynamicRHI->RHIUpdateTexture2D(
					RHICmdList, DestRHI, 0, Region,
					Region.Width * sizeof(FColor), (const uint8*)PixelBuffer.GetData());
			}
		});
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU bilinear resize
// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMCompositingPass::ResizeBilinear(
	const TArray<FColor>& Src, int32 SrcW, int32 SrcH,
	TArray<FColor>& Out, int32 DstW, int32 DstH)
{
	// Scale uniformly using height as the reference axis, then center-crop (or, if the
	// dest is relatively wider than the source, pad) along width. This keeps the source
	// aspect ratio intact -- independently scaling X and Y (a naive stretch) distorts
	// proportions, which both degrades AI matting quality and produces visible seams.
	// Used symmetrically in both directions (native -> inference and back).
	Out.SetNum(DstW * DstH);

	const float Scale = (float)DstH / (float)SrcH;
	const float SrcCropX = (SrcW - DstW / Scale) * 0.5f;

	// Rows are independent (each writes only its own slice of Out), so distribute them
	// across worker threads instead of running the loop single-threaded.
	ParallelFor(DstH, [&](int32 y)
	{
		const float SrcYf = (y + 0.5f) / Scale - 0.5f;
		const int32 y0 = FMath::FloorToInt(SrcYf);
		const float dy = SrcYf - y0;

		for (int32 x = 0; x < DstW; ++x)
		{
			const float SrcXf = SrcCropX + (x + 0.5f) / Scale - 0.5f;

			// Clamping below extends the nearest edge pixel into the margin cropped away
			// by the other resize direction, instead of leaving a hard-edged gap.
			const int32 x0 = FMath::Clamp(FMath::FloorToInt(SrcXf), 0, SrcW - 1);
			const int32 x1 = FMath::Clamp(x0 + 1, 0, SrcW - 1);
			const int32 cy0 = FMath::Clamp(y0, 0, SrcH - 1);
			const int32 cy1 = FMath::Clamp(y0 + 1, 0, SrcH - 1);
			const float dx = FMath::Clamp(SrcXf - x0, 0.0f, 1.0f);
			const float cdy = FMath::Clamp(dy, 0.0f, 1.0f);

			const FColor c00 = Src[cy0 * SrcW + x0];
			const FColor c10 = Src[cy0 * SrcW + x1];
			const FColor c01 = Src[cy1 * SrcW + x0];
			const FColor c11 = Src[cy1 * SrcW + x1];

			FColor Color;
			Color.R = (uint8)FMath::Lerp(FMath::Lerp((float)c00.R, (float)c10.R, dx), FMath::Lerp((float)c01.R, (float)c11.R, dx), cdy);
			Color.G = (uint8)FMath::Lerp(FMath::Lerp((float)c00.G, (float)c10.G, dx), FMath::Lerp((float)c01.G, (float)c11.G, dx), cdy);
			Color.B = (uint8)FMath::Lerp(FMath::Lerp((float)c00.B, (float)c10.B, dx), FMath::Lerp((float)c01.B, (float)c11.B, dx), cdy);
			Color.A = (uint8)FMath::Lerp(FMath::Lerp((float)c00.A, (float)c10.A, dx), FMath::Lerp((float)c01.A, (float)c11.A, dx), cdy);
			Out[y * DstW + x] = Color;
		}
	});
}

void UNGSVMCompositingPass::ResizeBilinearGrayscale(
	const TArray<uint8>& Src, int32 SrcW, int32 SrcH,
	TArray<uint8>& Out, int32 DstW, int32 DstH)
{
	// Same scale-by-height, crop/pad-width approach as ResizeBilinear, for a single channel.
	Out.SetNum(DstW * DstH);

	const float Scale = (float)DstH / (float)SrcH;
	const float SrcCropX = (SrcW - DstW / Scale) * 0.5f;

	ParallelFor(DstH, [&](int32 y)
	{
		const float SrcYf = (y + 0.5f) / Scale - 0.5f;
		const int32 y0 = FMath::FloorToInt(SrcYf);
		const float dy = SrcYf - y0;

		for (int32 x = 0; x < DstW; ++x)
		{
			const float SrcXf = SrcCropX + (x + 0.5f) / Scale - 0.5f;

			const int32 x0 = FMath::Clamp(FMath::FloorToInt(SrcXf), 0, SrcW - 1);
			const int32 x1 = FMath::Clamp(x0 + 1, 0, SrcW - 1);
			const int32 cy0 = FMath::Clamp(y0, 0, SrcH - 1);
			const int32 cy1 = FMath::Clamp(y0 + 1, 0, SrcH - 1);
			const float dx = FMath::Clamp(SrcXf - x0, 0.0f, 1.0f);
			const float cdy = FMath::Clamp(dy, 0.0f, 1.0f);

			const uint8 c00 = Src[cy0 * SrcW + x0];
			const uint8 c10 = Src[cy0 * SrcW + x1];
			const uint8 c01 = Src[cy1 * SrcW + x0];
			const uint8 c11 = Src[cy1 * SrcW + x1];

			Out[y * DstW + x] = (uint8)FMath::Lerp(FMath::Lerp((float)c00, (float)c10, dx), FMath::Lerp((float)c01, (float)c11, dx), cdy);
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMCompositingPass::BeginDestroy()
{
	// Flush render thread before releasing the readback to avoid use-after-free
	if (bReadbackPending && AsyncReadback)
	{
		FlushRenderingCommands();
	}

	AsyncReadback.Reset();
	Pipeline.Reset();

	Super::BeginDestroy();
}
