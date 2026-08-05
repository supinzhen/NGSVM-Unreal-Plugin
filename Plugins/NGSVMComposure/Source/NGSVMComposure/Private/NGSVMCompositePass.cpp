// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMCompositePass.h"
#include "NGSVMModelLoader.h"
#include "NGSVMPipeline.h"
#include "NGSVMRVMPipeline.h"
#include "NGSVMModnetPipeline.h"

#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/App.h"
#include "GlobalShader.h"
#include "Passes/CompositeCorePassProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"

// Converts a ResolutionScale enum value to its multiplier (1.0, 0.5, 0.25, 0.125).
static float GetResolutionScaleFactor(ENGSVMResolutionScale InScale)
{
	switch (InScale)
	{
	case ENGSVMResolutionScale::Half:    return 0.5f;
	case ENGSVMResolutionScale::Quarter: return 0.25f;
	case ENGSVMResolutionScale::Eighth:  return 0.125f;
	default:                             return 1.0f;
	}
}

// -----------------------------------------------------------------------------
// Proxy for rendering on the Render Thread
// -----------------------------------------------------------------------------
class FCompositePassNGSVMProxy : public FCompositeCorePassProxy
{
public:
	IMPLEMENT_COMPOSITE_PASS(FCompositePassNGSVMProxy);
	using FCompositeCorePassProxy::FCompositeCorePassProxy;

	const UNGSVMCompositePass *ParentPass = nullptr;

