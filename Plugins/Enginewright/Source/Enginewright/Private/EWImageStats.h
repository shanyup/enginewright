// Shared PNG reading and image statistics (ER-19): used by image_compose, image_stats and game_run variants.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace EW
{
	struct FPngImage
	{
		int32 W = 0;
		int32 H = 0;
		TArray<FColor> Px; // BGRA
	};

	/** Pixel rectangle inside an image. W or H of 0 = whole image. */
	struct FImageRegion
	{
		int32 X = 0, Y = 0, W = 0, H = 0;
	};

	/** Reads a PNG file into 8-bit BGRA pixels. */
	bool DecodePng(const FString& Path, FPngImage& Out, FString& Error);

	/**
	 * Statistics of Img inside Region: meanLuma (0-255, Rec.709 weights on the stored sRGB values), min/max luma,
	 * lumaHistogram (16 bins, percent of pixels), meanSaturation (0-1, HSV), clippedBlackPercent (luma <= 2),
	 * clippedWhitePercent (luma >= 253). With Ref (same size) also a 'diff' object: meanLumaDelta (Img - Ref),
	 * meanAbsPixelDiff (0-255, over R, G and B), changedPixelPercent (any channel differs by more than 8) and ssim
	 * (mean structural similarity of the luma over 8x8 blocks, 1 = identical). Region is clamped by the caller.
	 */
	TSharedRef<FJsonObject> ImageStatsJson(const FPngImage& Img, const FImageRegion& Region, const FPngImage* Ref);
}
