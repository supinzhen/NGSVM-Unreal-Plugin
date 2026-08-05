// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMFunctionLibrary.h"
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
#include "Async/ParallelFor.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
// Full definition required: GetTransientPackage()'s return type (UPackage) must be complete for
// the implicit UPackage*->UObject* upcast into NewObject<T>(UObject* Outer) below to resolve --
// same class of Unity-Build-ordering-luck bug as the RHI/LocalPlayer/World fixes elsewhere.
#include "UObject/Package.h"

float UNGSVMFunctionLibrary::GetResolutionScaleFactor(ENGSVMResolutionScale InScale)
{
    switch (InScale)
    {
    case ENGSVMResolutionScale::Half:
        return 0.5f;
    case ENGSVMResolutionScale::Quarter:
        return 0.25f;
    case ENGSVMResolutionScale::Eighth:
        return 0.125f;
    default:
        return 1.0f;
    }
}

bool UNGSVMFunctionLibrary::ReadTexturePixelsSync(UTexture2D *Texture, TArray<FColor> &OutPixels, int32 &OutWidth, int32 &OutHeight)
{
    if (!Texture || !Texture->GetResource() || !Texture->GetResource()->TextureRHI.IsValid())
    {
        return false;
    }

    OutWidth = Texture->GetSizeX();
    OutHeight = Texture->GetSizeY();
    if (OutWidth <= 0 || OutHeight <= 0)
    {
        return false;
    }

    // Blit into a plain render target first -- Texture's actual GPU format isn't guaranteed to
    // be directly readable as FColor (compressed formats, sRGB flag mismatches, etc.), so go
    // through a known-format GPU copy (same technique used by the NGSVM Composure passes) before
    // reading it back on the CPU. Match the render target's sRGB-ness to the source to avoid a
    // gamma mismatch during the blit (sampling an sRGB source into a non-sRGB destination -- or
    // vice versa -- silently shifts every color by one gamma step).
    UTextureRenderTarget2D *TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
    TempRT->RenderTargetFormat = Texture->SRGB ? RTF_RGBA8_SRGB : RTF_RGBA8;
    TempRT->ClearColor = FLinearColor::Transparent;
    TempRT->bAutoGenerateMips = false;
    TempRT->InitAutoFormat(OutWidth, OutHeight);
    TempRT->UpdateResourceImmediate(true);

    // UpdateResourceImmediate() enqueues the RHI resource creation on the render thread --
    // despite the name, GetRenderTargetTexture() read back here on the Game Thread right
    // afterward isn't guaranteed to be valid yet without an explicit flush.
    FlushRenderingCommands();

    FTextureRenderTargetResource *RTResource = TempRT->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        return false;
    }

    FTextureRHIRef SrcTex = Texture->GetResource()->TextureRHI;
    FTextureRHIRef DestTex = RTResource->GetRenderTargetTexture();
    if (!SrcTex.IsValid() || !DestTex.IsValid())
    {
        return false;
    }

    ENQUEUE_RENDER_COMMAND(NGSVMKeyImageBlitSource)(
        [SrcTex, DestTex](FRHICommandListImmediate &RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);
            FRDGTextureRef SrcRDG = RegisterExternalTexture(GraphBuilder, SrcTex, TEXT("NGSVMKeyImageSource"));
            FRDGTextureRef DestRDG = RegisterExternalTexture(GraphBuilder, DestTex, TEXT("NGSVMKeyImageDest"));

            FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            FRDGDrawTextureInfo DrawInfo;
            AddDrawTexturePass(GraphBuilder, ShaderMap, SrcRDG, DestRDG, DrawInfo);

            GraphBuilder.Execute();
        });

    // This function must return a fully-populated result synchronously (it has no
    // callback/delegate to report completion later), so force the blit above to finish before
    // reading pixels back.
    FlushRenderingCommands();

    return RTResource->ReadPixels(OutPixels) && OutPixels.Num() == OutWidth * OutHeight;
}

UTexture2D *UNGSVMFunctionLibrary::CreateGrayscaleTexture(int32 Width, int32 Height)
{
    UTexture2D *Texture = UTexture2D::CreateTransient(Width, Height, PF_G8);
    if (Texture)
    {
        Texture->SRGB = false;
        Texture->CompressionSettings = TC_Grayscale;
        Texture->UpdateResource();
    }
    return Texture;
}

