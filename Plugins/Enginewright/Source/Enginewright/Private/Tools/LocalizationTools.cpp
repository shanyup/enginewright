// Localization: String Tables (asset tables and tables registered from code / CSV), comparing a source table
// with a translation table, and switching the editor's language to preview translated text.
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"
#include "EWLocPo.h"

#include "AssetToolsModule.h"
#include "Factories/StringTableFactory.h"
#include "IAssetTools.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Internationalization/TextLocalizationResource.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

using namespace EWLoc;

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	TArray<FString> TableIds()
	{
		TArray<FString> Out;
		FStringTableRegistry::Get().EnumerateStringTables([&Out](const FName& Id, const FStringTableConstRef&) { Out.Add(Id.ToString()); return true; });
		return Out;
	}

	/** A String Table by registry id (e.g. ST_Events, or the asset's full path) or by asset path (/Game/Text/ST_UI). */
	FStringTableConstPtr FindTable(const FString& Key, FName& OutId, FString& OutErr, TArray<FString>& OutSug)
	{
		if (Key.IsEmpty()) { OutErr = TEXT("table is required."); return nullptr; }
		FStringTableRegistry& Reg = FStringTableRegistry::Get();
		if (FStringTableConstPtr T = Reg.FindStringTable(FName(*Key))) { OutId = FName(*Key); return T; }
		if (Key.StartsWith(TEXT("/")))
		{
			FString Resolved, Err;
			UObject* Obj = EW::ResolveObject(Key, false, Resolved, Err);
			if (UStringTable* ST = Cast<UStringTable>(Obj)) { OutId = ST->GetStringTableId(); return ST->GetStringTable(); }
			OutErr = Obj ? FString::Printf(TEXT("'%s' is a %s, not a String Table."), *Resolved, *Obj->GetClass()->GetName()) : Err;
			return nullptr;
		}
		OutErr = FString::Printf(TEXT("No String Table '%s' is registered (asset tables register when loaded; pass the asset path)."), *Key);
		OutSug = EW::Suggest(Key, TableIds());
		return nullptr;
	}

	TSharedRef<FJsonObject> Entry(const FString& Key, const FString& Source, const FString& Extra = FString(), const FString& ExtraName = FString())
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("key"), Key);
		J->SetStringField(TEXT("source"), Source);
		if (!ExtraName.IsEmpty()) { J->SetStringField(ExtraName, Extra); }
		return J;
	}

	/** A translated entry from a String Table, a .po file or a .locres file. */
	struct FTransEntry
	{
		FString Str, PoSource;
		bool bHasPoSource = false, bFuzzy = false, bHasHash = false;
		uint32 Hash = 0;
	};

	/**
	 * The engine's .locres reader loops for ever on a file that is not a .locres (it trusts the counts in it), which would
	 * freeze the editor. So check the file first: the new formats start with a fixed GUID; the legacy format is walked
	 * here with every length checked against the file size.
	 */
	bool LocresLooksValid(const TArray<uint8>& B)
	{
		static const uint8 Magic[16] = { 0x0E, 0x14, 0x74, 0x75, 0x67, 0x4A, 0x03, 0xFC, 0x4A, 0x15, 0x90, 0x9D, 0xC3, 0x37, 0x7F, 0x1B };
		if (B.Num() >= 17 && FMemory::Memcmp(B.GetData(), Magic, 16) == 0) { return true; }
		int64 Pos = 0;
		const int64 N = B.Num();
		auto U32 = [&](uint32& V) { if (Pos + 4 > N) { return false; } FMemory::Memcpy(&V, B.GetData() + Pos, 4); Pos += 4; return true; };
		auto Str = [&]() -> bool
		{
			uint32 Raw;
			if (!U32(Raw)) { return false; }
			const int32 Len = (int32)Raw;
			if (Len == INT32_MIN) { return false; }
			const int64 Bytes = Len >= 0 ? (int64)Len : (int64)(-Len) * 2;
			if (Pos + Bytes > N) { return false; }
			Pos += Bytes;
			return true;
		};
		uint32 Namespaces;
		if (!U32(Namespaces) || Namespaces > (uint32)N) { return false; }
		for (uint32 i = 0; i < Namespaces; ++i)
		{
			uint32 Keys;
			if (!Str() || !U32(Keys) || Keys > (uint32)N) { return false; }
			for (uint32 k = 0; k < Keys; ++k)
			{
				uint32 Hash;
				if (!Str() || !U32(Hash) || !Str()) { return false; }
			}
		}
		return Pos == N;
	}

	/** Reads the character map of a TrueType / OpenType font (formats 4 and 12), to ask whether the font draws a code point. */
	class FCmapReader
	{
	public:
		bool Load(const TArray<uint8>& InData, int32 FaceIndex)
		{
			Data = &InData;
			Tables.Reset();
			int64 Base = 0;
			uint32 Tag;
			if (!U32(0, Tag)) { return false; }
			if (Tag == 0x74746366) // 'ttcf': a collection, pick the face
			{
				uint32 Count, Off;
				if (!U32(8, Count) || FaceIndex < 0 || (uint32)FaceIndex >= Count || !U32(12 + 4 * FaceIndex, Off)) { return false; }
				Base = Off;
			}
			else if (Tag != 0x00010000 && Tag != 0x4F54544F && Tag != 0x74727565) { return false; }
			uint32 NumTables;
			if (!U16(Base + 4, NumTables)) { return false; }
			int64 Cmap = -1;
			for (uint32 i = 0; i < NumTables; ++i)
			{
				uint32 T, Off;
				if (U32(Base + 12 + 16 * i, T) && T == 0x636D6170 && U32(Base + 12 + 16 * i + 8, Off)) { Cmap = Off; break; }
			}
			uint32 NumSub;
			if (Cmap < 0 || !U16(Cmap + 2, NumSub)) { return false; }
			for (uint32 i = 0; i < NumSub; ++i)
			{
				uint32 Off, Format;
				if (!U32(Cmap + 4 + 8 * i + 4, Off) || !U16(Cmap + Off, Format)) { continue; }
				if (Format == 4 || Format == 12) { Tables.AddUnique(Cmap + Off); }
			}
			return Tables.Num() > 0;
		}

		bool Has(uint32 Cp) const
		{
			for (int64 T : Tables)
			{
				uint32 Format;
				if (!U16(T, Format)) { continue; }
				if (Format == 12)
				{
					uint32 Groups;
					if (!U32(T + 12, Groups)) { continue; }
					for (uint32 g = 0; g < Groups; ++g)
					{
						uint32 Start, End, Glyph;
						if (!U32(T + 16 + 12 * (int64)g, Start) || !U32(T + 20 + 12 * (int64)g, End) || !U32(T + 24 + 12 * (int64)g, Glyph)) { break; }
						if (Cp >= Start && Cp <= End) { if (Glyph + (Cp - Start) != 0) { return true; } break; }
					}
				}
				else if (Cp <= 0xFFFF)
				{
					uint32 SegX2;
					if (!U16(T + 6, SegX2)) { continue; }
					for (uint32 i = 0; i < SegX2; i += 2)
					{
						uint32 End, Start, Delta, RangeOff;
						if (!U16(T + 14 + i, End)) { break; }
						if (Cp > End) { continue; }
						if (!U16(T + 16 + SegX2 + i, Start) || Cp < Start) { break; }
						if (!U16(T + 16 + 2 * (int64)SegX2 + i, Delta) || !U16(T + 16 + 3 * (int64)SegX2 + i, RangeOff)) { break; }
						uint32 Gid = 0;
						if (RangeOff == 0) { Gid = (Cp + Delta) & 0xFFFF; }
						else if (U16(T + 16 + 3 * (int64)SegX2 + i + RangeOff + 2 * (int64)(Cp - Start), Gid) && Gid != 0) { Gid = (Gid + Delta) & 0xFFFF; }
						if (Gid != 0) { return true; }
						break;
					}
				}
			}
			return false;
		}

	private:
		bool U16(int64 O, uint32& V) const
		{
			if (!Data || O < 0 || O + 2 > Data->Num()) { return false; }
			V = ((uint32)(*Data)[(int32)O] << 8) | (*Data)[(int32)O + 1];
			return true;
		}
		bool U32(int64 O, uint32& V) const
		{
			uint32 A, B;
			if (!U16(O, A) || !U16(O + 2, B)) { return false; }
			V = (A << 16) | B;
			return true;
		}
		const TArray<uint8>* Data = nullptr;
		TArray<int64> Tables;
	};

	/** The letters a culture needs (basic Latin plus its own letters), by language code. */
	const TMap<FString, FString>& CultureChars()
	{
		static const TMap<FString, FString> M = {
		{ TEXT("tr"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c7\u011e\u0130\u00d6\u015e\u00dc\u00e7\u011f\u0131\u00f6\u015f\u00fc") },
		{ TEXT("de"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c4\u00d6\u00dc\u00e4\u00f6\u00fc\u00df") },
		{ TEXT("fr"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c0\u00c2\u00c6\u00c7\u00c8\u00c9\u00ca\u00cb\u00ce\u00cf\u00d4\u0152\u00d9\u00db\u00dc\u0178\u00e0\u00e2\u00e6\u00e7\u00e8\u00e9\u00ea\u00eb\u00ee\u00ef\u00f4\u0153\u00f9\u00fb\u00fc\u00ff") },
		{ TEXT("es"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c1\u00c9\u00cd\u00d1\u00d3\u00da\u00dc\u00e1\u00e9\u00ed\u00f1\u00f3\u00fa\u00fc\u00bf\u00a1") },
		{ TEXT("pt"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c1\u00c2\u00c3\u00c0\u00c7\u00c9\u00ca\u00cd\u00d3\u00d4\u00d5\u00da\u00e1\u00e2\u00e3\u00e0\u00e7\u00e9\u00ea\u00ed\u00f3\u00f4\u00f5\u00fa") },
		{ TEXT("it"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c0\u00c8\u00c9\u00cc\u00d2\u00d9\u00e0\u00e8\u00e9\u00ec\u00f2\u00f9") },
		{ TEXT("pl"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u0104\u0106\u0118\u0141\u0143\u00d3\u015a\u0179\u017b\u0105\u0107\u0119\u0142\u0144\u00f3\u015b\u017a\u017c") },
		{ TEXT("cs"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c1\u010c\u010e\u00c9\u011a\u00cd\u0147\u00d3\u0158\u0160\u0164\u00da\u016e\u00dd\u017d\u00e1\u010d\u010f\u00e9\u011b\u00ed\u0148\u00f3\u0159\u0161\u0165\u00fa\u016f\u00fd\u017e") },
		{ TEXT("sv"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c5\u00c4\u00d6\u00e5\u00e4\u00f6") },
		{ TEXT("da"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c6\u00d8\u00c5\u00e6\u00f8\u00e5") },
		{ TEXT("nb"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c6\u00d8\u00c5\u00e6\u00f8\u00e5") },
		{ TEXT("no"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c6\u00d8\u00c5\u00e6\u00f8\u00e5") },
		{ TEXT("fi"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c4\u00d6\u00c5\u00e4\u00f6\u00e5") },
		{ TEXT("hu"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u00c1\u00c9\u00cd\u00d3\u00d6\u0150\u00da\u00dc\u0170\u00e1\u00e9\u00ed\u00f3\u00f6\u0151\u00fa\u00fc\u0171") },
		{ TEXT("ro"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\u0102\u00c2\u00ce\u0218\u021a\u0103\u00e2\u00ee\u0219\u021b") },
		{ TEXT("ru"), TEXT("\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0401\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f\u0451") },
		{ TEXT("uk"), TEXT("\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f\u0490\u0404\u0406\u0407\u0491\u0454\u0456\u0457") },
		{ TEXT("el"), TEXT("\u0391\u0392\u0393\u0394\u0395\u0396\u0397\u0398\u0399\u039a\u039b\u039c\u039d\u039e\u039f\u03a0\u03a1\u03a3\u03a4\u03a5\u03a6\u03a7\u03a8\u03a9\u03b1\u03b2\u03b3\u03b4\u03b5\u03b6\u03b7\u03b8\u03b9\u03ba\u03bb\u03bc\u03bd\u03be\u03bf\u03c0\u03c1\u03c2\u03c3\u03c4\u03c5\u03c6\u03c7\u03c8\u03c9\u03c2\u0386\u0388\u0389\u038a\u038c\u038e\u038f\u03ac\u03ad\u03ae\u03af\u03cc\u03cd\u03ce") },
		{ TEXT("en"), TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") },
		};
		return M;
	}

	FString CodePointName(uint32 Cp) { return FString::Printf(TEXT("U+%04X"), Cp); }

	FString CodePointText(uint32 Cp)
	{
		FString S;
		if (Cp >= 0x10000) { Cp -= 0x10000; S.AppendChar((TCHAR)(0xD800 + (Cp >> 10))); S.AppendChar((TCHAR)(0xDC00 + (Cp & 0x3FF))); }
		else { S.AppendChar((TCHAR)Cp); }
		return S;
	}

	/** Distinct code points of a string (surrogate pairs joined), without spaces and control characters, in order. */
	TArray<uint32> CodePoints(const FString& S)
	{
		TArray<uint32> Out;
		for (int32 i = 0; i < S.Len(); ++i)
		{
			uint32 Cp = (uint32)S[i];
			if (Cp >= 0xD800 && Cp < 0xDC00 && i + 1 < S.Len() && (uint32)S[i + 1] >= 0xDC00 && (uint32)S[i + 1] < 0xE000)
			{
				Cp = 0x10000 + ((Cp - 0xD800) << 10) + ((uint32)S[++i] - 0xDC00);
			}
			if (Cp <= 0x20 || (Cp >= 0x7F && Cp < 0xA0)) { continue; }
			Out.AddUnique(Cp);
		}
		return Out;
	}

	/** Absolute path of a file argument; relative paths start at the project folder. */
	FString ResolveFile(const FString& P)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), P);
	}
}

void RegisterLocalizationTools(FEWRegistry& R)
{
	R.Register(TEXT("stringtable_list"),
		TEXT("List the registered String Tables: asset tables that are loaded and tables registered from code or CSV (LOCTABLE_FROMFILE). Gives id, namespace, entry count and the asset path for asset tables."),
		TEXT(R"({"type":"object","properties":{"filter":{"type":"string","description":"Wildcard on the id, e.g. ST_*."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Filter = EW::Str(Args, TEXT("filter"));
			TArray<TSharedPtr<FJsonValue>> List;
			FStringTableRegistry::Get().EnumerateStringTables([&](const FName& Id, const FStringTableConstRef& T)
			{
				if (!Filter.IsEmpty() && !Id.ToString().MatchesWildcard(Filter)) { return true; }
				int32 N = 0;
				T->EnumerateSourceStrings([&N](const FString&, const FString&) { ++N; return true; });
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("id"), Id.ToString());
				J->SetStringField(TEXT("namespace"), T->GetNamespace());
				J->SetNumberField(TEXT("entries"), N);
				if (UStringTable* A = T->GetOwnerAsset()) { J->SetStringField(TEXT("asset"), A->GetOutermost()->GetName()); }
				J->SetBoolField(TEXT("internal"), T->IsInternal());
				List.Add(MakeShared<FJsonValueObject>(J));
				return true;
			});
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("tables"), List);
			Data->SetNumberField(TEXT("count"), List.Num());
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("stringtable_get"),
		TEXT("Read entries of a String Table (registry id or asset path): key, source string and, when the current language translates it, the display string. Filter by keys or a key wildcard."),
		TEXT(R"({"type":"object","properties":{
			"table":{"type":"string","description":"Registry id (ST_Events) or asset path (/Game/Text/ST_UI)."},
			"keys":{"type":"array","items":{"type":"string"},"description":"Only these keys; default every entry."},
			"match":{"type":"string","description":"Key wildcard, e.g. Event.Loki*."},
			"limit":{"type":"integer","description":"Default 200."}},
			"required":["table"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FName Id; FString Err; TArray<FString> Sug;
			FStringTableConstPtr T = FindTable(EW::Str(Args, TEXT("table")), Id, Err, Sug);
			if (!T.IsValid()) { return Bad(TEXT("TABLE_NOT_FOUND"), Err, Sug); }
			const TArray<FString> Keys = EW::StrList(Args, TEXT("keys"));
			const FString Match = EW::Str(Args, TEXT("match"));
			const int32 Limit = FMath::Clamp((int32)EW::Num(Args, TEXT("limit"), 200), 1, 5000);
			TArray<TSharedPtr<FJsonValue>> Out;
			TArray<FString> AllKeys, Missing;
			int32 Total = 0;
			T->EnumerateSourceStrings([&](const FString& Key, const FString& Source)
			{
				AllKeys.Add(Key);
				if (Keys.Num() && !Keys.Contains(Key)) { return true; }
				if (!Match.IsEmpty() && !Key.MatchesWildcard(Match)) { return true; }
				++Total;
				if (Out.Num() >= Limit) { return true; }
				TSharedRef<FJsonObject> J = Entry(Key, Source);
				if (FStringTableEntryConstPtr E = T->FindEntry(FTextKey(Key)))
				{
					if (FTextConstDisplayStringPtr D = E->GetDisplayString()) { if (*D != Source) { J->SetStringField(TEXT("display"), *D); } }
				}
				Out.Add(MakeShared<FJsonValueObject>(J));
				return true;
			});
			for (const FString& K : Keys) { if (!AllKeys.Contains(K)) { Missing.Add(K); } }
			if (Keys.Num() && Missing.Num() == Keys.Num()) { return Bad(TEXT("KEY_NOT_FOUND"), FString::Printf(TEXT("None of the keys are in %s."), *Id.ToString()), EW::Suggest(Keys[0], AllKeys)); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("id"), Id.ToString());
			Data->SetStringField(TEXT("namespace"), T->GetNamespace());
			Data->SetNumberField(TEXT("tableEntries"), AllKeys.Num());
			Data->SetNumberField(TEXT("matched"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Out.Num());
			Data->SetArrayField(TEXT("entries"), Out);
			if (Missing.Num()) { TArray<TSharedPtr<FJsonValue>> M; for (const FString& K : Missing) { M.Add(MakeShared<FJsonValueString>(K)); } Data->SetArrayField(TEXT("missingKeys"), M); }
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("stringtable_set"),
		TEXT("Create a String Table asset or change its entries: set (key -> source string), remove keys, namespace. Only asset tables can be edited; tables loaded from CSV by code are read-only here (edit the CSV). Saves and reads every entry back."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string","description":"String Table asset path, e.g. /Game/Text/ST_UI."},
			"create":{"type":"boolean","description":"Create the asset when it does not exist (default false)."},
			"namespace":{"type":"string","description":"String Table namespace (set on create or changed)."},
			"entries":{"type":"object","description":"Key -> source string.","additionalProperties":{"type":"string"}},
			"remove":{"type":"array","items":{"type":"string"},"description":"Keys to remove."},
			"save":{"type":"boolean","description":"Default true."}},
			"required":["path"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Path = EW::Str(Args, TEXT("path"));
			FString Folder, Name, Err;
			if (!EW::SplitAssetPath(Path, Folder, Name, Err)) { return Bad(TEXT("INVALID_PATH"), Err); }
			const TSharedPtr<FJsonObject>* Entries = nullptr;
			Args->TryGetObjectField(TEXT("entries"), Entries);
			const TArray<FString> Remove = EW::StrList(Args, TEXT("remove"));
			// Validate everything before changing anything.
			if (Entries && Entries->IsValid())
			{
				for (const auto& KV : (*Entries)->Values)
				{
					FString V;
					if (FString(*KV.Key).TrimStartAndEnd().IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Entry keys cannot be empty.")); }
					if (!KV.Value.IsValid() || KV.Value->Type != EJson::String || !KV.Value->TryGetString(V)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Entry '%s' must be a string."), *FString(*KV.Key))); }
				}
			}

			UStringTable* ST = nullptr;
			bool bCreated = false;
			if (EW::AssetPathTaken(Folder / Name))
			{
				FString Resolved;
				UObject* Obj = EW::ResolveObject(Path, false, Resolved, Err);
				ST = Cast<UStringTable>(Obj);
				if (!ST) { return Bad(TEXT("NOT_A_STRING_TABLE"), Obj ? FString::Printf(TEXT("'%s' is a %s, not a String Table."), *Resolved, *Obj->GetClass()->GetName()) : Err); }
			}
			else
			{
				if (!EW::Bool(Args, TEXT("create"), false)) { return Bad(TEXT("TABLE_NOT_FOUND"), FString::Printf(TEXT("'%s' does not exist. Pass create: true to make it."), *Path)); }
				IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
				ST = Cast<UStringTable>(AT.CreateAsset(Name, Folder, UStringTable::StaticClass(), NewObject<UStringTableFactory>()));
				if (!ST) { return Bad(TEXT("CREATE_FAILED"), TEXT("The engine could not create the String Table (check the folder path).")); }
				bCreated = true;
			}
			FStringTableRef T = ST->GetMutableStringTable();
			for (const FString& K : Remove)
			{
				FString Old;
				if (!T->GetSourceString(FTextKey(K), Old)) { return Bad(TEXT("KEY_NOT_FOUND"), FString::Printf(TEXT("Cannot remove '%s': not in the table."), *K)); }
			}
			ST->Modify();
			if (Args->HasField(TEXT("namespace"))) { T->SetNamespace(FTextKey(EW::Str(Args, TEXT("namespace")))); }
			int32 Set = 0;
			if (Entries && Entries->IsValid())
			{
				for (const auto& KV : (*Entries)->Values) { T->SetSourceString(FTextKey(FString(*KV.Key)), KV.Value->AsString(), FString()); ++Set; }
			}
			for (const FString& K : Remove) { T->RemoveSourceString(FTextKey(K)); }
			ST->PostEditChange();
			ST->MarkPackageDirty();

			// Read back.
			TArray<FString> Wrong;
			if (Entries && Entries->IsValid())
			{
				for (const auto& KV : (*Entries)->Values)
				{
					FString Got;
					if (!T->GetSourceString(FTextKey(FString(*KV.Key)), Got) || Got != KV.Value->AsString()) { Wrong.Add(FString(*KV.Key)); }
				}
			}
			for (const FString& K : Remove) { FString Got; if (T->GetSourceString(FTextKey(K), Got)) { Wrong.Add(K); } }
			if (Wrong.Num()) { return Bad(TEXT("READBACK_MISMATCH"), FString::Printf(TEXT("Entries did not read back as written: %s"), *FString::Join(Wrong, TEXT(", ")))); }

			bool bSaved = false, bDeferred = false;
			FEWResult Res = FEWResult::Ok(MakeShared<FJsonObject>());
			if (EW::Bool(Args, TEXT("save"), true))
			{
				bSaved = EW::SaveOrDefer(ST->GetOutermost(), bDeferred);
				if (!bSaved) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
			}
			int32 N = 0;
			T->EnumerateSourceStrings([&N](const FString&, const FString&) { ++N; return true; });
			Res.Data->SetStringField(TEXT("path"), ST->GetOutermost()->GetName());
			Res.Data->SetStringField(TEXT("id"), ST->GetStringTableId().ToString());
			Res.Data->SetStringField(TEXT("namespace"), T->GetNamespace());
			Res.Data->SetBoolField(TEXT("created"), bCreated);
			Res.Data->SetNumberField(TEXT("set"), Set);
			Res.Data->SetNumberField(TEXT("removed"), Remove.Num());
			Res.Data->SetNumberField(TEXT("entries"), N);
			Res.Data->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("stringtable_compare"),
		TEXT("Compare a source with a translation, read-only: keys missing from the translation, extra keys, empty translations, translations identical to the source (likely untranslated), and format arguments {x} or rich text tags <x> that differ between source and translation. ")
		TEXT("Source: a String Table (registry id or asset path) or a CSV file (sourceCsv, Key,SourceString columns like LOCTABLE_FROMFILE). Translation: a second String Table, or a translated file: po (Game.po of the localization dashboard, entries matched by namespace + key) or locres (compiled Game.locres). ")
		TEXT("A po or locres input also reports translations made for an older source text (sourceChanged) and, for po, entries marked fuzzy. Pass exactly one source and one translation. Relative file paths start at the project folder."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string","description":"Source String Table (path or asset name)."},
			"sourceCsv":{"type":"string","description":"Source from a CSV file (Key,SourceString[,Comment]) instead of a String Table."},
			"translation":{"type":"string","description":"Translated String Table to check against the source."},
			"po":{"type":"string","description":"Translated .po file (e.g. Content/Localization/Game/tr/Game.po)."},
			"locres":{"type":"string","description":"Compiled .locres file (e.g. Content/Localization/Game/tr/Game.locres)."},
			"namespace":{"type":"string","description":"Namespace of the entries in the po/locres file. Default: the namespace of the source String Table, or the CSV file name for sourceCsv. '*' = any namespace."},
			"ignoreSame":{"type":"array","items":{"type":"string"},"description":"Key wildcards allowed to equal the source (names, numbers)."},
			"limit":{"type":"integer","description":"Max listed per category, default 100."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString SourceArg = EW::Str(Args, TEXT("source")), CsvArg = EW::Str(Args, TEXT("sourceCsv"));
			const FString TransArg = EW::Str(Args, TEXT("translation")), PoArg = EW::Str(Args, TEXT("po")), LocresArg = EW::Str(Args, TEXT("locres"));
			if (SourceArg.IsEmpty() == CsvArg.IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass exactly one of source (a String Table) and sourceCsv (a CSV file).")); }
			if ((int32)!TransArg.IsEmpty() + (int32)!PoArg.IsEmpty() + (int32)!LocresArg.IsEmpty() != 1) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass exactly one of translation (a String Table), po and locres.")); }
			if (!PoArg.IsEmpty() && !PoArg.EndsWith(TEXT(".po"), ESearchCase::IgnoreCase)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("po must be a .po file.")); }
			if (!LocresArg.IsEmpty() && !LocresArg.EndsWith(TEXT(".locres"), ESearchCase::IgnoreCase)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("locres must be a .locres file.")); }
			const TArray<FString> IgnoreSame = EW::StrList(Args, TEXT("ignoreSame"));
			const int32 Limit = FMath::Clamp((int32)EW::Num(Args, TEXT("limit"), 100), 1, 5000);

			// Source: key -> source string, in file / table order.
			FString Err, SourceName, SrcNamespace;
			FName SId, TId;
			TArray<FString> Sug;
			TMap<FString, FString> Src;
			TArray<FString> SrcOrder;
			if (!SourceArg.IsEmpty())
			{
				FStringTableConstPtr S = FindTable(SourceArg, SId, Err, Sug);
				if (!S.IsValid()) { return Bad(TEXT("TABLE_NOT_FOUND"), TEXT("source: ") + Err, Sug); }
				S->EnumerateSourceStrings([&](const FString& K, const FString& V) { Src.Add(K, V); SrcOrder.Add(K); return true; });
				SourceName = SId.ToString();
				SrcNamespace = S->GetNamespace();
			}
			else
			{
				FString Text;
				const FString Path = ResolveFile(CsvArg);
				if (!FFileHelper::LoadFileToString(Text, *Path)) { return Bad(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("sourceCsv: cannot read '%s'."), *Path)); }
				FStringTableRef Csv = FStringTable::NewStringTable();
				if (!Csv->ImportStringsFromCSVString(Text, *Path)) { return Bad(TEXT("INVALID_FILE"), FString::Printf(TEXT("sourceCsv: '%s' is not a String Table CSV (needs Key and SourceString columns)."), *Path)); }
				Csv->EnumerateSourceStrings([&](const FString& K, const FString& V) { Src.Add(K, V); SrcOrder.Add(K); return true; });
				SourceName = Path;
				SrcNamespace = FPaths::GetBaseFilename(Path);   // a CSV table loaded with LOCTABLE_FROMFILE usually takes the file name as its namespace
			}

			// Translation: key -> entry.
			TMap<FString, FTransEntry> Tr;
			FString TransName, Kind;
			TArray<FString> Notes;
			FString NsUsed;       // namespace the po/locres entries were read from ('*' = any)
			bool bNsUsed = false;
			bool bNsAny = false;  // the namespace argument was '*'
			TMap<FString, int32> NsSourceCounts; // per namespace: how many of the source keys it has entries for
			if (!TransArg.IsEmpty())
			{
				FStringTableConstPtr T = FindTable(TransArg, TId, Err, Sug);
				if (!T.IsValid()) { return Bad(TEXT("TABLE_NOT_FOUND"), TEXT("translation: ") + Err, Sug); }
				if (!SourceArg.IsEmpty() && SId == TId) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("source and translation are the same table.")); }
				T->EnumerateSourceStrings([&](const FString& K, const FString& V) { FTransEntry E; E.Str = V; Tr.Add(K, E); return true; });
				TransName = TId.ToString();
				Kind = TEXT("table");
			}
			else
			{
				const FString Path = ResolveFile(PoArg.IsEmpty() ? LocresArg : PoArg);
				if (!IFileManager::Get().FileExists(*Path)) { return Bad(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("'%s' does not exist."), *Path)); }
				const FString NsArg = Args->HasField(TEXT("namespace")) ? EW::Str(Args, TEXT("namespace")) : SrcNamespace;
				NsUsed = NsArg;
				bNsUsed = true;
				const bool bAnyNs = NsArg == TEXT("*");
				bNsAny = bAnyNs;
				TMap<FString, FTransEntry> Any;      // every namespace, first entry of a key wins
				TMap<FString, FTransEntry> InNs;     // only NsArg
				TSet<FString> CountedNsKeys;         // "namespace<US>key", counted once even when a po lists an entry twice
				auto CountNsKey = [&Src, &NsSourceCounts, &CountedNsKeys](const FString& Ns, const FString& Key)
				{
					FString Id = Ns;
					Id += TEXT('\x1F');
					Id += Key;
					if (Src.Contains(Key) && !CountedNsKeys.Contains(Id)) { CountedNsKeys.Add(Id); ++NsSourceCounts.FindOrAdd(Ns); }
				};
				int32 NoContext = 0, Total = 0;
				if (!PoArg.IsEmpty())
				{
					FString Text;
					if (!FFileHelper::LoadFileToString(Text, *Path)) { return Bad(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("cannot read '%s'."), *Path)); }
					TArray<FPoItem> Items;
					ParsePo(Text, Items);
					for (const FPoItem& It : Items)
					{
						if (It.Ctxt.IsEmpty() && It.Id.IsEmpty()) { continue; } // header
						FString Ns, Key;
						SplitContext(It, Ns, Key);
						if (Key.IsEmpty()) { ++NoContext; continue; }
						++Total;
						FTransEntry E; E.Str = It.Str; E.PoSource = It.Id; E.bHasPoSource = true; E.bFuzzy = It.bFuzzy;
						if (!Any.Contains(Key)) { Any.Add(Key, E); }
						if (Ns == NsArg && !InNs.Contains(Key)) { InNs.Add(Key, E); }
						CountNsKey(Ns, Key);
					}
					Kind = TEXT("po");
				}
				else
				{
					TArray<uint8> Bytes;
					if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { return Bad(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("cannot read '%s'."), *Path)); }
					if (!LocresLooksValid(Bytes)) { return Bad(TEXT("INVALID_FILE"), FString::Printf(TEXT("'%s' is not a readable .locres file."), *Path)); }
					FTextLocalizationResource Res;
					if (!Res.LoadFromFile(Path, 0)) { return Bad(TEXT("INVALID_FILE"), FString::Printf(TEXT("'%s' is not a readable .locres file."), *Path)); }
					for (const TPair<FTextId, FTextLocalizationResource::FEntry>& KV : Res.Entries)
					{
						++Total;
						const FString Key = KV.Key.GetKey().ToString(), Ns = KV.Key.GetNamespace().ToString();
						FTransEntry E; E.Str = KV.Value.LocalizedString.IsValid() ? *KV.Value.LocalizedString : FString(); E.bHasHash = true; E.Hash = KV.Value.SourceStringHash;
						if (!Any.Contains(Key)) { Any.Add(Key, E); }
						if (Ns == NsArg && !InNs.Contains(Key)) { InNs.Add(Key, E); }
						CountNsKey(Ns, Key);
					}
					Kind = TEXT("locres");
				}
				if (Total == 0) { return Bad(TEXT("INVALID_FILE"), FString::Printf(TEXT("'%s' has no translated entries."), *Path)); }
				if (bAnyNs || (InNs.Num() == 0 && NsArg.IsEmpty())) { Tr = Any; }
				else if (InNs.Num() == 0)
				{
					Tr = Any;
					Notes.Add(FString::Printf(TEXT("No entry in namespace '%s' in %s; keys were matched in any namespace. Pass namespace to choose one."), *NsArg, *FPaths::GetCleanFilename(Path)));
				}
				else { Tr = InNs; }
				if (NoContext) { Notes.Add(FString::Printf(TEXT("%d po entries have no msgctxt/Key and were skipped."), NoContext)); }
				TransName = Path;
			}

			TArray<TSharedPtr<FJsonValue>> Missing, Extra, Empty, Same, Format, Changed, Fuzzy;
			int32 NMissing = 0, NExtra = 0, NEmpty = 0, NSame = 0, NFormat = 0, NChanged = 0, NFuzzy = 0;
			auto Add = [Limit](TArray<TSharedPtr<FJsonValue>>& L, int32& N, TSharedRef<FJsonObject> J) { ++N; if (L.Num() < Limit) { L.Add(MakeShared<FJsonValueObject>(J)); } };
			for (const FString& K : SrcOrder)
			{
				const FString& V = Src[K];
				const FTransEntry* TE = Tr.Find(K);
				if (!TE) { Add(Missing, NMissing, Entry(K, V)); continue; }
				const FString& TV = TE->Str;
				if (TE->bFuzzy) { Add(Fuzzy, NFuzzy, Entry(K, V, TV, TEXT("translation"))); }
				if (TV.TrimStartAndEnd().IsEmpty() && !V.TrimStartAndEnd().IsEmpty()) { Add(Empty, NEmpty, Entry(K, V)); continue; }
				const bool bIgnored = IgnoreSame.ContainsByPredicate([&K](const FString& W) { return K.MatchesWildcard(W); });
				if (TV == V && !bIgnored && V.Len() > 1 && !V.IsNumeric()) { Add(Same, NSame, Entry(K, V)); }
				if ((TE->bHasPoSource && TE->PoSource != V) || (TE->bHasHash && TE->Hash != FTextLocalizationResource::HashString(V)))
				{
					TSharedRef<FJsonObject> J = Entry(K, V, TV, TEXT("translation"));
					if (TE->bHasPoSource) { J->SetStringField(TEXT("translatedFrom"), TE->PoSource); }
					Add(Changed, NChanged, J);
				}
				const TArray<FString> A = Tokens(V), B = Tokens(TV);
				if (A != B)
				{
					TSharedRef<FJsonObject> J = Entry(K, V, TV, TEXT("translation"));
					J->SetStringField(TEXT("sourceTokens"), FString::Join(A, TEXT(" ")));
					J->SetStringField(TEXT("translationTokens"), FString::Join(B, TEXT(" ")));
					Add(Format, NFormat, J);
				}
			}
			for (const TPair<FString, FTransEntry>& KV : Tr) { if (!Src.Contains(FString(*KV.Key))) { Add(Extra, NExtra, Entry(FString(*KV.Key), KV.Value.Str)); } }

			// The namespace guess was clearly wrong: most keys are missing and the file has them under other namespaces.
			if (bNsUsed && !bNsAny && NMissing * 2 > SrcOrder.Num())
			{
				TArray<TPair<FString, int32>> Rank;
				for (const TPair<FString, int32>& KV : NsSourceCounts)
				{
					if (KV.Key != NsUsed) { Rank.Emplace(KV.Key, KV.Value); }
				}
				Rank.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
				TArray<FString> Where;
				for (int32 i = 0; i < Rank.Num() && i < 3; ++i) { Where.Add(FString::Printf(TEXT("%s (%d keys)"), *Rank[i].Key, Rank[i].Value)); }
				if (Where.Num()) { Notes.Add(FString::Printf(TEXT("Most source keys are missing in namespace '%s'. The file has them in: %s. Pass namespace."), *NsUsed, *FString::Join(Where, TEXT(", ")))); }
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("source"), SourceName);
			Data->SetStringField(TEXT("translation"), TransName);
			Data->SetStringField(TEXT("translationKind"), Kind);
			if (bNsUsed) { Data->SetStringField(TEXT("namespace"), NsUsed); }
			Data->SetNumberField(TEXT("sourceEntries"), Src.Num());
			Data->SetNumberField(TEXT("translationEntries"), Tr.Num());
			TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
			Counts->SetNumberField(TEXT("missing"), NMissing);
			Counts->SetNumberField(TEXT("extra"), NExtra);
			Counts->SetNumberField(TEXT("empty"), NEmpty);
			Counts->SetNumberField(TEXT("sameAsSource"), NSame);
			Counts->SetNumberField(TEXT("formatMismatch"), NFormat);
			Counts->SetNumberField(TEXT("sourceChanged"), NChanged);
			Counts->SetNumberField(TEXT("fuzzy"), NFuzzy);
			Data->SetObjectField(TEXT("counts"), Counts);
			Data->SetArrayField(TEXT("missing"), Missing);
			Data->SetArrayField(TEXT("extra"), Extra);
			Data->SetArrayField(TEXT("empty"), Empty);
			Data->SetArrayField(TEXT("sameAsSource"), Same);
			Data->SetArrayField(TEXT("formatMismatch"), Format);
			Data->SetArrayField(TEXT("sourceChanged"), Changed);
			Data->SetArrayField(TEXT("fuzzy"), Fuzzy);
			Data->SetBoolField(TEXT("clean"), NMissing + NExtra + NEmpty + NFormat + NChanged + NFuzzy == 0);
			FEWResult Res = FEWResult::Ok(Data);
			Res.Warnings = Notes;
			return Res;
		});

	R.Register(TEXT("font_coverage"),
		TEXT("Check which characters a font cannot draw (they show as empty boxes, 'tofu', in the game), read-only. font is a Font asset (/Game/UI/F_Main, a Slate runtime font with its typefaces and fallback font), a Font Face asset, or a .ttf / .otf / .ttc file. ")
		TEXT("Pass culture (tr, de, fr, es, pt, it, pl, cs, sv, da, nb, fi, hu, ro, ru, uk, el, en: basic Latin or the alphabet plus the language's own letters) or chars (any text; its distinct characters are checked). ")
		TEXT("Reports every face with the characters it lacks, and 'missing': the characters no face of the font draws. A Font asset is judged over all its faces; Slate uses a sub-font only for its character ranges and cultures. For the glyph misses of a running game see game_run (glyphMisses)."),
		TEXT(R"({"type":"object","properties":{
			"font":{"type":"string","description":"Font asset path (/Game/UI/F_Main), Font Face asset path, or a .ttf / .otf / .ttc file (absolute, or relative to the project folder)."},
			"culture":{"type":"string","description":"Culture whose letters are checked, e.g. tr or tr-TR. Pass this or chars."},
			"chars":{"type":"string","description":"Characters to check, e.g. the Turkish letters. Pass this or culture."}},
			"required":["font"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString FontArg = EW::Str(Args, TEXT("font")), CultureArg = EW::Str(Args, TEXT("culture")), CharsArg = EW::Str(Args, TEXT("chars"));
			if (CultureArg.IsEmpty() == CharsArg.IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Pass exactly one of culture and chars.")); }
			FString Wanted, Lang;
			if (!CultureArg.IsEmpty())
			{
				FCulturePtr C = FInternationalization::Get().GetCulture(CultureArg);
				if (!C.IsValid())
				{
					TArray<FString> All;
					FInternationalization::Get().GetCultureNames(All);
					return Bad(TEXT("UNKNOWN_CULTURE"), FString::Printf(TEXT("'%s' is not a known culture name."), *CultureArg), EW::Suggest(CultureArg, All));
				}
				Lang = C->GetTwoLetterISOLanguageName();
				const FString* Found = CultureChars().Find(Lang);
				if (!Found)
				{
					TArray<FString> Known;
					CultureChars().GetKeys(Known);
					Known.Sort();
					return Bad(TEXT("NO_CHAR_TABLE"), FString::Printf(TEXT("No letter list for language '%s'. Pass chars with the characters to check instead."), *Lang), Known);
				}
				Wanted = *Found;
			}
			else { Wanted = CharsArg; }
			const TArray<uint32> Cps = CodePoints(Wanted);
			if (Cps.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("chars has no characters to check (spaces and control characters are skipped).")); }

			struct FFace { FString Typeface, Name, Source; TArray<uint8> Bytes; int32 Index = 0; FString Error; };
			TArray<FFace> Faces;
			FString Kind, FontName;
			auto AddEntry = [&Faces](const FString& Typeface, const FTypefaceEntry& E)
			{
				FFace F;
				F.Typeface = Typeface; F.Name = E.Name.ToString(); F.Index = E.Font.GetSubFaceIndex();
				F.Source = E.Font.GetFontFilename();
				if (const UObject* Asset = E.Font.GetFontFaceAsset()) { F.Source = Asset->GetPathName(); }
				if (FFontFaceDataConstPtr D = E.Font.GetFontFaceData()) { if (D->HasData()) { F.Bytes = D->GetData(); } }
				if (F.Bytes.Num() == 0 && !F.Source.IsEmpty() && !FFileHelper::LoadFileToArray(F.Bytes, *F.Source)) { F.Error = TEXT("font data not available"); }
				if (F.Bytes.Num() == 0 && F.Error.IsEmpty()) { F.Error = TEXT("the entry has no font"); }
				Faces.Add(MoveTemp(F));
			};
			if (FontArg.StartsWith(TEXT("/")))
			{
				FString Resolved, Err;
				UObject* Obj = EW::ResolveObject(FontArg, false, Resolved, Err);
				if (!Obj) { return Bad(TEXT("ASSET_NOT_FOUND"), Err); }
				FontName = Resolved;
				if (UFont* Font = Cast<UFont>(Obj))
				{
					if (Font->FontCacheType != EFontCacheType::Runtime) { return Bad(TEXT("UNSUPPORTED_FONT"), FString::Printf(TEXT("'%s' is an offline (texture) font; only runtime fonts with font faces can be checked."), *Resolved)); }
					const FCompositeFont* CF = Font->GetCompositeFont();
					if (!CF) { return Bad(TEXT("UNSUPPORTED_FONT"), FString::Printf(TEXT("'%s' has no typefaces."), *Resolved)); }
					for (const FTypefaceEntry& E : CF->DefaultTypeface.Fonts) { AddEntry(TEXT("default"), E); }
					for (const FTypefaceEntry& E : CF->FallbackTypeface.Typeface.Fonts) { AddEntry(TEXT("fallback"), E); }
					for (int32 i = 0; i < CF->SubTypefaces.Num(); ++i)
					{
						for (const FTypefaceEntry& E : CF->SubTypefaces[i].Typeface.Fonts) { AddEntry(FString::Printf(TEXT("sub[%d]"), i), E); }
					}
					Kind = TEXT("font");
				}
				else if (UFontFace* Face = Cast<UFontFace>(Obj))
				{
					FFace F;
					F.Typeface = TEXT("face"); F.Name = Face->GetName(); F.Source = Face->GetPathName();
					if (FFontFaceDataConstRef D = Face->GetFontFaceData(); D->HasData()) { F.Bytes = D->GetData(); }
					else { F.Error = TEXT("font data not available"); }
					Faces.Add(MoveTemp(F));
					Kind = TEXT("fontFace");
				}
				else { return Bad(TEXT("NOT_A_FONT"), FString::Printf(TEXT("'%s' is a %s, not a Font or Font Face."), *Resolved, *Obj->GetClass()->GetName())); }
			}
			else
			{
				const FString Ext = FPaths::GetExtension(FontArg).ToLower();
				if (Ext != TEXT("ttf") && Ext != TEXT("otf") && Ext != TEXT("ttc") && Ext != TEXT("otc")) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("font must be a /Game/... asset path or a .ttf / .otf / .ttc file.")); }
				const FString Path = ResolveFile(FontArg);
				FFace F;
				F.Typeface = TEXT("file"); F.Name = FPaths::GetBaseFilename(Path); F.Source = Path;
				if (!FFileHelper::LoadFileToArray(F.Bytes, *Path)) { return Bad(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("cannot read '%s'."), *Path)); }
				Faces.Add(MoveTemp(F));
				FontName = Path;
				Kind = TEXT("file");
			}
			if (Faces.Num() == 0) { return Bad(TEXT("UNSUPPORTED_FONT"), FString::Printf(TEXT("'%s' has no font faces."), *FontName)); }

			TSet<uint32> Drawn;
			TArray<TSharedPtr<FJsonValue>> FaceJson;
			int32 Readable = 0;
			for (const FFace& F : Faces)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("typeface"), F.Typeface);
				J->SetStringField(TEXT("name"), F.Name);
				J->SetStringField(TEXT("source"), F.Source);
				FCmapReader Map;
				if (!F.Error.IsEmpty()) { J->SetStringField(TEXT("error"), F.Error); }
				else if (!Map.Load(F.Bytes, F.Index)) { J->SetStringField(TEXT("error"), TEXT("not a TrueType / OpenType font with a character map")); }
				else
				{
					++Readable;
					TArray<TSharedPtr<FJsonValue>> Missing;
					for (uint32 Cp : Cps)
					{
						if (Map.Has(Cp)) { Drawn.Add(Cp); } else { Missing.Add(MakeShared<FJsonValueString>(CodePointName(Cp))); }
					}
					J->SetNumberField(TEXT("missingCount"), Missing.Num());
					J->SetArrayField(TEXT("missing"), Missing);
				}
				FaceJson.Add(MakeShared<FJsonValueObject>(J));
			}
			if (Readable == 0) { return Bad(TEXT("UNSUPPORTED_FONT"), FString::Printf(TEXT("None of the faces of '%s' could be read as TrueType / OpenType (see faces[].error)."), *FontName)); }

			TArray<TSharedPtr<FJsonValue>> Missing;
			FString MissingText;
			for (uint32 Cp : Cps)
			{
				if (Drawn.Contains(Cp)) { continue; }
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("char"), CodePointText(Cp));
				J->SetStringField(TEXT("codePoint"), CodePointName(Cp));
				Missing.Add(MakeShared<FJsonValueObject>(J));
				MissingText += CodePointText(Cp);
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("font"), FontName);
			Data->SetStringField(TEXT("kind"), Kind);
			if (!Lang.IsEmpty()) { Data->SetStringField(TEXT("language"), Lang); }
			Data->SetNumberField(TEXT("checked"), Cps.Num());
			Data->SetNumberField(TEXT("covered"), Cps.Num() - Missing.Num());
			Data->SetNumberField(TEXT("missingCount"), Missing.Num());
			Data->SetStringField(TEXT("missingText"), MissingText);
			Data->SetArrayField(TEXT("missing"), Missing);
			Data->SetArrayField(TEXT("faces"), FaceJson);
			Data->SetBoolField(TEXT("clean"), Missing.Num() == 0);
			FEWResult Res = FEWResult::Ok(Data);
			if (Readable < Faces.Num()) { Res.Warnings.Add(FString::Printf(TEXT("%d of %d faces could not be read; the result covers the others (see faces[].error)."), Faces.Num() - Readable, Faces.Num())); }
			return Res;
		});

	R.Register(TEXT("loc_culture"),
		TEXT("Read or switch the editor's current language (and culture), e.g. to preview translated text in Play In Editor or in a screenshot. Lists the cultures that have game localization data. Switching changes text everywhere in the editor until switched back. It does not reach a game started by game_run (a separate process): give game_run its own culture argument."),
		TEXT(R"({"type":"object","properties":{
			"language":{"type":"string","description":"Culture name to switch to, e.g. tr, en, de-DE. Omit to only read."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FInternationalization& I18N = FInternationalization::Get();
			const FString Before = I18N.GetCurrentLanguage()->GetName();
			const FString Want = EW::Str(Args, TEXT("language"));
			TArray<FString> Game = FTextLocalizationManager::Get().GetLocalizedCultureNames(ELocalizationLoadFlags::Game);
			if (!Want.IsEmpty())
			{
				if (!I18N.GetCulture(Want).IsValid())
				{
					TArray<FString> All;
					I18N.GetCultureNames(All);
					return Bad(TEXT("UNKNOWN_CULTURE"), FString::Printf(TEXT("'%s' is not a known culture name."), *Want), EW::Suggest(Want, All));
				}
				if (!I18N.SetCurrentLanguage(Want)) { return Bad(TEXT("NOT_APPLIED"), FString::Printf(TEXT("The engine refused language '%s'."), *Want)); }
			}
			const FString Now = I18N.GetCurrentLanguage()->GetName();
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("language"), Now);
			Data->SetStringField(TEXT("culture"), I18N.GetCurrentCulture()->GetName());
			if (!Want.IsEmpty()) { Data->SetStringField(TEXT("before"), Before); }
			TArray<TSharedPtr<FJsonValue>> G;
			for (const FString& N : Game) { G.Add(MakeShared<FJsonValueString>(N)); }
			Data->SetArrayField(TEXT("gameCultures"), G);
			FEWResult Res = FEWResult::Ok(Data);
			if (!Want.IsEmpty() && !Game.Contains(Now) && !Game.ContainsByPredicate([&Now](const FString& C) { return Now.StartsWith(C + TEXT("-")); }))
			{
				Res.Warnings.Add(FString::Printf(TEXT("No compiled game localization for '%s'; asset text stays in the source language (String Tables loaded per language by code are not affected)."), *Now));
			}
			if (!Want.IsEmpty() && Now != Want && !Now.StartsWith(Want)) { return Bad(TEXT("NOT_APPLIED"), FString::Printf(TEXT("Language is %s after asking for %s."), *Now, *Want)); }
			return Res;
		});
}
