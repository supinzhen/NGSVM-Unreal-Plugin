// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMManager.h"
#include "NGSVMModelLoader.h"
#include "NGSVMPipeline.h"
#include "NGSVMRVMPipeline.h"
#include "NGSVMModnetPipeline.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
// Full definition required: GetWorld()'s return type (UWorld) must be complete here for the
// implicit UWorld*->UObject* upcast in DrawMaterialToRenderTarget(GetWorld(), ...) below to
// resolve -- see the identical fix in NGSVMBillboardPawn.cpp for why this only broke under the
// Game (non-Editor) target's unity grouping.
#include "Engine/World.h"
#include "TextureResource.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Kismet/KismetRenderingLibrary.h"

UNGSVMManager::UNGSVMManager()
{
    // Enable ticking for real-time video frame processing
    PrimaryComponentTick.bCanEverTick = true;
}

#if WITH_EDITOR
void UNGSVMManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // Update details panel parameter visibility if the selected model type changes
    FName PropertyName = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
    if (PropertyName == GET_MEMBER_NAME_CHECKED(UNGSVMManager, ModelType))
    {
        UpdateInputParamsVisibility();
    }
}

void UNGSVMManager::PostInitProperties()
{
    Super::PostInitProperties();
    // Initialize parameter visibility for input resolution settings
    UpdateInputParamsVisibility();
}
#endif

void UNGSVMManager::UpdateInputParamsVisibility()
{
    // Input resolution settings are shared by the manager for all supported model types.
    bShowInputParams = true;
}

void UNGSVMManager::BeginPlay()
{
    Super::BeginPlay();

    if (bAutoStart)
    {
        StartNGSVMService();
    }
}

void UNGSVMManager::BeginDestroy()
{
    // A GPU readback copy may still be in flight holding a raw AsyncReadback pointer inside an
    // already-enqueued render command -- flush before freeing it to avoid a use-after-free.
    if (bReadbackPending && AsyncReadback)
    {
        FlushRenderingCommands();
    }

    AsyncReadback.Reset();
    Pipeline.Reset();

    Super::BeginDestroy();
}

void UNGSVMManager::StartNGSVMService()
{
    if (bServiceActive)
    {
        return;
    }

    if (!bIsInitialized)
    {
        InitializeService();
    }

    if (bIsInitialized)
    {
        bServiceActive = true;
    }
}

void UNGSVMManager::StopNGSVMService()
{
    bServiceActive = false;
}

void UNGSVMManager::InitializeService()
{
    if (bIsInitialized)
    {
        return;
    }

    // 1. Load model asset data configured in settings
    UNNEModelData* ModelData = FNGSVMModelLoader::LoadModelData(ModelType);
    if (!ModelData)
    {
        return;
    }

    // 2. Create the executable model instance on the selected device (CPU/GPU)
    TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance = FNGSVMModelLoader::CreateModelInstance(ModelData, ExecutionDevice);
    if (!ModelInstance.IsValid())
    {
        return;
    }

    // 3. Initialize the corresponding pipeline based on the model type
    FString EnumName = UEnum::GetValueAsString(ModelType).ToLower();
    if (EnumName.Contains(TEXT("rvm")))
    {
        TSharedPtr<FNGSVMRVMPipeline> RVMPipeline = MakeShared<FNGSVMRVMPipeline>();
        if (RVMPipeline->Init(ModelInstance.ToSharedRef(), InputWidth, InputHeight))
        {
            Pipeline = RVMPipeline;
        }
    }
    else if (EnumName.Contains(TEXT("modnet")))
    {
        TSharedPtr<FNGSVMModnetPipeline> MODNETPipeline = MakeShared<FNGSVMModnetPipeline>();
        if (MODNETPipeline->Init(ModelInstance.ToSharedRef(), InputWidth, InputHeight))
        {
            Pipeline = MODNETPipeline;
        }
    }

    // 4. Initialize output textures if pipeline setup succeeded
    if (Pipeline.IsValid())
    {
        InitOutputTexture();
        bIsInitialized = true;
    }
}