	virtual UE::CompositeCore::FPassTexture Add(
		FRDGBuilder &GraphBuilder,
		const FSceneView &InView,
		const UE::CompositeCore::FPassInputArray &Inputs,
		const UE::CompositeCore::FPassContext &PassContext) const override
	{
		check(ValidateInputs(Inputs));

		if (ParentPass && Inputs.Num() > 0)
		{
			FRDGTextureRef InputTexture = Inputs[0].Texture.Texture;
			if (InputTexture)
			{
				int32 SrcW = InputTexture->Desc.Extent.X;
				int32 SrcH = InputTexture->Desc.Extent.Y;

				if (SrcW < 64 || SrcH < 64)
				{
					// CompositeCore supplies a 2x2 placeholder while a plate/source is not
					// ready. It is not a valid video frame and must not enter the inference
					// or output-override path during editor startup.
					return UE::CompositeCore::FPassTexture{Inputs[0].Texture, Inputs[0].Metadata};
				}

				// Store original input resolution for later use when uploading keyed image
				UNGSVMCompositePass *MutablePass = const_cast<UNGSVMCompositePass *>(ParentPass);
				MutablePass->OriginalInputWidth = SrcW;
				MutablePass->OriginalInputHeight = SrcH;

				if (ParentPass->bAutoComputeResolution)
				{
					// Round width and height independently to a multiple of 32. This can cost a
					// small (sub-1%, generally imperceptible) center crop in ResizeBilinear's
					// height-driven scale when the two axes round by different amounts -- but it
					// never inflates the inference canvas past what's needed, which matters when
					// the source is already 32-aligned (e.g. 1920x1088): deriving width from
					// height's rounding slack would otherwise round it up unnecessarily (e.g. to
					// 1952) and burn extra compute for no visual benefit.
					const float Scale = GetResolutionScaleFactor(ParentPass->ResolutionScale);
					int32 NewW = FMath::Clamp((FMath::RoundToInt(SrcW * Scale) + 31) & ~31, 64, 4096);
					int32 NewH = FMath::Clamp((FMath::RoundToInt(SrcH * Scale) + 31) & ~31, 64, 4096);

					if (ParentPass->InferenceWidth != NewW || ParentPass->InferenceHeight != NewH)
					{
						TWeakObjectPtr<UNGSVMCompositePass> WeakPass(MutablePass);

						// Safely queue a reinit on the Game Thread
						AsyncTask(ENamedThreads::GameThread, [WeakPass, NewW, NewH]()
								  {
							if (UNGSVMCompositePass* Pass = WeakPass.Get())
							{
								if (Pass->bAutoComputeResolution && (Pass->InferenceWidth != NewW || Pass->InferenceHeight != NewH))
								{
									Pass->InferenceWidth = NewW;
									Pass->InferenceHeight = NewH;
									Pass->ReinitializePipeline();
								}
							} });
					}
				}

				ParentPass->EnqueueReadback_RenderThread(GraphBuilder, InputTexture);

				FScreenPassTexture SourceForOutput = Inputs[0].Texture;
				const bool bKeyedTextureValid = ParentPass->KeyedTextureState.IsValid() && ParentPass->KeyedTextureState->TextureRHI.IsValid();

				// TextureRHI is only written/read on the Render Thread. The shared state
				// itself remains valid while render commands are in flight.
				if (ParentPass->bApplyMaskToOutput && bKeyedTextureValid)
				{
					SourceForOutput.Texture = RegisterExternalTexture(GraphBuilder, ParentPass->KeyedTextureState->TextureRHI, TEXT("KeyedVideo"));
				}

				// Match the built-in Color Keyer: the final pre-processing pass receives
				// an externally supplied target. Returning an external texture directly
				// bypasses that target, so Composure never receives the keyed frame.
				FScreenPassRenderTarget Output = Inputs.OverrideOutput;
				if (!Output.IsValid())
				{
					Output = CreateOutputRenderTarget(GraphBuilder, InView, PassContext.OutputViewRect, InputTexture->Desc, TEXT("NGSVMCompositePass"));
				}

				FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(InView.GetFeatureLevel());
				FRDGDrawTextureInfo DrawInfo;
				AddDrawTexturePass(GraphBuilder, ShaderMap, SourceForOutput.Texture, Output.Texture, DrawInfo);

				return UE::CompositeCore::FPassTexture{MoveTemp(Output), Inputs[0].Metadata};
			}

			// Fallback
			return UE::CompositeCore::FPassTexture{};
		}

		return UE::CompositeCore::FPassTexture{};
	}
};

	void UNGSVMCompositePass::UploadKeyedImage(const TArray<FColor> &KeyedData, int32 Width, int32 Height)
	{
		if (KeyedData.Num() != Width * Height)
		{
			UE_LOG(LogTemp, Error, TEXT("[UploadKeyedImage] Size mismatch: %d != %d*%d"), KeyedData.Num(), Width, Height);
			return;
		}

		if (!KeyedTexture || KeyedTexture->GetSizeX() != Width || KeyedTexture->GetSizeY() != Height)
		{
			// Reset the RT-side ref when we recreate the texture
			TSharedPtr<FNGSVMKeyedTextureState> TextureState = KeyedTextureState;
			ENQUEUE_RENDER_COMMAND(ResetKeyedRHI)([TextureState](FRHICommandListImmediate &)
											  { TextureState->TextureRHI = nullptr; });

			KeyedTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
			if (KeyedTexture)
			{
				KeyedTexture->UpdateResource();
			}
		}

		// Wait until the resource is initialised on the render thread before uploading
		if (!KeyedTexture || !KeyedTexture->GetResource() || !KeyedTexture->GetResource()->TextureRHI)
		{
			return;
		}

		struct FUploadCtx
		{
			FTextureRHIRef TextureRHI;
			TSharedPtr<FNGSVMKeyedTextureState> TextureState;
			TArray<FColor> Pixels;
			int32 Width;
			int32 Height;
		};

		FUploadCtx *Ctx = new FUploadCtx{
			KeyedTexture->GetResource()->TextureRHI,
			KeyedTextureState,
			KeyedData,
			Width,
			Height};

		ENQUEUE_RENDER_COMMAND(UpdateKeyedTexture)(
			[Ctx](FRHICommandListImmediate &RHICmdList)
			{
				FUpdateTextureRegion2D Region(0, 0, 0, 0, Ctx->Width, Ctx->Height);
				GDynamicRHI->RHIUpdateTexture2D(
					RHICmdList,
					Ctx->TextureRHI,
					0,
					Region,
					Ctx->Width * sizeof(FColor),
					(const uint8 *)Ctx->Pixels.GetData());
				// Now that pixels are uploaded, expose to Proxy::Add() on the same thread
				Ctx->TextureState->TextureRHI = Ctx->TextureRHI;
				delete Ctx;
			});
	}

	// -----------------------------------------------------------------------------

	UNGSVMCompositePass::UNGSVMCompositePass(const FObjectInitializer &ObjectInitializer)
		: Super(ObjectInitializer)
	{
		KeyedTextureState = MakeShared<FNGSVMKeyedTextureState>();
		SetEnabled(false);
	}

	UNGSVMCompositePass::~UNGSVMCompositePass() = default;

	bool UNGSVMCompositePass::InitPipeline()
	{
		if (Pipeline.IsValid())
		{
			return true;
		}

		UNNEModelData *ModelData = FNGSVMModelLoader::LoadModelData(ModelType);
		if (!ModelData)
		{
			UE_LOG(LogTemp, Error, TEXT("NGSVMCompositePass: Failed to load model data."));
			return false;
		}

		TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance =
			FNGSVMModelLoader::CreateModelInstance(ModelData, ExecutionDevice);
		if (!ModelInstance.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("NGSVMCompositePass: Failed to create NNE model instance."));
			return false;
		}

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
			UE_LOG(LogTemp, Error, TEXT("NGSVMCompositePass: Pipeline Init() failed."));
			return false;
		}

		EnsureOutputRT();
		return true;
	}

	void UNGSVMCompositePass::ReinitializePipeline()
	{
		// A GPU readback copy pass recorded via EnqueueReadback_RenderThread() may still be
		// in flight (holding a raw FRHIGPUTextureReadback* into the RDG graph) when this runs
		// on the Game Thread. Flush first so we don't free AsyncReadback out from under it.
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

#if WITH_EDITOR
	void UNGSVMCompositePass::PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent)
	{
		Super::PostEditChangeProperty(PropertyChangedEvent);

		// Re-initialize pipeline when any inference-relevant property changes.
		// This is critical: if InferenceWidth/Height changes, the NNE tensor buffers
		// must be rebuilt to match the new shape, or we'll crash on the next RunSync.
		static const TArray<FName> ResensitiveProps = {
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, InferenceWidth),
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, InferenceHeight),
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, ModelType),
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, ExecutionDevice),
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, bAutoComputeResolution),
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, ResolutionScale),
			GET_MEMBER_NAME_CHECKED(UNGSVMCompositePass, bApplyMaskToOutput),
		};

		FName ChangedProp = PropertyChangedEvent.GetPropertyName();
		if (ResensitiveProps.Contains(ChangedProp))
		{
			ReinitializePipeline();
		}
	}
