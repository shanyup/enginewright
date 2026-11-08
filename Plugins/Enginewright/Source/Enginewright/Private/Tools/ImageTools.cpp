// image_compose: put PNG images side by side in one labelled PNG (ER-03). Runs in the editor, no Python.
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
#include "EWFontData.inl"

	struct FImg
	{
		int32 W = 0;
		int32 H = 0;
		TArray<FColor> Px; // BGRA
	};

	struct FFont
	{
		TMap<TCHAR, int32> Index;
		TArray<uint8> Raw;
		bool bOk = false;
	};

	const FFont& Font()
	{
		static FFont F;
		static bool bInit = false;
		if (!bInit)
		{
			bInit = true;
			F.Raw.SetNumUninitialized(GEWFontRawSize);
			F.bOk = FCompression::UncompressMemory(NAME_Zlib, F.Raw.GetData(), GEWFontRawSize, GEWFontZ, sizeof(GEWFontZ));
			for (int32 i = 0; i < UE_ARRAY_COUNT(GEWFontChars); ++i) { F.Index.Add((TCHAR)GEWFontChars[i], i); }
		}
		return F;
	}

	bool ReadPng(const FString& Path, FImg& Out, FString& Error)
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

	FImg Resize(const FImg& In, int32 NW, int32 NH)
	{
		if (In.W == NW && In.H == NH) { return In; }
		FImg Out;
		Out.W = NW;
		Out.H = NH;
		Out.Px.SetNumUninitialized(NW * NH);
		FImageUtils::ImageResize(In.W, In.H, TArrayView<const FColor>(In.Px), NW, NH, TArrayView<FColor>(Out.Px), /*bLinearSpace*/ false, /*bForceOpaque*/ true);
		return Out;
	}

	void Fill(FImg& D, int32 X0, int32 Y0, int32 W, int32 H, FColor C)
	{
		for (int32 Y = FMath::Max(0, Y0); Y < FMath::Min(D.H, Y0 + H); ++Y)
		{
			for (int32 X = FMath::Max(0, X0); X < FMath::Min(D.W, X0 + W); ++X) { D.Px[Y * D.W + X] = C; }
		}
	}

	void Blit(FImg& D, const FImg& S, int32 X0, int32 Y0)
	{
		for (int32 Y = 0; Y < S.H; ++Y)
		{
			if (Y0 + Y < 0 || Y0 + Y >= D.H) { continue; }
			FMemory::Memcpy(&D.Px[(Y0 + Y) * D.W + X0], &S.Px[Y * S.W], S.W * sizeof(FColor));
		}
	}

	int32 TextWidth(const FString& T, int32 Scale) { return T.Len() * GEWFontCellW * Scale; }

	/** Draws text with alpha blending; characters outside the font are drawn as '?'. Cut with an ellipsis at MaxWidth. */
	void DrawText(FImg& D, int32 X0, int32 Y0, FString Text, FColor C, int32 Scale, int32 MaxWidth)
	{
		const FFont& F = Font();
		if (!F.bOk) { return; }
		if (MaxWidth > 0 && TextWidth(Text, Scale) > MaxWidth)
		{
			const int32 Keep = FMath::Max(1, MaxWidth / (GEWFontCellW * Scale) - 1);
			Text = Text.Left(Keep) + TEXT("\u2026");
		}
		const int32 Cell = GEWFontCellW * GEWFontCellH;
		int32 X = X0;
		for (TCHAR Ch : Text)
		{
			const int32* Idx = F.Index.Find(Ch);
			if (!Idx) { Idx = F.Index.Find(TEXT('?')); }
			const uint8* G = F.Raw.GetData() + (*Idx) * Cell;
			for (int32 GY = 0; GY < GEWFontCellH; ++GY)
			{
				for (int32 GX = 0; GX < GEWFontCellW; ++GX)
				{
					const int32 A = G[GY * GEWFontCellW + GX];
					if (!A) { continue; }
					for (int32 SY = 0; SY < Scale; ++SY)
					{
						const int32 PY = Y0 + GY * Scale + SY;
						if (PY < 0 || PY >= D.H) { continue; }
						for (int32 SX = 0; SX < Scale; ++SX)
						{
							const int32 PX = X + GX * Scale + SX;
							if (PX < 0 || PX >= D.W) { continue; }
							FColor& P = D.Px[PY * D.W + PX];
							P.R = (uint8)((C.R * A + P.R * (255 - A)) / 255);
							P.G = (uint8)((C.G * A + P.G * (255 - A)) / 255);
							P.B = (uint8)((C.B * A + P.B * (255 - A)) / 255);
						}
					}
				}
			}
			X += GEWFontCellW * Scale;
		}
	}

	FString UnsupportedChars(const TArray<FString>& Texts)
	{
		const FFont& F = Font();
		TArray<TCHAR> Missing;
		for (const FString& T : Texts)
		{
			for (TCHAR Ch : T) { if (!F.Index.Contains(Ch)) { Missing.AddUnique(Ch); } }
		}
		Missing.Sort();
		FString Out;
		for (TCHAR Ch : Missing) { Out.AppendChar(Ch); }
		return Out;
	}

	TSharedPtr<FJsonValue> IntArray(std::initializer_list<int32> V)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (int32 X : V) { A.Add(MakeShared<FJsonValueNumber>(X)); }
		return MakeShared<FJsonValueArray>(A);
	}

	const FColor BG(24, 24, 28, 255);   // FColor(R, G, B, A)
	const FColor BAND(40, 40, 48, 255);
	const FColor FG(240, 240, 240, 255);
	const FColor DIM(170, 170, 180, 255);
}