void UNGSVMManager::ReinitializePipeline()
{
    // Safe to reset even while an inference task is in flight: the background task holds its own
    // TSharedPtr<INGSVMPipeline> copy (captured before dispatch), so clearing this member only
    // drops the Manager's reference -- the pipeline object itself stays alive until that task's
    // copy goes out of scope.
    Pipeline.Reset();
    bIsInitialized = false;
    InitializeService();
}

float UNGSVMManager::GetResolutionScaleFactor(ENGSVMResolutionScale InScale)
{
    switch (InScale)
    {
    case ENGSVMResolutionScale::Half:    return 0.5f;
    case ENGSVMResolutionScale::Quarter: return 0.25f;
    case ENGSVMResolutionScale::Eighth:  return 0.125f;
    default:                             return 1.0f;
    }
}

void UNGSVMManager::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bServiceActive || !Pipeline.IsValid() || !InputRenderTarget)
    {
        return;
    }

    FTextureRenderTargetResource* RTResource = InputRenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        return;
    }

    TimeSinceLastInference += DeltaTime;
    const float TargetInterval = TargetInferenceFPS > 0.0f ? (1.0f / TargetInferenceFPS) : 0.0f;
    const bool bShouldCapture = !bIsRunning && (TimeSinceLastInference >= TargetInterval);

    // ── Step 1: if the previous async readback is ready, consume it ────────────────────────────
    // AsyncReadback->Lock()/Unlock() internally call RHICmdList.MapStagingSurface(), which asserts
    // IsInRenderingThread() -- must only ever happen inside ENQUEUE_RENDER_COMMAND, never directly
    // here on the Game Thread (same constraint as UNGSVMCompositingPass).
    if (bReadbackPending && AsyncReadback && AsyncReadback->IsReady())
    {
        bReadbackPending = false;

        FRHIGPUTextureReadback* ReadbackPtr = AsyncReadback.Get();
        const int32 CopyWidth = ReadbackSrcWidth;
        const int32 CopyHeight = ReadbackSrcHeight;

        if (bShouldCapture)
        {
            TWeakObjectPtr<UNGSVMManager> WeakThis(this);
            ENQUEUE_RENDER_COMMAND(NGSVMManagerLockReadback)(
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
                            if (UNGSVMManager* StrongThis = WeakThis.Get())
                            {
                                StrongThis->ProcessCapturedFrame(MoveTemp(SrcPixels), CopyWidth, CopyHeight);
                            }
                        });
                    }
                });
        }
        else
        {
            // Not time to infer yet -- still need to Lock+Unlock on the render thread to release
            // the staging resource so it can be reused for the next capture.
            ENQUEUE_RENDER_COMMAND(NGSVMManagerDiscardReadback)(
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

    // ── Step 2: enqueue the next async readback (non-blocking) ─────────────────────────────────
    if (!bReadbackPending)
    {
        ProcessInputFrame();
    }
}

void UNGSVMManager::InitOutputTexture()
{
    int32 Width = (OutputWidth > 0) ? OutputWidth : (InputRenderTarget ? InputRenderTarget->SizeX : InputWidth);
    int32 Height = (OutputHeight > 0) ? OutputHeight : (InputRenderTarget ? InputRenderTarget->SizeY : InputHeight);
    Width = FMath::Max(1, Width);
    Height = FMath::Max(1, Height);

    // Always create the transient UTexture2D staging textures
    if (!OutputMaskTexture || OutputMaskTexture->GetSizeX() != Width || OutputMaskTexture->GetSizeY() != Height)
    {
        OutputMaskTexture = UTexture2D::CreateTransient(Width, Height, PF_G8);
        if (OutputMaskTexture)
        {
            OutputMaskTexture->SRGB = false;
            OutputMaskTexture->CompressionSettings = TC_Grayscale;
            OutputMaskTexture->UpdateResource();
        }
    }

    if (!OutputColorTexture || OutputColorTexture->GetSizeX() != Width || OutputColorTexture->GetSizeY() != Height)
    {
        OutputColorTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (OutputColorTexture)
        {
            OutputColorTexture->SRGB = true;
            OutputColorTexture->CompressionSettings = TC_Default;
            OutputColorTexture->UpdateResource();
        }
    }

    // Resize custom Render Targets to match the model resolution if specified
    if (OutputMaskRenderTarget)
    {
        if (OutputMaskRenderTarget->SizeX != Width || OutputMaskRenderTarget->SizeY != Height)
        {
            OutputMaskRenderTarget->ResizeTarget(Width, Height);
        }
    }

    if (OutputColorRenderTarget)
    {
        if (OutputColorRenderTarget->SizeX != Width || OutputColorRenderTarget->SizeY != Height)
        {
            OutputColorRenderTarget->ResizeTarget(Width, Height);
        }
    }
}

void UNGSVMManager::ProcessInputFrame()
{
    // Ensure we have a valid input source render target and an initialized AI pipeline
    if (!InputRenderTarget || !Pipeline.IsValid())
    {
        return;
    }

    FTextureRenderTargetResource* RTResource = InputRenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        return;
    }

    // Automatically draw the material to the Render Target if configured
    if (InputMaterial)
    {
        UKismetRenderingLibrary::DrawMaterialToRenderTarget(GetWorld(), InputRenderTarget, InputMaterial);
    }

    const int32 SrcWidth = InputRenderTarget->SizeX;
    const int32 SrcHeight = InputRenderTarget->SizeY;

    // Re-derive the inference resolution from the current source size x ResolutionScale, rounding
    // width and height independently to a multiple of 32. This can cost a small (sub-1%, generally
    // imperceptible) center crop in ResizeBilinear's height-driven scale when the two axes round by
    // different amounts -- but it never inflates the inference canvas past what's needed, which
    // matters when the source is already 32-aligned: deriving width from height's rounding slack
    // would otherwise round it up unnecessarily and burn extra compute for no visual benefit. If the
    // result differs from what the pipeline was last initialized with, the NNE tensor buffers no
    // longer match -- tear down and recreate the pipeline before continuing.
    const float Scale = GetResolutionScaleFactor(ResolutionScale);
    const int32 TargetWidth = FMath::Clamp((FMath::RoundToInt(SrcWidth * Scale) + 31) & ~31, 64, 4096);
    const int32 TargetHeight = FMath::Clamp((FMath::RoundToInt(SrcHeight * Scale) + 31) & ~31, 64, 4096);

    if (TargetWidth != InputWidth || TargetHeight != InputHeight)
    {
        InputWidth = TargetWidth;
        InputHeight = TargetHeight;
        ReinitializePipeline();
        return; // Pipeline was just reset; resume processing on the next tick.
    }

    // Enqueue an async GPU->CPU readback of the render target instead of calling
    // RTResource->ReadPixels() synchronously here -- ReadPixels() forces the Game Thread to stall
    // until the GPU flushes and copies the full native-resolution frame back, and that stall's
    // cost is completely independent of ResolutionScale (it always reads the native size, before
    // any downscaling), which was capping FPS at a fixed value no matter how far the inference
    // resolution was reduced. See UNGSVMCompositingPass for the identical pattern.
    if (!AsyncReadback)
    {
        AsyncReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("NGSVMManagerReadback"));
    }

    ReadbackSrcWidth = SrcWidth;
    ReadbackSrcHeight = SrcHeight;

    ENQUEUE_RENDER_COMMAND(NGSVMManagerEnqueueReadback)(
        [ReadbackPtr = AsyncReadback.Get(), RTResource, Width = SrcWidth, Height = SrcHeight](FRHICommandListImmediate& RHICmdList)
        {
            FTextureRHIRef SrcTex = RTResource->GetTextureRHI();
            if (!SrcTex.IsValid())
            {
                return;
            }

            // Convert to a known PF_B8G8R8A8 LDR texture before reading back -- InputRenderTarget's
            // actual pixel format isn't guaranteed to be BGRA8, and reinterpreting other formats'
            // raw bytes as FColor on the CPU side would produce garbage.
            FRDGBuilder GraphBuilder(RHICmdList);
            FRDGTextureRef SrcRDG = RegisterExternalTexture(GraphBuilder, SrcTex, TEXT("NGSVMManagerSource"));

            // Match the source's sRGB flag on the temp texture -- sampling an sRGB source
            // implicitly linearizes on read; writing that into a non-sRGB destination stores it
            // as-is with no re-encode, making the captured colors read too dark by roughly one
            // gamma step. Propagating the flag keeps the blit gamma-neutral.
            const ETextureCreateFlags SRGBFlag = EnumHasAnyFlags(SrcTex->GetDesc().Flags, TexCreate_SRGB) ? TexCreate_SRGB : TexCreate_None;
            FRDGTextureDesc TempDesc = FRDGTextureDesc::Create2D(
                FIntPoint(Width, Height),
                PF_B8G8R8A8,
                FClearValueBinding::None,
                TexCreate_RenderTargetable | TexCreate_ShaderResource | SRGBFlag);
            FRDGTextureRef TempTexture = GraphBuilder.CreateTexture(TempDesc, TEXT("NGSVM_ManagerReadbackTemp"));

            FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            FRDGDrawTextureInfo DrawInfo;
            AddDrawTexturePass(GraphBuilder, ShaderMap, SrcRDG, TempTexture, DrawInfo);

            AddEnqueueCopyPass(GraphBuilder, ReadbackPtr, TempTexture);

            GraphBuilder.Execute();
        });

    bReadbackPending = true;
}