#endif

	bool UNGSVMCompositePass::GetProxy(
		const UE::CompositeCore::FPassInputDecl &InputDecl,
		FSceneRenderingBulkObjectAllocator &InFrameAllocator,
		FCompositeCorePassProxy *&OutProxy) const
	{
		if (!IsEnabled())
		{
			return false;
		}

		// NOTE: GetProxy() is called on the Render Thread.
		// All readback consumption (AsyncReadback->Lock) happens safely on Game Thread via Tick().

		// Create and return the proxy for this frame's rendering
		FCompositePassNGSVMProxy *Proxy = InFrameAllocator.Create<FCompositePassNGSVMProxy>(UE::CompositeCore::FPassInputDeclArray{InputDecl});
		Proxy->ParentPass = this;

		OutProxy = Proxy;
		return true;
	}

	void UNGSVMCompositePass::Tick(float DeltaTime)
	{
		const bool bNowEnabled = IsEnabled();
		if (bNowEnabled && !bWasEnabledLastTick)
		{
			// Model Type / Execution Device / resolution settings may have changed while
			// disabled (ProcessFrameInternal never runs while disabled, so those changes
			// were never picked up) -- force a reinit now so re-enabling always reflects
			// the current settings.
			ReinitializePipeline();
		}
		bWasEnabledLastTick = bNowEnabled;

		if (!bNowEnabled)
		{
			return;
		}
		ProcessFrameInternal();
	}

	void UNGSVMCompositePass::ProcessFrameInternal()
	{
		if (!Pipeline.IsValid())
		{
			if (!InitPipeline())
			{
				UE_LOG(LogTemp, Error, TEXT("[ProcessFrameInternal] Failed to initialize pipeline"));
				return;
			}
		}

		const float FrameDeltaTime = FApp::GetDeltaTime();
		TimeSinceLastInference += FrameDeltaTime;
		const float TargetInterval = TargetInferenceFPS > 0.0f ? (1.0f / TargetInferenceFPS) : 0.0f;
		const bool bShouldInfer = (TimeSinceLastInference >= TargetInterval) && !bInferenceRunning;

		if (bReadbackPending && AsyncReadback && AsyncReadback->IsReady())
		{
			int32 RowPitchInPixels = 0;
			void *RawData = AsyncReadback->Lock(RowPitchInPixels);

			if (RawData && bShouldInfer)
			{
				TArray<FColor> SrcPixels;
				SrcPixels.SetNum(ReadbackSrcWidth * ReadbackSrcHeight);
				const FColor *SrcRow = static_cast<const FColor *>(RawData);

				for (int32 Row = 0; Row < ReadbackSrcHeight; ++Row)
				{
					FMemory::Memcpy(
						SrcPixels.GetData() + Row * ReadbackSrcWidth,
						SrcRow + Row * RowPitchInPixels,
						ReadbackSrcWidth * sizeof(FColor));
				}
				AsyncReadback->Unlock();

				TArray<FColor> InferencePixels;
				if (ReadbackSrcWidth == InferenceWidth && ReadbackSrcHeight == InferenceHeight)
				{
					InferencePixels = SrcPixels; // copy: SrcPixels (full native resolution) is kept below
				}
				else
				{
					ResizeBilinear(SrcPixels, ReadbackSrcWidth, ReadbackSrcHeight,
								   InferencePixels, InferenceWidth, InferenceHeight);
				}

				// Keep the FULL native-resolution color frame for the final composite. Only
				// InferencePixels (possibly downscaled per ResolutionScale) goes to the AI model --
				// this way ResolutionScale only affects the AI mask computation, never the
				// color/detail quality of the final keyed output. The mask gets upscaled back up
				// to this resolution below, after inference, instead of upscaling the color image.
				TArray<FColor> CapturedPixels = MoveTemp(SrcPixels);
				const int32 CapturedWidth = ReadbackSrcWidth;
				const int32 CapturedHeight = ReadbackSrcHeight;

				if (Pipeline->Preprocess(InferencePixels))
				{
					bInferenceRunning = true;
					TSharedPtr<INGSVMPipeline> PipelineRef = Pipeline;
					TWeakObjectPtr<UNGSVMCompositePass> WeakThis(this);
					bool bShouldApplyMask = bApplyMaskToOutput;
					const bool bIsGPU = (ExecutionDevice != ENNEExecutionDevice::CPU);
					int32 StoredInferenceWidth = InferenceWidth;
					int32 StoredInferenceHeight = InferenceHeight;

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
								UE_LOG(LogTemp, Error, TEXT("NGSVMCompositePass: NNE inference failed."));
							}
						}

						// Postprocess: read output buffers and convert to mask (at inference resolution)
						TArray<uint8> MaskBuffer;
						PipelineRef->Postprocess(MaskBuffer);

						// Merge onto the FULL-resolution captured color frame. The mask comes back at
						// inference resolution, so upscale it to match CapturedPixels first (same
						// aspect-preserving resize as the color image) rather than downscaling the
						// color image down to the mask's resolution.
						if (bShouldApplyMask && MaskBuffer.Num() == StoredInferenceWidth * StoredInferenceHeight)
						{
							TArray<uint8> UpscaledMask;
							if (StoredInferenceWidth == CapturedWidth && StoredInferenceHeight == CapturedHeight)
							{
								UpscaledMask = MaskBuffer;
							}
							else
							{
								UNGSVMCompositePass::ResizeBilinearGrayscale(
									MaskBuffer, StoredInferenceWidth, StoredInferenceHeight,
									UpscaledMask, CapturedWidth, CapturedHeight);
							}

							if (CapturedPixels.Num() == UpscaledMask.Num())
							{
								ParallelFor(CapturedPixels.Num(), [&](int32 i)
								{
									const uint8 Alpha = UpscaledMask[i];

									CapturedPixels[i].A = Alpha;

									// The Composite Mesh's "Default Unlit Alpha Composite" material
									// expects premultiplied alpha (see Engine's
									// ECompositeMeshMaterialType::DefaultUnlitAlphaComposite doc
									// comment). Straight (non-premultiplied) color there reads as an
									// un-attenuated addition on top of the destination, which only
									// shows up where 0 < alpha < 1 (identical to premultiplied at the
									// alpha=0/1 extremes) -- i.e. exactly the bright edge halo.
									CapturedPixels[i].R = (uint8)((int32)CapturedPixels[i].R * Alpha / 255);
									CapturedPixels[i].G = (uint8)((int32)CapturedPixels[i].G * Alpha / 255);
									CapturedPixels[i].B = (uint8)((int32)CapturedPixels[i].B * Alpha / 255);
								});
							}
						}

						AsyncTask(ENamedThreads::GameThread, [WeakThis, MaskBuffer, CapturedPixels, CapturedWidth, CapturedHeight, bShouldApplyMask]()
								  {
							if (UNGSVMCompositePass *StrongThis = WeakThis.Get())
							{
								StrongThis->UploadMaskToRT(MaskBuffer);

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
				}
				else if (AsyncReadback)
				{
					AsyncReadback->Unlock();
				}

				bReadbackPending = false;
			}
		}

		void UNGSVMCompositePass::EnqueueReadback_RenderThread(FRDGBuilder & GraphBuilder, FRDGTexture * InputTexture) const
		{
			if (!InputTexture)
			{
				return;
			}

			UNGSVMCompositePass *MutableThis = const_cast<UNGSVMCompositePass *>(this);

			if (!MutableThis->bReadbackPending)
			{
				if (!MutableThis->AsyncReadback)
				{
					MutableThis->AsyncReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("NGSVMCompositeReadback"));
				}

				// Create a temporary LDR copyable texture at the SOURCE's native resolution
				// (not the inference resolution). AddDrawTexturePass (RenderCore) never actually
				// scales -- it's either a straight hardware CopyTexture (matching formats) or a
				// 1:1 pixel-shader blit (mismatched formats); it has no notion of source/dest
				// having different sizes. Previously TempTexture was created at
				// InferenceWidth/Height expecting AddDrawTexturePass to "stretch to fit", which it
				// never did -- that produced a corrupted/garbage strip whenever InferenceWidth/Height
				// (rounded to a multiple of 32) differed from the source extent. Capturing 1:1 here
				// and doing the actual aspect-preserving resize on the CPU (see ResizeBilinear)
				// avoids that.
				FRDGTextureDesc TempDesc = FRDGTextureDesc::Create2D(
					InputTexture->Desc.Extent,
					PF_B8G8R8A8,
					FClearValueBinding::None,
					TexCreate_RenderTargetable | TexCreate_ShaderResource);

				FRDGTextureRef TempTexture = GraphBuilder.CreateTexture(TempDesc, TEXT("NGSVM_TempReadbackTexture"));

				FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
				FRDGDrawTextureInfo DrawInfo;
				AddDrawTexturePass(
					GraphBuilder,
					ShaderMap,
					InputTexture,
					TempTexture,
					DrawInfo);

				MutableThis->ReadbackSrcWidth = InputTexture->Desc.Extent.X;
				MutableThis->ReadbackSrcHeight = InputTexture->Desc.Extent.Y;

				// Use the built-in UE5 utility to add a readback pass from the TempTexture
				AddEnqueueCopyPass(GraphBuilder, MutableThis->AsyncReadback.Get(), TempTexture);

				MutableThis->bReadbackPending = true;
			}
		}

		void UNGSVMCompositePass::EnsureOutputRT()
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

		void UNGSVMCompositePass::UploadMaskToRT(const TArray<uint8> &MaskData)
		{
			if (!OutputMaskRT || MaskData.Num() == 0)
			{
				return;
			}

			EnsureOutputRT();

			FTextureRenderTargetResource *RTResource = OutputMaskRT->GameThread_GetRenderTargetResource();
			if (!RTResource)
			{
				return;
			}

			const int32 W = InferenceWidth;
			const int32 H = InferenceHeight;
			FUpdateTextureRegion2D Region(0, 0, 0, 0, W, H);
			TArray<uint8> PixelBuffer = MaskData;

			ENQUEUE_RENDER_COMMAND(NGSVMCompositeUploadMask)(
				[RTResource, Region, PixelBuffer](FRHICommandListImmediate &RHICmdList)
				{
					FTextureRHIRef DestRHI = RTResource->GetTextureRHI();
					if (DestRHI.IsValid())
					{
						GDynamicRHI->RHIUpdateTexture2D(
							RHICmdList, DestRHI, 0, Region, Region.Width, PixelBuffer.GetData());
					}
				});
		}

		void UNGSVMCompositePass::ResizeBilinear(
			const TArray<FColor> &Src, int32 SrcW, int32 SrcH,
			TArray<FColor> &Out, int32 DstW, int32 DstH)
		{
			// Scale uniformly using height as the reference axis, then center-crop (or, if the
			// dest is relatively wider than the source, pad with transparent) along width. This
			// keeps the source aspect ratio intact -- independently scaling X and Y (as a naive
			// stretch would) distorts proportions, which both degrades AI matting quality and
			// produces visible seams when the two aspect ratios are only slightly different (e.g.
			// a 1080-tall source against a 1088-tall inference canvas rounded up to a multiple of
			// 32). Used symmetrically in both directions (native -> inference and back), so any
			// margin cropped away on the way in reappears as a transparent margin on the way out
			// rather than smeared/duplicated edge pixels.
			Out.SetNum(DstW * DstH);

			const float Scale = (float)DstH / (float)SrcH;
			// X offset, in source pixels, of the DstW-wide window centered within Src.
			const float SrcCropX = (SrcW - DstW / Scale) * 0.5f;

			// Rows are independent (each writes only its own slice of Out), so distribute them
			// across worker threads instead of running the ~2M-pixel loop single-threaded.
			ParallelFor(DstH, [&](int32 y)
			{
				const float SrcYf = (y + 0.5f) / Scale - 0.5f;
				const int32 y0 = FMath::FloorToInt(SrcYf);
				const float dy = SrcYf - y0;

				for (int32 x = 0; x < DstW; ++x)
				{
					const float SrcXf = SrcCropX + (x + 0.5f) / Scale - 0.5f;

					// Outside the source image -- only possible for the margin cropped away when
					// resizing in the other direction (e.g. the aspect ratio mismatch between a
					// 32-aligned inference canvas and the native 16:9 source). Clamping below
					// naturally extends the nearest edge pixel/alpha here instead of leaving a
					// transparent gap, which would otherwise look like the image being cut off.
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
					// Preserve alpha channel (contains mask data)
					Color.A = (uint8)FMath::Lerp(FMath::Lerp((float)c00.A, (float)c10.A, dx), FMath::Lerp((float)c01.A, (float)c11.A, dx), cdy);
					Out[y * DstW + x] = Color;
				}
			});
		}

		void UNGSVMCompositePass::ResizeBilinearGrayscale(
			const TArray<uint8> &Src, int32 SrcW, int32 SrcH,
			TArray<uint8> &Out, int32 DstW, int32 DstH)
		{
			// Same scale-by-height, crop/pad-width approach as ResizeBilinear, for a single channel.
			// Used to upscale the AI mask (computed at inference resolution) up to the full native
			// resolution before merging it onto the un-downscaled color frame.
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

					// Clamping below extends the nearest edge alpha value into the margin cropped
					// away by the other resize direction, instead of leaving a hard transparent
					// gap that reads as the subject being cut off at the frame edge.
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

		void UNGSVMCompositePass::BeginDestroy()
		{
			if (bReadbackPending && AsyncReadback)
			{
				FlushRenderingCommands();
			}

			AsyncReadback.Reset();
			Pipeline.Reset();

			Super::BeginDestroy();
		}
