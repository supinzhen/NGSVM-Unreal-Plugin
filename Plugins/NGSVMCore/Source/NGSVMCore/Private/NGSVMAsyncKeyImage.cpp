// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMAsyncKeyImage.h"
#include "NGSVMFunctionLibrary.h"
#include "NGSVMModelLoader.h"
#include "NGSVMPipeline.h"
#include "NGSVMRVMPipeline.h"
#include "NGSVMModnetPipeline.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

UNGSVMAsyncKeyImage* UNGSVMAsyncKeyImage::NGSVM_KeyImageAsync(const UObject* WorldContextObject, UTexture2D* OriginalTexture, ENGSVMModelType ModelType, ENNEExecutionDevice Device, ENGSVMResolutionScale Scale)
{
    UNGSVMAsyncKeyImage* Action = NewObject<UNGSVMAsyncKeyImage>();
    Action->OriginalTexture = OriginalTexture;
    Action->ModelType = ModelType;
    Action->ExecutionDevice = Device;
    Action->ResolutionScale = Scale;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UNGSVMAsyncKeyImage::Fail(const TCHAR* Reason)
{
    if (PollTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(PollTickerHandle);
        PollTickerHandle.Reset();
    }
    AsyncReadback.Reset();
    ModelLoadHandle.Reset();

    UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImageAsync: %s"), Reason);
    OnFailure.Broadcast(OriginalTexture, nullptr);
    SetReadyToDestroy();
}

void UNGSVMAsyncKeyImage::Succeed(UTexture2D* ResultTexture, UTexture2D* AlphaMatte)
{
    LogStep(DebugStartTime, TEXT("Succeed() - broadcasting OnSuccess"));
    OnSuccess.Broadcast(ResultTexture, AlphaMatte);
    SetReadyToDestroy();
}

// Diagnostic helper (temporary) -- logs elapsed time since Activate() and which thread is
// currently running, so a reported stall can be pinned to a specific step/thread. Disabled for
// now that the async pipeline is confirmed working; all call sites are left in place so this can
// be re-enabled with a single-line change if a stall needs diagnosing again.
void UNGSVMAsyncKeyImage::LogStep(double StartTime, const TCHAR* Step)
{
    // const TCHAR* ThreadName = IsInGameThread() ? TEXT("Game") : (IsInRenderingThread() ? TEXT("Render") : TEXT("Other"));
    // UE_LOG(LogTemp, Warning, TEXT("[NGSVMAsyncKeyImage] t=%.3fs thread=%s : %s"), FPlatformTime::Seconds() - StartTime, ThreadName, Step);
}

void UNGSVMAsyncKeyImage::Activate()
{
    DebugStartTime = FPlatformTime::Seconds();
    LogStep(DebugStartTime, TEXT("Activate() begin"));

    if (!OriginalTexture || !OriginalTexture->GetResource() || !OriginalTexture->GetResource()->TextureRHI.IsValid())
    {
        Fail(TEXT("OriginalTexture is null or its GPU resource isn't loaded."));
        return;
    }

    PendingSrcWidth = OriginalTexture->GetSizeX();
    PendingSrcHeight = OriginalTexture->GetSizeY();
    if (PendingSrcWidth <= 0 || PendingSrcHeight <= 0)
    {
        Fail(TEXT("OriginalTexture has an invalid size."));
        return;
    }

    BeginAsyncReadback();
    LogStep(DebugStartTime, TEXT("Activate() end (returning to caller)"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Async, non-blocking readback of OriginalTexture's native-resolution pixels.
// Same technique as the per-frame NGSVM Composure passes: blit into a known PF_B8G8R8A8 texture,
// AddEnqueueCopyPass into a FRHIGPUTextureReadback, then poll IsReady() once per frame via
// FTSTicker (no FlushRenderingCommands -- that would block the Game Thread, defeating the whole
// point of this async action existing). Lock()/Unlock() only ever run inside a render command,
// never synchronously on the Game Thread -- calling them off the Render Thread crashes
// (IsInRenderingThread() assertion in RHICommandList.h), a lesson learned the hard way from the
// Legacy Composure pass.
// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMAsyncKeyImage::BeginAsyncReadback()
{
    LogStep(DebugStartTime, TEXT("BeginAsyncReadback() begin"));

    AsyncReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("NGSVMKeyImageReadback"));

    FTextureRHIRef SrcTex = OriginalTexture->GetResource()->TextureRHI;
    const bool bSRGB = OriginalTexture->SRGB;
    const int32 Width = PendingSrcWidth;
    const int32 Height = PendingSrcHeight;
    FRHIGPUTextureReadback* ReadbackPtr = AsyncReadback.Get();
    const double StartTime = DebugStartTime;

    ENQUEUE_RENDER_COMMAND(NGSVMKeyImageAsyncEnqueueReadback)(
        [SrcTex, ReadbackPtr, Width, Height, bSRGB, StartTime](FRHICommandListImmediate& RHICmdList)
        {
            LogStep(StartTime, TEXT("  [RenderCmd] EnqueueReadback command executing"));

            FRDGBuilder GraphBuilder(RHICmdList);
            FRDGTextureRef SrcRDG = RegisterExternalTexture(GraphBuilder, SrcTex, TEXT("NGSVMKeyImageAsyncSource"));

            // Match the source's sRGB-ness on the temp texture to avoid a gamma mismatch during
            // the blit (sampling an sRGB source into a non-sRGB destination, or vice versa,
            // silently shifts every color by one gamma step).
            FRDGTextureDesc TempDesc = FRDGTextureDesc::Create2D(
                FIntPoint(Width, Height),
                PF_B8G8R8A8,
                FClearValueBinding::None,
                TexCreate_RenderTargetable | TexCreate_ShaderResource | (bSRGB ? TexCreate_SRGB : TexCreate_None));
            FRDGTextureRef TempTexture = GraphBuilder.CreateTexture(TempDesc, TEXT("NGSVMKeyImageAsyncTemp"));

            FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            FRDGDrawTextureInfo DrawInfo;
            AddDrawTexturePass(GraphBuilder, ShaderMap, SrcRDG, TempTexture, DrawInfo);

            AddEnqueueCopyPass(GraphBuilder, ReadbackPtr, TempTexture);

            GraphBuilder.Execute();

            LogStep(StartTime, TEXT("  [RenderCmd] EnqueueReadback command done"));
        });

    TWeakObjectPtr<UNGSVMAsyncKeyImage> WeakThis(this);
    PollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [WeakThis](float DeltaTime) -> bool
        {
            UNGSVMAsyncKeyImage* StrongThis = WeakThis.Get();
            if (!StrongThis)
            {
                return false; // action already gone -- stop polling
            }
            return StrongThis->PollReadback(DeltaTime);
        }));

    LogStep(DebugStartTime, TEXT("BeginAsyncReadback() end (render command + ticker queued, not waited on)"));
}

bool UNGSVMAsyncKeyImage::PollReadback(float DeltaTime)
{
    if (!AsyncReadback || !AsyncReadback->IsReady())
    {
        return true; // not ready yet -- keep polling next frame
    }

    LogStep(DebugStartTime, TEXT("PollReadback: readback IsReady() -- locking"));

    FRHIGPUTextureReadback* ReadbackPtr = AsyncReadback.Get();
    const int32 Width = PendingSrcWidth;
    const int32 Height = PendingSrcHeight;
    TWeakObjectPtr<UNGSVMAsyncKeyImage> WeakThis(this);
    const double StartTime = DebugStartTime;

    ENQUEUE_RENDER_COMMAND(NGSVMKeyImageAsyncLockReadback)(
        [ReadbackPtr, Width, Height, WeakThis, StartTime](FRHICommandListImmediate&)
        {
            LogStep(StartTime, TEXT("  [RenderCmd] LockReadback command executing"));

            int32 RowPitchInPixels = 0;
            void* RawData = ReadbackPtr->Lock(RowPitchInPixels);

            TArray<FColor> SrcPixels;
            if (RawData)
            {
                SrcPixels.SetNum(Width * Height);
                const FColor* SrcRow = static_cast<const FColor*>(RawData);
                for (int32 Row = 0; Row < Height; ++Row)
                {
                    FMemory::Memcpy(
                        SrcPixels.GetData() + Row * Width,
                        SrcRow + Row * RowPitchInPixels,
                        Width * sizeof(FColor));
                }
            }
            ReadbackPtr->Unlock();

            LogStep(StartTime, TEXT("  [RenderCmd] LockReadback command done"));

            AsyncTask(ENamedThreads::GameThread, [WeakThis, SrcPixels = MoveTemp(SrcPixels), Width, Height, StartTime]() mutable
            {
                UNGSVMAsyncKeyImage* StrongThis = WeakThis.Get();
                if (!StrongThis)
                {
                    return;
                }
                LogStep(StartTime, TEXT("Back on Game Thread with native pixels"));
                if (SrcPixels.Num() != Width * Height)
                {
                    StrongThis->Fail(TEXT("failed to read source texture pixels."));
                    return;
                }
                StrongThis->AsyncReadback.Reset();
                StrongThis->ContinueWithPixels(MoveTemp(SrcPixels), Width, Height);
            });
        });

    PollTickerHandle.Reset(); // returning false below unregisters us; just drop our own copy
    return false; // stop polling -- readback consumed above
}

// ─────────────────────────────────────────────────────────────────────────────
// Model load, inference, and upload -- runs once native-resolution pixels are available.
// ─────────────────────────────────────────────────────────────────────────────

void UNGSVMAsyncKeyImage::ContinueWithPixels(TArray<FColor>&& SrcPixels, int32 SrcWidth, int32 SrcHeight)
{
    LogStep(DebugStartTime, TEXT("ContinueWithPixels() begin"));

    PendingPixels = MoveTemp(SrcPixels);
    PendingSrcWidth = SrcWidth;
    PendingSrcHeight = SrcHeight;

    // FNGSVMModelLoader::LoadModelData() (called from RunPipelineWithLoadedModel() below) loads
    // synchronously -- LoadSynchronous() blocks the Game Thread on disk I/O the first time an
    // asset is loaded (confirmed via LogStreaming's FlushAsyncLoading in testing). Pre-warm it
    // asynchronously first so that call resolves against an already-cached asset instead.
    TSoftObjectPtr<UNNEModelData> ModelPtr = FNGSVMModelLoader::GetModelSoftPath(ModelType);
    if (ModelPtr.IsNull())
    {
        Fail(TEXT("model path not configured in Project Settings."));
        return;
    }

    if (ModelPtr.IsValid())
    {
        // Already resident in memory (e.g. loaded earlier this session) -- nothing to wait on.
        LogStep(DebugStartTime, TEXT("ContinueWithPixels: model already resident, skipping async preload"));
        RunPipelineWithLoadedModel();
        return;
    }

    LogStep(DebugStartTime, TEXT("ContinueWithPixels: model not resident -- starting RequestAsyncLoad"));
    TWeakObjectPtr<UNGSVMAsyncKeyImage> WeakThis(this);
    ModelLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
        ModelPtr.ToSoftObjectPath(),
        FStreamableDelegate::CreateLambda([WeakThis]()
        {
            if (UNGSVMAsyncKeyImage* StrongThis = WeakThis.Get())
            {
                LogStep(StrongThis->DebugStartTime, TEXT("RequestAsyncLoad completion callback firing"));
                StrongThis->RunPipelineWithLoadedModel();
            }
        }));
    LogStep(DebugStartTime, TEXT("ContinueWithPixels() end (RequestAsyncLoad queued, not waited on)"));
}