void UNGSVMManager::ProcessCapturedFrame(TArray<FColor>&& SrcPixels, int32 CapturedWidth, int32 CapturedHeight)
{
    if (!Pipeline.IsValid())
    {
        return;
    }

    // Queue the full-resolution color frame into history to allow delay synchronization matching
    // model latency. Kept at source resolution (not inference resolution) so the eventual output
    // color texture isn't limited by however much ResolutionScale downscaled the AI input.
    ColorFrameHistory.Insert(SrcPixels, 0);
    if (ColorFrameHistory.Num() > 30)
    {
        ColorFrameHistory.RemoveAt(30);
    }

    // Retrieve the matching historical color frame based on configured delay
    const int32 TargetIdx = FMath::Clamp(ColorFrameDelay, 0, ColorFrameHistory.Num() - 1);
    CurrentDelayedColorFrame = ColorFrameHistory[TargetIdx];

    // Scale the captured pixels down to the neural network's inference resolution
    TArray<FColor> ResizedPixels;
    if (CapturedWidth == InputWidth && CapturedHeight == InputHeight)
    {
        ResizedPixels = SrcPixels;
    }
    else
    {
        ResizeBilinear(SrcPixels, CapturedWidth, CapturedHeight, ResizedPixels, InputWidth, InputHeight);
    }

    // Convert pixels to tensor format (e.g. NCHW float/half) inside the pipeline input data buffer
    if (!Pipeline->Preprocess(ResizedPixels))
    {
        return;
    }

    TimeSinceLastInference = 0.0f;
    bIsRunning = true;

    TSharedPtr<INGSVMPipeline> PipelineRef = Pipeline;
    TWeakObjectPtr<UNGSVMManager> WeakThis(this);
    const bool bIsGPU = (ExecutionDevice != ENNEExecutionDevice::CPU);

    auto RunInferenceAndFinish = [PipelineRef, WeakThis]()
        {
            auto ModelInstance = PipelineRef->GetModelInstance();
            if (ModelInstance.IsValid())
            {
                // Execute synchronous inference using NNE Runtime bindings
                if (ModelInstance->RunSync(
                    PipelineRef->GetInputBindings(),
                    PipelineRef->GetOutputBindings()) != UE::NNE::EResultStatus::Ok)
                {
                    UE_LOG(LogTemp, Error, TEXT("Inference Failed"));
                }
            }

            TArray<uint8> MaskBuffer;
            const bool bHasMask = PipelineRef->Postprocess(MaskBuffer);

            // Complete processing and update textures on the Game Thread
            AsyncTask(ENamedThreads::GameThread, [WeakThis, MaskBuffer, bHasMask]()
                {
                    if (UNGSVMManager* StrongThis = WeakThis.Get())
                    {
                        StrongThis->bIsRunning = false;
                        if (bHasMask)
                        {
                            StrongThis->UpdateTextures(MaskBuffer);
                        }
                    }
                });
        };

    if (bIsGPU)
    {
        // DirectML/D3D12 GPU command submission isn't safe from an arbitrary TaskGraph worker
        // thread -- ONNX Runtime's DirectML execution provider needs to run on the Render Thread
        // (the only thread the RHI/graphics driver is set up to be called from). Calling RunSync()
        // from AnyNormalThreadNormalTask (as this used to, unconditionally for both CPU and GPU)
        // crashes deep inside the NVIDIA driver (nvwgf2umx/D3D12Core/DirectML/onnxruntime).
        ENQUEUE_RENDER_COMMAND(NGSVMManagerRunGPUInference)(
            [RunInferenceAndFinish](FRHICommandListImmediate&) mutable
            {
                RunInferenceAndFinish();
            });
    }
    else
    {
        // CPU inference MUST run on the Game Thread.
        AsyncTask(ENamedThreads::GameThread, [RunInferenceAndFinish]() mutable
            {
                RunInferenceAndFinish();
            });
    }
}