void RegisterImageTools(FEWRegistry& R)
{
	R.Register(TEXT("image_compose"),
		TEXT("Put PNG images side by side in one labelled PNG (e.g. web mock-up vs game, 1920 vs 1280, before vs after): optional title band, column labels, row labels; without column labels each cell shows its file name. ")
		TEXT("Images are scaled down to cellWidth keeping their aspect ratio. The written file is read back: size and every cell's pixels are compared. Labels support Latin-1 and Latin Extended-A (Turkish, Polish, ...); data.unsupportedChars lists characters drawn as '?'. Works without Play In Editor; files only, no assets."),
		TEXT(R"({"type":"object","properties":{
			"rows":{"type":"array","description":"Rows of the grid: [{\"label\":\"1920\",\"images\":[\"C:/shots/web.png\",\"C:/shots/game.png\"]}, ...]. label is optional. Absolute paths, or relative to the project folder."},
			"images":{"type":"array","description":"Shorthand for one row without a label: [\"a.png\",\"b.png\"]. Use rows or images."},
			"columns":{"type":"array","description":"Column labels, one per column, e.g. [\"Web\",\"Game\"]."},
			"title":{"type":"string","description":"Title band above the grid."},
			"cellWidth":{"type":"integer","description":"Scale images down to this width (default 960; 0 = original size; never scaled up)."},
			"out":{"type":"string","description":"Output PNG path (absolute, or relative to the project folder). Parent folders are created."}},
			"required":["out"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			auto Abs = [&ProjectDir](const FString& P)
			{
				FString S = P;
				FPaths::NormalizeFilename(S);
				return FPaths::IsRelative(S) ? FPaths::ConvertRelativePathToFull(ProjectDir, S) : FPaths::ConvertRelativePathToFull(S);
			};

			// Collect rows: (label, paths)
			TArray<TPair<FString, TArray<FString>>> Rows;
			const TArray<TSharedPtr<FJsonValue>>* RowsJson = nullptr;
			if (Args->TryGetArrayField(TEXT("rows"), RowsJson))
			{
				for (int32 i = 0; i < RowsJson->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* RowObj = nullptr;
					if (!(*RowsJson)[i]->TryGetObject(RowObj))
					{
						return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("rows[%d] must be an object {\"label\":..., \"images\":[...]}."), i));
					}
					for (const auto& KV : (*RowObj)->Values)
					{
						if (KV.Key != TEXT("label") && KV.Key != TEXT("images"))
						{
							return FEWResult::Error(TEXT("UNKNOWN_ARGUMENT"), FString::Printf(TEXT("rows[%d] has unknown key '%s'. Allowed: label, images."), i, *KV.Key));
						}
					}
					TArray<FString> Paths = EW::StrList(*RowObj, TEXT("images"));
					if (Paths.Num() == 0) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("rows[%d] needs at least one image in 'images'."), i)); }
					Rows.Emplace(EW::Str(*RowObj, TEXT("label")), Paths);
				}
			}
			if (Args->HasField(TEXT("images")))
			{
				TArray<FString> Paths = EW::StrList(Args, TEXT("images"));
				if (Paths.Num() == 0) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("'images' is empty.")); }
				Rows.Emplace(FString(), Paths);
			}
			if (Rows.Num() == 0) { return FEWResult::Error(TEXT("MISSING_ARGUMENT"), TEXT("Give 'rows' ([{label, images}]) or 'images' (one row).")); }
			const TArray<FString> Cols = EW::StrList(Args, TEXT("columns"));
			const FString Title = EW::Str(Args, TEXT("title"));
			const int32 CellWidth = (int32)EW::Num(Args, TEXT("cellWidth"), 960);
			if (CellWidth < 0) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("cellWidth must be 0 (original size) or more.")); }
			const FString Out = Abs(EW::Str(Args, TEXT("out")));
			if (!Out.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("'out' must be a .png file path.")); }
			int32 NCols = 0;
			for (const auto& Row : Rows) { NCols = FMath::Max(NCols, Row.Value.Num()); }
			if (Cols.Num() && Cols.Num() != NCols)
			{
				return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%d column label(s) for %d column(s)."), Cols.Num(), NCols));
			}
			if (!Font().bOk) { return FEWResult::Error(TEXT("INTERNAL"), TEXT("The built-in label font could not be unpacked.")); }

			// Load and scale.
			TArray<TArray<FImg>> Src, Scaled;
			TArray<TArray<FString>> AbsPaths;
			for (const auto& Row : Rows)
			{
				TArray<FImg>& SR = Src.AddDefaulted_GetRef();
				TArray<FImg>& DR = Scaled.AddDefaulted_GetRef();
				TArray<FString>& PR = AbsPaths.AddDefaulted_GetRef();
				for (const FString& P : Row.Value)
				{
					const FString A = Abs(P);
					if (!IFileManager::Get().FileExists(*A)) { return FEWResult::Error(TEXT("NOT_FOUND"), FString::Printf(TEXT("Image not found: %s"), *A)); }
					FImg I;
					FString Err;
					if (!ReadPng(A, I, Err)) { return FEWResult::Error(TEXT("BAD_IMAGE"), Err); }
					const int32 TW = (CellWidth == 0 || I.W <= CellWidth) ? I.W : CellWidth;
					const int32 TH = FMath::Max(1, FMath::RoundToInt((double)I.H * TW / I.W));
					DR.Add(Resize(I, TW, TH));
					SR.Add(MoveTemp(I));
					PR.Add(A);
				}
			}
			TArray<int32> ColW, RowH;
			ColW.Init(0, NCols);
			for (const TArray<FImg>& Row : Scaled)
			{
				int32 H = 0;
				for (int32 c = 0; c < Row.Num(); ++c) { ColW[c] = FMath::Max(ColW[c], Row[c].W); H = FMath::Max(H, Row[c].H); }
				RowH.Add(H);
			}
			int32 SumW = 0;
			for (int32 W : ColW) { SumW += W; }
			const int32 Gap = 8;
			const int32 Scale = SumW >= 1600 ? 2 : 1;
			const int32 CH = GEWFontCellH * Scale;
			const int32 Pad = 6 * Scale;
			const int32 LabelH = CH + 2 * Pad;
			bool bRowLabels = false;
			int32 Left = 0;
			for (const auto& Row : Rows) { if (!Row.Key.IsEmpty()) { bRowLabels = true; Left = FMath::Max(Left, TextWidth(Row.Key, Scale)); } }
			Left = bRowLabels ? Left + 2 * Pad : 0;
			const int32 TitleH = Title.IsEmpty() ? 0 : CH + 2 * Pad;
			const int32 HeadH = Cols.Num() ? LabelH : 0;
			const int32 CellLabelH = Cols.Num() ? 0 : LabelH; // without column labels each cell shows its file name
			const int32 W = Left + SumW + Gap * (NCols + 1);
			int32 H = TitleH + HeadH + Gap * (Rows.Num() + 1);
			for (int32 RH : RowH) { H += RH + CellLabelH; }
			if ((int64)W * H > 16384LL * 16384LL) { return FEWResult::Error(TEXT("TOO_LARGE"), FString::Printf(TEXT("The result would be %d x %d pixels; lower cellWidth."), W, H)); }

			FImg Img;
			Img.W = W;
			Img.H = H;
			Img.Px.Init(BG, W * H);
			int32 Y = 0;
			if (!Title.IsEmpty())
			{
				Fill(Img, 0, 0, W, TitleH, BAND);
				DrawText(Img, Pad + Gap, Pad, Title, FG, Scale, W - 2 * Pad);
				Y = TitleH;
			}
			TArray<int32> Xs;
			int32 X = Left + Gap;
			for (int32 c = 0; c < NCols; ++c) { Xs.Add(X); X += ColW[c] + Gap; }
			if (Cols.Num())
			{
				for (int32 c = 0; c < NCols; ++c)
				{
					DrawText(Img, Xs[c] + FMath::Max(0, (ColW[c] - TextWidth(Cols[c], Scale)) / 2), Y + Pad, Cols[c], FG, Scale, ColW[c]);
				}
				Y += HeadH;
			}
			Y += Gap;
			struct FCell { int32 Row, Col, X, Y; };
			TArray<FCell> Cells;
			TArray<TSharedPtr<FJsonValue>> CellsJson;
			for (int32 r = 0; r < Rows.Num(); ++r)
			{
				if (!Rows[r].Key.IsEmpty())
				{
					DrawText(Img, Pad, Y + CellLabelH + FMath::Max(0, (RowH[r] - CH) / 2), Rows[r].Key, FG, Scale, Left - Pad);
				}
				for (int32 c = 0; c < Scaled[r].Num(); ++c)
				{
					const FImg& S = Scaled[r][c];
					const int32 CX = Xs[c] + (ColW[c] - S.W) / 2;
					const int32 CY = Y + CellLabelH;
					if (CellLabelH) { DrawText(Img, Xs[c], Y + Pad, FPaths::GetCleanFilename(AbsPaths[r][c]), DIM, Scale, ColW[c]); }
					Blit(Img, S, CX, CY);
					Cells.Add({ r, c, CX, CY });
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetNumberField(TEXT("row"), r);
					J->SetNumberField(TEXT("col"), c);
					if (!Rows[r].Key.IsEmpty()) { J->SetStringField(TEXT("rowLabel"), Rows[r].Key); }
					if (Cols.Num()) { J->SetStringField(TEXT("colLabel"), Cols[c]); }
					J->SetStringField(TEXT("source"), AbsPaths[r][c]);
					J->SetField(TEXT("sourceSize"), IntArray({ Src[r][c].W, Src[r][c].H }));
					J->SetField(TEXT("rect"), IntArray({ CX, CY, S.W, S.H }));
					CellsJson.Add(MakeShared<FJsonValueObject>(J));
				}
				Y += RowH[r] + CellLabelH + Gap;
			}

			TArray64<uint8> Png;
			if (!EW::EncodePng(Img.Px, W, H, Png)) { return FEWResult::Error(TEXT("ENCODE_FAILED"), TEXT("PNG encoding failed.")); }
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Out), /*Tree*/ true);
			if (!FFileHelper::SaveArrayToFile(Png, *Out)) { return FEWResult::Error(TEXT("WRITE_FAILED"), FString::Printf(TEXT("Could not write %s."), *Out)); }

			// Read back: the file decodes, has the planned size, and each cell holds its scaled source image.
			FImg Back;
			FString Err;
			TArray<FString> Problems;
			if (!ReadPng(Out, Back, Err)) { Problems.Add(Err); }
			else if (Back.W != W || Back.H != H) { Problems.Add(FString::Printf(TEXT("output is %dx%d, expected %dx%d"), Back.W, Back.H, W, H)); }
			else
			{
				for (const FCell& C : Cells)
				{
					const FImg& S = Scaled[C.Row][C.Col];
					for (int32 FY : { 0, S.H / 2, S.H - 1 })
					{
						if (FMemory::Memcmp(&Back.Px[(C.Y + FY) * W + C.X], &S.Px[FY * S.W], S.W * sizeof(FColor)) != 0)
						{
							Problems.Add(FString::Printf(TEXT("cell %d,%d pixels differ at row %d"), C.Row, C.Col, FY));
							break;
						}
					}
				}
			}
			if (Problems.Num())
			{
				IFileManager::Get().Delete(*Out);
				return FEWResult::Error(TEXT("READBACK_FAILED"), TEXT("The written image did not read back correctly: ") + FString::Join(Problems, TEXT("; ")));
			}
			TArray<FString> Texts = Cols;
			Texts.Add(Title);
			for (const auto& Row : Rows) { Texts.Add(Row.Key); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("out"), Out);
			Data->SetField(TEXT("size"), IntArray({ W, H }));
			Data->SetArrayField(TEXT("cells"), CellsJson);
			Data->SetStringField(TEXT("unsupportedChars"), UnsupportedChars(Texts));
			Data->SetNumberField(TEXT("bytes"), (double)Png.Num());
			FEWResult Res = FEWResult::Ok(Data);
			if (!Data->GetStringField(TEXT("unsupportedChars")).IsEmpty())
			{
				Res.Warnings.Add(TEXT("Some label characters are not in the built-in font and were drawn as '?': ") + Data->GetStringField(TEXT("unsupportedChars")));
			}
			return Res;
		});
}