void UNGSVMAsyncKeyImage::RunPipelineWithLoadedModel()
{
    LogStep(DebugStartTime, TEXT("RunPipelineWithLoadedModel() begin"));
    ModelLoadHandle.Reset();

    TArray<FColor> SrcPixels = MoveTemp(PendingPixels);
    const int32 SrcWidth = PendingSrcWidth;
    const int32 SrcHeight = PendingSrcHeight;

    // 1. Load model data (near-instant now that it's pre-warmed above) and create the NNE model
    // instance on the requested device.
    LogStep(DebugStartTime, TEXT("  before LoadModelData()"));
    UNNEModelData* ModelData = FNGSVMModelLoader::LoadModelData(ModelType);
    LogStep(DebugStartTime, TEXT("  after LoadModelData()"));
    if (!ModelData)
    {
        Fail(TEXT("failed to load model data."));
        return;
    }

    LogStep(DebugStartTime, TEXT("  before CreateModelInstance()"));
    TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance = FNGSVMModelLoader::CreateModelInstance(ModelData, ExecutionDevice);
    LogStep(DebugStartTime, TEXT("  after CreateModelInstance()"));
    if (!ModelInstance.IsValid())
    {
        Fail(TEXT("failed to create NNE model instance."));
        return;
    }

    // 2. Compute the inference resolution (same 32-aligned rounding used throughout NGSVM).
    const float ScaleFactor = UNGSVMFunctionLibrary::GetResolutionScaleFactor(ResolutionScale);
    const int32 InferenceWidth = FMath::Clamp((FMath::RoundToInt(SrcWidth * ScaleFactor) + 31) & ~31, 64, 4096);
    const int32 InferenceHeight = FMath::Clamp((FMath::RoundToInt(SrcHeight * ScaleFactor) + 31) & ~31, 64, 4096);

    // 3. Instantiate the correct pipeline based on model type name, initialized directly at the
    // resolution it will actually run inference at (must match what Preprocess() is fed below).
    LogStep(DebugStartTime, TEXT("  before Pipeline->Init()"));
    TSharedPtr<INGSVMPipeline> Pipeline;
    const FString EnumName = UEnum::GetValueAsString(ModelType).ToLower();
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
        TSharedPtr<FNGSVMModnetPipeline> ModnetPipeline = MakeShared<FNGSVMModnetPipeline>();
        if (ModnetPipeline->Init(ModelInstance.ToSharedRef(), InferenceWidth, InferenceHeight))
        {
            Pipeline = ModnetPipeline;
        }
    }

    LogStep(DebugStartTime, TEXT("  after Pipeline->Init()"));
    if (!Pipeline.IsValid())
    {
        Fail(TEXT("pipeline Init() failed."));
        return;
    }

    // 4. Resize down to inference resolution (aspect-preserving) if needed, then preprocess.
    TArray<FColor> InferencePixels;
    if (SrcWidth == InferenceWidth && SrcHeight == InferenceHeight)
    {
        InferencePixels = SrcPixels;
    }
    else
    {
        UNGSVMFunctionLibrary::ResizeBilinear(SrcPixels, SrcWidth, SrcHeight, InferencePixels, InferenceWidth, InferenceHeight);
    }

    if (!Pipeline->Preprocess(InferencePixels))
    {
        Fail(TEXT("Preprocess() failed."));
        return;
    }

    // 5. Run inference off the calling stack -- this is the actual slow part this async version
    // exists to not block on. GPU (DirectML/CUDA) execution must happen on the Render Thread
    // (same constraint as the NGSVM Composure passes); CPU execution runs on a queued Game
    // Thread task (CPU inference itself must run on the Game Thread in this codebase).
    const bool bIsGPU = (ExecutionDevice != ENNEExecutionDevice::CPU);
    const int32 CapturedWidth = SrcWidth;
    const int32 CapturedHeight = SrcHeight;
    TWeakObjectPtr<UNGSVMAsyncKeyImage> WeakThis(this);
    TArray<FColor> CapturedPixels = MoveTemp(SrcPixels);
    const double StartTime = DebugStartTime;

    auto RunInferenceAndFinish = [ModelInstance, Pipeline, WeakThis, CapturedPixels = MoveTemp(CapturedPixels), CapturedWidth, CapturedHeight, InferenceWidth, InferenceHeight, StartTime]() mutable
    {
        LogStep(StartTime, TEXT("  RunInferenceAndFinish: before RunSync()"));
        UE::NNE::EResultStatus RunResult = ModelInstance->RunSync(Pipeline->GetInputBindings(), Pipeline->GetOutputBindings());
        LogStep(StartTime, TEXT("  RunInferenceAndFinish: after RunSync()"));

        TArray<uint8> MaskBuffer;
        const bool bPostprocessOk = (RunResult == UE::NNE::EResultStatus::Ok) && Pipeline->Postprocess(MaskBuffer)
            && MaskBuffer.Num() == InferenceWidth * InferenceHeight;
        LogStep(StartTime, TEXT("  RunInferenceAndFinish: after Postprocess()"));

        AsyncTask(ENamedThreads::GameThread, [WeakThis, RunResult, bPostprocessOk, MaskBuffer, CapturedPixels = MoveTemp(CapturedPixels), CapturedWidth, CapturedHeight, InferenceWidth, InferenceHeight]() mutable
        {
            UNGSVMAsyncKeyImage* StrongThis = WeakThis.Get();
            if (!StrongThis)
            {
                return;
            }
            LogStep(StrongThis->DebugStartTime, TEXT("Back on Game Thread after inference"));

            if (RunResult != UE::NNE::EResultStatus::Ok)
            {
                StrongThis->Fail(TEXT("NNE inference failed."));
                return;
            }
            if (!bPostprocessOk)
            {
                StrongThis->Fail(TEXT("Postprocess() failed."));
                return;
            }

            // 6. Upscale the mask back to native resolution (aspect-preserving) and apply as
            // straight (non-premultiplied) alpha onto the FULL native-resolution pixels -- only
            // the mask goes through the (possibly downscaled) AI round trip, so the returned
            // texture's RGB detail always matches the original image's full quality.
            TArray<uint8> UpscaledMask;
            if (InferenceWidth == CapturedWidth && InferenceHeight == CapturedHeight)
            {
                UpscaledMask = MaskBuffer;
            }
            else
            {
                UNGSVMFunctionLibrary::ResizeBilinearGrayscale(MaskBuffer, InferenceWidth, InferenceHeight, UpscaledMask, CapturedWidth, CapturedHeight);
            }

            if (UpscaledMask.Num() != CapturedPixels.Num())
            {
                StrongThis->Fail(TEXT("mask/pixel count mismatch after upscale."));
                return;
            }

            ParallelFor(CapturedPixels.Num(), [&](int32 i)
            {
                CapturedPixels[i].A = UpscaledMask[i];
            });

            // 7. Build and upload the final keyed texture. No flush here -- the whole point of
            // this async action is to not block the Game Thread, and the GPU upload completing a
            // frame later is imperceptible, same as every other NGSVM texture output.
            UTexture2D* ResultTexture = UTexture2D::CreateTransient(CapturedWidth, CapturedHeight, PF_B8G8R8A8);
            if (!ResultTexture)
            {
                StrongThis->Fail(TEXT("failed to create result texture."));
                return;
            }

            ResultTexture->SRGB = StrongThis->OriginalTexture ? StrongThis->OriginalTexture->SRGB : true;
            ResultTexture->UpdateResource();

            FTextureResource* DestResource = ResultTexture->GetResource();
            if (!DestResource)
            {
                StrongThis->Fail(TEXT("result texture resource not ready."));
                return;
            }

            // Don't fetch DestResource->TextureRHI here -- UpdateResource() only enqueues the
            // RHI resource creation on the Render Thread, it doesn't wait for it, so TextureRHI
            // isn't populated yet at this point on the Game Thread. Read it from inside the
            // render command instead: the render thread processes its command queue in order, so
            // by the time this upload command runs, the resource-creation command
            // UpdateResource() enqueued earlier is guaranteed to have already completed.
            FUpdateTextureRegion2D Region(0, 0, 0, 0, CapturedWidth, CapturedHeight);
            ENQUEUE_RENDER_COMMAND(NGSVMKeyImageAsyncUpload)(
                [DestResource, Region, Pixels = MoveTemp(CapturedPixels)](FRHICommandListImmediate& RHICmdList)
                {
                    FTextureRHIRef DestRHI = DestResource->TextureRHI;
                    if (DestRHI.IsValid())
                    {
                        GDynamicRHI->RHIUpdateTexture2D(RHICmdList, DestRHI, 0, Region, Region.Width * sizeof(FColor), (const uint8*)Pixels.GetData());
                    }
                });

            // 8. Build and upload the standalone alpha matte texture (same UpscaledMask already
            // computed above), same no-flush contract as the keyed texture.
            UTexture2D* AlphaMatteTexture = UNGSVMFunctionLibrary::CreateGrayscaleTexture(CapturedWidth, CapturedHeight);
            FTextureResource* AlphaDestResource = AlphaMatteTexture ? AlphaMatteTexture->GetResource() : nullptr;
            if (AlphaDestResource)
            {
                FUpdateTextureRegion2D AlphaRegion(0, 0, 0, 0, CapturedWidth, CapturedHeight);
                ENQUEUE_RENDER_COMMAND(NGSVMKeyImageAsyncAlphaUpload)(
                    [AlphaDestResource, AlphaRegion, MaskPixels = MoveTemp(UpscaledMask)](FRHICommandListImmediate& RHICmdList)
                    {
                        FTextureRHIRef AlphaDestRHI = AlphaDestResource->TextureRHI;
                        if (AlphaDestRHI.IsValid())
                        {
                            GDynamicRHI->RHIUpdateTexture2D(RHICmdList, AlphaDestRHI, 0, AlphaRegion, AlphaRegion.Width, MaskPixels.GetData());
                        }
                    });
            }
            else
            {
                AlphaMatteTexture = nullptr;
            }

            StrongThis->Succeed(ResultTexture, AlphaMatteTexture);
        });
    };

    LogStep(DebugStartTime, bIsGPU ? TEXT("  dispatching RunInferenceAndFinish via ENQUEUE_RENDER_COMMAND (GPU)") : TEXT("  dispatching RunInferenceAndFinish via AsyncTask(GameThread) (CPU)"));

    if (bIsGPU)
    {
        ENQUEUE_RENDER_COMMAND(NGSVMKeyImageAsyncRunGPUInference)(
            [RunInferenceAndFinish](FRHICommandListImmediate&) mutable
            {
                RunInferenceAndFinish();
            });
    }
    else
    {
        // CPU inference MUST run on the Game Thread (same constraint as the NGSVM Composure
        // passes' RunSync calls) -- so unlike a normal background offload, this doesn't move the
        // actual inference work off Game Thread time. What it still buys us: this function
        // returns immediately instead of running RunSync() inline on the calling stack, so the
        // engine gets a chance to pump other Game Thread work (and a frame can complete) before
        // this queued task actually runs, rather than the call site itself stalling synchronously.
        AsyncTask(ENamedThreads::GameThread, [RunInferenceAndFinish]() mutable
        {
            RunInferenceAndFinish();
        });
    }

    LogStep(DebugStartTime, TEXT("RunPipelineWithLoadedModel() end (inference dispatched, not waited on)"));
}