void UNGSVMManager::ResizeBilinear(const TArray<FColor>& SourcePixels, int32 SourceWidth, int32 SourceHeight,
                                  TArray<FColor>& OutPixels, int32 TargetWidth, int32 TargetHeight)
{
    // Scale uniformly using height as the reference axis, then center-crop (or pad) width, to
    // preserve the source aspect ratio instead of stretching X and Y independently.
    OutPixels.SetNum(TargetWidth * TargetHeight);

    const float Scale = (float)TargetHeight / (float)SourceHeight;
    const float SrcCropX = (SourceWidth - TargetWidth / Scale) * 0.5f;

    // Rows are independent (each writes only its own slice of OutPixels), so distribute them
    // across worker threads instead of running the loop single-threaded.
    ParallelFor(TargetHeight, [&](int32 y)
    {
        const float SrcYf = (y + 0.5f) / Scale - 0.5f;
        const int32 y0 = FMath::FloorToInt(SrcYf);
        const float dy = SrcYf - y0;

        for (int32 x = 0; x < TargetWidth; ++x)
        {
            const float SrcXf = SrcCropX + (x + 0.5f) / Scale - 0.5f;

            // Outside the source image -- only possible for the margin cropped away when
            // resizing in the other direction (e.g. the aspect ratio mismatch between a
            // 32-aligned inference canvas and the native 16:9 source). Clamping below naturally
            // extends the nearest edge pixel here instead of leaving a hard-edged gap, which
            // would otherwise look like the image being cut off.
            const int32 x0 = FMath::Clamp(FMath::FloorToInt(SrcXf), 0, SourceWidth - 1);
            const int32 x1 = FMath::Clamp(x0 + 1, 0, SourceWidth - 1);
            const int32 cy0 = FMath::Clamp(y0, 0, SourceHeight - 1);
            const int32 cy1 = FMath::Clamp(y0 + 1, 0, SourceHeight - 1);
            const float dx = FMath::Clamp(SrcXf - x0, 0.0f, 1.0f);
            const float cdy = FMath::Clamp(dy, 0.0f, 1.0f);

            const FColor c00 = SourcePixels[cy0 * SourceWidth + x0];
            const FColor c10 = SourcePixels[cy0 * SourceWidth + x1];
            const FColor c01 = SourcePixels[cy1 * SourceWidth + x0];
            const FColor c11 = SourcePixels[cy1 * SourceWidth + x1];

            FColor Color;
            Color.R = (uint8)FMath::Lerp(FMath::Lerp((float)c00.R, (float)c10.R, dx), FMath::Lerp((float)c01.R, (float)c11.R, dx), cdy);
            Color.G = (uint8)FMath::Lerp(FMath::Lerp((float)c00.G, (float)c10.G, dx), FMath::Lerp((float)c01.G, (float)c11.G, dx), cdy);
            Color.B = (uint8)FMath::Lerp(FMath::Lerp((float)c00.B, (float)c10.B, dx), FMath::Lerp((float)c01.B, (float)c11.B, dx), cdy);
            Color.A = 255;

            OutPixels[y * TargetWidth + x] = Color;
        }
    });
}

