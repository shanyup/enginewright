#include "EWImageStats.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

namespace EW
{
	bool DecodePng(const FString& Path, FPngImage& Out, FString& Error)
	{
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			Error = FString::Printf(TEXT("cannot read %s"), *Path);
			return false;
		}
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		if (M.DetectImageFormat(Bytes.GetData(), Bytes.Num()) != EImageFormat::PNG)
		{
			Error = FString::Printf(TEXT("%s is not a PNG file"), *Path);
			return false;
		}
		TSharedPtr<IImageWrapper> W = M.CreateImageWrapper(EImageFormat::PNG);
		TArray64<uint8> Raw;
		if (!W.IsValid() || !W->SetCompressed(Bytes.GetData(), Bytes.Num()) || !W->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			Error = FString::Printf(TEXT("%s could not be decoded as PNG"), *Path);
			return false;
		}
		Out.W = (int32)W->GetWidth();
		Out.H = (int32)W->GetHeight();
		if (Out.W <= 0 || Out.H <= 0 || Raw.Num() != (int64)Out.W * Out.H * 4)
		{
			Error = FString::Printf(TEXT("%s has an unexpected size"), *Path);
			return false;
		}
		Out.Px.SetNumUninitialized(Out.W * Out.H);
		FMemory::Memcpy(Out.Px.GetData(), Raw.GetData(), Raw.Num());
		return true;
	}

	namespace
	{
		inline double Luma(const FColor& C) { return 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B; }

		double Round(double V, double Scale = 1000.0) { return FMath::RoundToDouble(V * Scale) / Scale; }

		TSharedPtr<FJsonValue> Num(double V) { return MakeShared<FJsonValueNumber>(Round(V)); }
	}

	TSharedRef<FJsonObject> ImageStatsJson(const FPngImage& Img, const FImageRegion& Rgn, const FPngImage* Ref)
	{
		const int32 X0 = Rgn.X, Y0 = Rgn.Y;
		const int32 RW = Rgn.W > 0 ? Rgn.W : Img.W;
		const int32 RH = Rgn.H > 0 ? Rgn.H : Img.H;
		const double N = (double)RW * RH;

		double SumLuma = 0, SumSat = 0, MinLuma = 255, MaxLuma = 0;
		int64 Black = 0, White = 0;
		int64 Bins[16] = {};
		for (int32 Y = Y0; Y < Y0 + RH; ++Y)
		{
			for (int32 X = X0; X < X0 + RW; ++X)
			{
				const FColor& C = Img.Px[Y * Img.W + X];
				const double L = Luma(C);
				SumLuma += L;
				MinLuma = FMath::Min(MinLuma, L);
				MaxLuma = FMath::Max(MaxLuma, L);
				const int32 Mx = FMath::Max3<int32>(C.R, C.G, C.B), Mn = FMath::Min3<int32>(C.R, C.G, C.B);
				if (Mx > 0) { SumSat += (double)(Mx - Mn) / Mx; }
				if (L <= 2.0) { ++Black; }
				if (L >= 253.0) { ++White; }
				++Bins[FMath::Clamp((int32)(L / 16.0), 0, 15)];
			}
		}

		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		TArray<int32> Size = { RW, RH };
		TArray<TSharedPtr<FJsonValue>> SizeJson;
		for (int32 V : Size) { SizeJson.Add(MakeShared<FJsonValueNumber>(V)); }
		J->SetArrayField(TEXT("size"), SizeJson);
		J->SetNumberField(TEXT("pixels"), N);
		J->SetField(TEXT("meanLuma"), Num(SumLuma / N));
		J->SetField(TEXT("minLuma"), Num(MinLuma));
		J->SetField(TEXT("maxLuma"), Num(MaxLuma));
		TArray<TSharedPtr<FJsonValue>> Hist;
		for (int32 B = 0; B < 16; ++B) { Hist.Add(Num(100.0 * Bins[B] / N)); }
		J->SetArrayField(TEXT("lumaHistogram"), Hist);
		J->SetField(TEXT("meanSaturation"), Num(SumSat / N));
		J->SetField(TEXT("clippedBlackPercent"), Num(100.0 * Black / N));
		J->SetField(TEXT("clippedWhitePercent"), Num(100.0 * White / N));

		if (Ref)
		{
			double SumRefLuma = 0, SumAbs = 0;
			int64 Changed = 0;
			for (int32 Y = Y0; Y < Y0 + RH; ++Y)
			{
				for (int32 X = X0; X < X0 + RW; ++X)
				{
					const FColor& A = Img.Px[Y * Img.W + X];
					const FColor& B = Ref->Px[Y * Ref->W + X];
					SumRefLuma += Luma(B);
					const int32 DR = FMath::Abs((int32)A.R - B.R), DG = FMath::Abs((int32)A.G - B.G), DB = FMath::Abs((int32)A.B - B.B);
					SumAbs += (DR + DG + DB) / 3.0;
					if (FMath::Max3(DR, DG, DB) > 8) { ++Changed; }
				}
			}
			// SSIM of the luma over non-overlapping 8x8 blocks (smaller at the edges), averaged.
			const double C1 = (0.01 * 255) * (0.01 * 255), C2 = (0.03 * 255) * (0.03 * 255);
			double SsimSum = 0;
			int64 Blocks = 0;
			for (int32 By = Y0; By < Y0 + RH; By += 8)
			{
				for (int32 Bx = X0; Bx < X0 + RW; Bx += 8)
				{
					const int32 BW = FMath::Min(8, X0 + RW - Bx), BH = FMath::Min(8, Y0 + RH - By);
					const double Cnt = (double)BW * BH;
					double Ma = 0, Mb = 0;
					for (int32 Y = By; Y < By + BH; ++Y)
					{
						for (int32 X = Bx; X < Bx + BW; ++X) { Ma += Luma(Img.Px[Y * Img.W + X]); Mb += Luma(Ref->Px[Y * Ref->W + X]); }
					}
					Ma /= Cnt;
					Mb /= Cnt;
					double Va = 0, Vb = 0, Cov = 0;
					for (int32 Y = By; Y < By + BH; ++Y)
					{
						for (int32 X = Bx; X < Bx + BW; ++X)
						{
							const double A = Luma(Img.Px[Y * Img.W + X]) - Ma, B = Luma(Ref->Px[Y * Ref->W + X]) - Mb;
							Va += A * A; Vb += B * B; Cov += A * B;
						}
					}
					const double Div = FMath::Max(1.0, Cnt - 1);
					Va /= Div; Vb /= Div; Cov /= Div;
					SsimSum += ((2 * Ma * Mb + C1) * (2 * Cov + C2)) / ((Ma * Ma + Mb * Mb + C1) * (Va + Vb + C2));
					++Blocks;
				}
			}
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetField(TEXT("meanLumaDelta"), Num(SumLuma / N - SumRefLuma / N));
			D->SetField(TEXT("meanAbsPixelDiff"), Num(SumAbs / N));
			D->SetField(TEXT("changedPixelPercent"), Num(100.0 * Changed / N));
			D->SetField(TEXT("ssim"), Num(Blocks ? SsimSum / Blocks : 1.0));
			J->SetObjectField(TEXT("diff"), D);
		}
		return J;
	}
}