void UNGSVMFunctionLibrary::NGSVM_KeyImage_asTexture2D(UTexture2D *OriginalTexture, ENGSVMModelType ModelType, ENNEExecutionDevice ExecutionDevice, ENGSVMResolutionScale Scale, UTexture2D *&ResultTexture, UTexture2D *&AlphaMatte)
{
    ResultTexture = OriginalTexture;
    AlphaMatte = nullptr;

    if (!OriginalTexture)
    {
        return;
    }

    // 1. Read the source pixels synchronously at native resolution.
    TArray<FColor> SrcPixels;
    int32 SrcWidth = 0, SrcHeight = 0;
    if (!ReadTexturePixelsSync(OriginalTexture, SrcPixels, SrcWidth, SrcHeight))
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: failed to read source texture pixels."));
        return;
    }

    // 2. Load model data and create the NNE model instance on the requested device.
    UNNEModelData *ModelData = FNGSVMModelLoader::LoadModelData(ModelType);
    if (!ModelData)
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: failed to load model data."));
        return;
    }

    TSharedPtr<UE::NNE::IModelInstanceRunSync> ModelInstance = FNGSVMModelLoader::CreateModelInstance(ModelData, ExecutionDevice);
    if (!ModelInstance.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: failed to create NNE model instance."));
        return;
    }

    // 3. Compute the inference resolution (same 32-aligned rounding used throughout NGSVM).
    const float ScaleFactor = GetResolutionScaleFactor(Scale);
    const int32 InferenceWidth = FMath::Clamp((FMath::RoundToInt(SrcWidth * ScaleFactor) + 31) & ~31, 64, 4096);
    const int32 InferenceHeight = FMath::Clamp((FMath::RoundToInt(SrcHeight * ScaleFactor) + 31) & ~31, 64, 4096);

    // 4. Instantiate the correct pipeline based on model type name, initialized directly at the
    // resolution it will actually run inference at (must match what Preprocess() is fed below).
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

    if (!Pipeline.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: pipeline Init() failed."));
        return;
    }

    // 5. Resize down to inference resolution (aspect-preserving) if needed, then preprocess.
    TArray<FColor> InferencePixels;
    if (SrcWidth == InferenceWidth && SrcHeight == InferenceHeight)
    {
        InferencePixels = SrcPixels;
    }
    else
    {
        ResizeBilinear(SrcPixels, SrcWidth, SrcHeight, InferencePixels, InferenceWidth, InferenceHeight);
    }

    if (!Pipeline->Preprocess(InferencePixels))
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: Preprocess() failed."));
        return;
    }

    // 6. Run inference synchronously. GPU (DirectML) execution must happen on the Render Thread
    // (same constraint as the NGSVM Composure passes); CPU execution runs inline here.
    const bool bIsGPU = (ExecutionDevice != ENNEExecutionDevice::CPU);
    UE::NNE::EResultStatus RunResult = UE::NNE::EResultStatus::Fail;

    if (bIsGPU)
    {
        ENQUEUE_RENDER_COMMAND(NGSVMKeyImageRunGPUInference)(
            [ModelInstance, Pipeline, &RunResult](FRHICommandListImmediate &)
            {
                RunResult = ModelInstance->RunSync(Pipeline->GetInputBindings(), Pipeline->GetOutputBindings());
            });
        FlushRenderingCommands();
    }
    else
    {
        RunResult = ModelInstance->RunSync(Pipeline->GetInputBindings(), Pipeline->GetOutputBindings());
    }

    if (RunResult != UE::NNE::EResultStatus::Ok)
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: NNE inference failed."));
        return;
    }

    // 7. Postprocess into the alpha mask, upscale back to native resolution (aspect-preserving),
    // and apply as straight (non-premultiplied) alpha onto the FULL native-resolution pixels --
    // only the mask goes through the (possibly downscaled) AI round trip, so the RGB detail of
    // the returned texture always matches the original image's full quality.
    TArray<uint8> MaskBuffer;
    if (!Pipeline->Postprocess(MaskBuffer) || MaskBuffer.Num() != InferenceWidth * InferenceHeight)
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: Postprocess() failed."));
        return;
    }

    TArray<uint8> UpscaledMask;
    if (InferenceWidth == SrcWidth && InferenceHeight == SrcHeight)
    {
        UpscaledMask = MaskBuffer;
    }
    else
    {
        ResizeBilinearGrayscale(MaskBuffer, InferenceWidth, InferenceHeight, UpscaledMask, SrcWidth, SrcHeight);
    }

    if (UpscaledMask.Num() != SrcPixels.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: mask/pixel count mismatch after upscale."));
        return;
    }

    ParallelFor(SrcPixels.Num(), [&](int32 i)
                { SrcPixels[i].A = UpscaledMask[i]; });

    // 8. Build and upload the final keyed texture, flushing so the returned handle is fully
    // populated and ready to sample the moment this function returns.
    UTexture2D *NewResultTexture = UTexture2D::CreateTransient(SrcWidth, SrcHeight, PF_B8G8R8A8);
    if (!NewResultTexture)
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: failed to create result texture."));
        return;
    }

    NewResultTexture->SRGB = OriginalTexture->SRGB;
    NewResultTexture->UpdateResource();
    FlushRenderingCommands();

    FTextureRHIRef DestRHI = NewResultTexture->GetResource() ? NewResultTexture->GetResource()->TextureRHI : nullptr;
    if (!DestRHI.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: result texture resource not ready."));
        return;
    }

    FUpdateTextureRegion2D Region(0, 0, 0, 0, SrcWidth, SrcHeight);
    ENQUEUE_RENDER_COMMAND(NGSVMKeyImageUpload)(
        [DestRHI, Region, Pixels = MoveTemp(SrcPixels)](FRHICommandListImmediate &RHICmdList)
        {
            GDynamicRHI->RHIUpdateTexture2D(RHICmdList, DestRHI, 0, Region, Region.Width * sizeof(FColor), (const uint8 *)Pixels.GetData());
        });

    // 9. Build and upload the standalone alpha matte texture (same UpscaledMask already computed
    // above), same flush-before-return contract as the keyed texture.
    UTexture2D *AlphaMatteTexture = CreateGrayscaleTexture(SrcWidth, SrcHeight);
    if (AlphaMatteTexture)
    {
        // CreateGrayscaleTexture()'s UpdateResource() only enqueues the RHI resource creation on
        // the Render Thread, it doesn't wait for it -- same as NewResultTexture above, flush
        // before reading TextureRHI back on the Game Thread.
        FlushRenderingCommands();
        FTextureRHIRef AlphaDestRHI = AlphaMatteTexture->GetResource() ? AlphaMatteTexture->GetResource()->TextureRHI : nullptr;
        if (AlphaDestRHI.IsValid())
        {
            FUpdateTextureRegion2D AlphaRegion(0, 0, 0, 0, SrcWidth, SrcHeight);
            ENQUEUE_RENDER_COMMAND(NGSVMKeyImageAlphaUpload)(
                [AlphaDestRHI, AlphaRegion, MaskPixels = MoveTemp(UpscaledMask)](FRHICommandListImmediate &RHICmdList)
                {
                    GDynamicRHI->RHIUpdateTexture2D(RHICmdList, AlphaDestRHI, 0, AlphaRegion, AlphaRegion.Width, MaskPixels.GetData());
                });
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: alpha matte texture resource not ready."));
            AlphaMatteTexture = nullptr;
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("NGSVM_KeyImage_asTexture2D: failed to create alpha matte texture."));
    }

    FlushRenderingCommands();

    ResultTexture = NewResultTexture;
    AlphaMatte = AlphaMatteTexture;
}

void UNGSVMFunctionLibrary::ResizeBilinear(const TArray<FColor> &SourcePixels, int32 SourceWidth, int32 SourceHeight,
                                           TArray<FColor> &OutPixels, int32 TargetWidth, int32 TargetHeight)
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
                // resizing in the other direction. Clamping below naturally extends the nearest
                // edge pixel here instead of leaving a hard-edged gap.
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
                Color.A = (uint8)FMath::Lerp(FMath::Lerp((float)c00.A, (float)c10.A, dx), FMath::Lerp((float)c01.A, (float)c11.A, dx), cdy);

                OutPixels[y * TargetWidth + x] = Color;
            } });
}

void UNGSVMFunctionLibrary::ResizeBilinearGrayscale(const TArray<uint8> &SourcePixels, int32 SourceWidth, int32 SourceHeight,
                                                    TArray<uint8> &OutPixels, int32 TargetWidth, int32 TargetHeight)
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
            } });
}