void UNGSVMManager::ResizeBilinearGrayscale(const TArray<uint8>& SourcePixels, int32 SourceWidth, int32 SourceHeight,
                                  TArray<uint8>& OutPixels, int32 TargetWidth, int32 TargetHeight)
{
    // Same scale-by-height, crop/pad-width approach as ResizeBilinear, for a single channel.
    OutPixels.SetNum(TargetWidth * TargetHeight);

    const float Scale = (float)TargetHeight / (float)SourceHeight;
    const float SrcCropX = (SourceWidth - TargetWidth / Scale) * 0.5f;

    ParallelFor(TargetHeight, [&](int32 y)
    {
        const float SrcYf = (y + 0.5f) / Scale - 0.5f;
        const int32 y0 = FMath::FloorToInt(SrcYf);
        const float dy = SrcYf - y0;

        for (int32 x = 0; x < TargetWidth; ++x)
        {
            const float SrcXf = SrcCropX + (x + 0.5f) / Scale - 0.5f;

            // Clamping below extends the nearest edge alpha value into the margin cropped away
            // by the other resize direction, instead of leaving a hard transparent gap that
            // reads as the subject being cut off at the frame edge.
            const int32 x0 = FMath::Clamp(FMath::FloorToInt(SrcXf), 0, SourceWidth - 1);
            const int32 x1 = FMath::Clamp(x0 + 1, 0, SourceWidth - 1);
            const int32 cy0 = FMath::Clamp(y0, 0, SourceHeight - 1);
            const int32 cy1 = FMath::Clamp(y0 + 1, 0, SourceHeight - 1);
            const float dx = FMath::Clamp(SrcXf - x0, 0.0f, 1.0f);
            const float cdy = FMath::Clamp(dy, 0.0f, 1.0f);

            const uint8 c00 = SourcePixels[cy0 * SourceWidth + x0];
            const uint8 c10 = SourcePixels[cy0 * SourceWidth + x1];
            const uint8 c01 = SourcePixels[cy1 * SourceWidth + x0];
            const uint8 c11 = SourcePixels[cy1 * SourceWidth + x1];

            OutPixels[y * TargetWidth + x] = (uint8)FMath::Lerp(FMath::Lerp((float)c00, (float)c10, dx), FMath::Lerp((float)c01, (float)c11, dx), cdy);
        }
    });
}

