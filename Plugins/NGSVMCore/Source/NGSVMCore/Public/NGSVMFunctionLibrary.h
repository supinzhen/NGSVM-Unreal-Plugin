// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NGSVMSettings.h"
#include "NGSVMManager.h" // for ENNEExecutionDevice / ENGSVMResolutionScale enums
#include "NGSVMFunctionLibrary.generated.h"

/**
 * Stateless NGSVM utility functions that don't need a persistent UNGSVMManager component.
 */
UCLASS()
class NGSVMCORE_API UNGSVMFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Runs NGSVM AI matting on a single static texture. ResultTexture receives a new UTexture2D
	 * with the alpha channel set to the computed matte (straight, non-premultiplied alpha; the RGB
	 * channels are the original image, untouched). AlphaMatte receives a second texture containing
	 * just the grayscale matte (format PF_G8), at the same native resolution. Output names/order
	 * match UNGSVMAsyncKeyImage's OnSuccess/OnFailure delegate (ResultTexture, AlphaMatte) exactly.
	 * Fully synchronous/blocking -- on failure, logs an error, sets ResultTexture to OriginalTexture
	 * unchanged, and leaves AlphaMatte null.
	 */
	UFUNCTION(BlueprintCallable, Category = "NGSVM")
	static void NGSVM_KeyImage_asTexture2D(UTexture2D* OriginalTexture, ENGSVMModelType ModelType, ENNEExecutionDevice Device, ENGSVMResolutionScale Scale, UTexture2D*& ResultTexture, UTexture2D*& AlphaMatte);

	// ── Shared helpers ────────────────────────────────────────────────────────
	// Public (not UFUNCTION -- these aren't reflectable/Blueprint types) so UNGSVMAsyncKeyImage
	// can reuse them instead of duplicating this logic a second time.

	// Converts a ResolutionScale enum value to its multiplier (1.0, 0.5, 0.25, 0.125).
	static float GetResolutionScaleFactor(ENGSVMResolutionScale InScale);

	/** Synchronously reads Texture's pixels at its native resolution via a GPU blit into a
	 *  known PF_B8G8R8A8 render target followed by ReadPixels() -- works regardless of the
	 *  source texture's compression/streaming settings, unlike reading CPU bulk data directly
	 *  (which isn't retained for most imported textures). */
	static bool ReadTexturePixelsSync(UTexture2D* Texture, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);

	// CPU bilinear resize -- aspect-preserving (scale by height, edge-clamped width margin),
	// same algorithm used throughout NGSVM (UNGSVMManager, UNGSVMCompositePass, etc).
	static void ResizeBilinear(const TArray<FColor>& Src, int32 SrcW, int32 SrcH, TArray<FColor>& Out, int32 DstW, int32 DstH);

	// Same as ResizeBilinear, for the single-channel (grayscale) mask buffer.
	static void ResizeBilinearGrayscale(const TArray<uint8>& Src, int32 SrcW, int32 SrcH, TArray<uint8>& Out, int32 DstW, int32 DstH);

	/** Creates a Transient PF_G8 texture (SRGB=false, CompressionSettings=TC_Grayscale, matching
	 *  UNGSVMManager's OutputMaskTexture convention) and calls UpdateResource() on it. Caller is
	 *  responsible for uploading MaskData afterward (synchronously or async, as appropriate). */
	static UTexture2D* CreateGrayscaleTexture(int32 Width, int32 Height);
};