void UNGSVMManager::UpdateTextures(const TArray<uint8>& MaskData)
{
    if (!OutputMaskTexture || MaskData.Num() == 0)
    {
        return;
    }

    int32 Width = (OutputWidth > 0) ? OutputWidth : (InputRenderTarget ? InputRenderTarget->SizeX : InputWidth);
    int32 Height = (OutputHeight > 0) ? OutputHeight : (InputRenderTarget ? InputRenderTarget->SizeY : InputHeight);
    Width = FMath::Max(1, Width);
    Height = FMath::Max(1, Height);

    // Get the RHI Texture Reference for the grayscale mask staging texture
    FTextureRHIRef TextureRHI = OutputMaskTexture->GetResource() ? OutputMaskTexture->GetResource()->GetTexture2DRHI() : nullptr;
    FTextureRenderTargetResource* MaskRTResource = OutputMaskRenderTarget ? OutputMaskRenderTarget->GameThread_GetRenderTargetResource() : nullptr;

    if (!TextureRHI.IsValid())
    {
        return;
    }

    // MaskData comes back from the model at InputWidth x InputHeight (the inference resolution,
    // which ResolutionScale may have downscaled) -- scale it back up to the output resolution
    // proportionally before uploading.
    FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
    TArray<uint8> PixelBuffer;
    if (MaskData.Num() == Width * Height)
    {
        PixelBuffer = MaskData;
    }
    else
    {
        ResizeBilinearGrayscale(MaskData, InputWidth, InputHeight, PixelBuffer, Width, Height);
    }

    // Enqueue command on the render thread to update the staging texture and copy to Render Target if specified
    ENQUEUE_RENDER_COMMAND(UpdateTextureMaskCmd)(
        [TextureRHI, Region, PixelBuffer, MaskRTResource](FRHICommandListImmediate& RHICmdList)
        {
            GDynamicRHI->RHIUpdateTexture2D(
                RHICmdList,
                TextureRHI,
                0,
                Region,
                Region.Width,
                PixelBuffer.GetData()
            );

            if (MaskRTResource)
            {
                FTextureRHIRef DestTextureRHI = MaskRTResource->GetTextureRHI();
                if (DestTextureRHI.IsValid())
                {
                    FRHICopyTextureInfo CopyInfo;
                    RHICmdList.CopyTexture(TextureRHI, DestTextureRHI, CopyInfo);
                }
            }
        });

    // Update synchronized color texture with the historical (delayed) color buffer.
    if (OutputColorTexture && CurrentDelayedColorFrame.Num() > 0)
    {
        FTextureRHIRef ColorTextureRHI = OutputColorTexture->GetResource() ? OutputColorTexture->GetResource()->GetTexture2DRHI() : nullptr;
        FTextureRenderTargetResource* ColorRTResource = OutputColorRenderTarget ? OutputColorRenderTarget->GameThread_GetRenderTargetResource() : nullptr;

        if (ColorTextureRHI.IsValid())
        {
            FUpdateTextureRegion2D ColorRegion(0, 0, 0, 0, Width, Height);

            // CurrentDelayedColorFrame is captured at InputRenderTarget's native resolution; only
            // needs resizing here if OutputWidth/OutputHeight were manually overridden to something else.
            TArray<FColor> TempColorBuffer;
            if (CurrentDelayedColorFrame.Num() == Width * Height)
            {
                TempColorBuffer = CurrentDelayedColorFrame;
            }
            else if (CurrentDelayedColorFrame.Num() > 0)
            {
                const int32 CapturedWidth = InputRenderTarget ? InputRenderTarget->SizeX : Width;
                const int32 CapturedHeight = InputRenderTarget ? InputRenderTarget->SizeY : Height;
                ResizeBilinear(CurrentDelayedColorFrame, CapturedWidth, CapturedHeight, TempColorBuffer, Width, Height);
            }

            // Composite the upscaled alpha matte (PixelBuffer, already resized to Width x Height
            // above) into the color buffer's alpha channel -- straight, non-premultiplied alpha,
            // RGB untouched -- so OutputColorTexture/OutputColorRenderTarget carries the keyed
            // result directly, matching UNGSVMFunctionLibrary::NGSVM_KeyImage_asTexture2D's output
            // convention instead of always being fully opaque.
            if (TempColorBuffer.Num() == PixelBuffer.Num())
            {
                ParallelFor(TempColorBuffer.Num(), [&](int32 i)
                {
                    TempColorBuffer[i].A = PixelBuffer[i];
                });
            }

            // Enqueue render command to update RHI texture memory and copy to Render Target if specified
            ENQUEUE_RENDER_COMMAND(UpdateColorTextureCmd)(
                [ColorTextureRHI, ColorRegion, TempColorBuffer, ColorRTResource](FRHICommandListImmediate& RHICmdList)
                {
                    GDynamicRHI->RHIUpdateTexture2D(
                        RHICmdList,
                        ColorTextureRHI,
                        0,
                        ColorRegion,
                        ColorRegion.Width * sizeof(FColor),
                        (const uint8*)TempColorBuffer.GetData()
                    );

                    if (ColorRTResource)
                    {
                        FTextureRHIRef DestTextureRHI = ColorRTResource->GetTextureRHI();
                        if (DestTextureRHI.IsValid())
                        {
                            FRHICopyTextureInfo CopyInfo;
                            RHICmdList.CopyTexture(ColorTextureRHI, DestTextureRHI, CopyInfo);
                        }
                    }
                });
        }
    }
}