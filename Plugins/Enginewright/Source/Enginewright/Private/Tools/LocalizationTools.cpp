// Localization: String Tables (asset tables and tables registered from code / CSV), comparing a source table
// with a translation table, and switching the editor's language to preview translated text.
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "AssetToolsModule.h"
#include "Factories/StringTableFactory.h"
#include "IAssetTools.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

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

	/** {name} style format arguments and <tag> rich text tags used by a string, sorted. */
	TArray<FString> Tokens(const FString& S)
	{
		TArray<FString> Out;
		for (int32 i = 0; i < S.Len(); ++i)
		{
			const TCHAR Open = S[i];
			if (Open != TEXT('{') && Open != TEXT('<')) { continue; }
			if (Open == TEXT('{') && i + 1 < S.Len() && S[i + 1] == TEXT('{')) { ++i; continue; } // escaped
			const TCHAR Close = Open == TEXT('{') ? TEXT('}') : TEXT('>');
			const int32 End = S.Find(FString(1, &Close), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
			if (End == INDEX_NONE || End - i > 64) { continue; }
			FString Tok = S.Mid(i, End - i + 1);
			if (Open == TEXT('<')) { int32 Sp; if (Tok.FindChar(TEXT(' '), Sp)) { Tok = Tok.Left(Sp) + TEXT(">"); } } // <tag attr="x"> -> <tag>
			Out.Add(Tok);
			i = End;
		}
		Out.Sort();
		return Out;
	}

	TSharedRef<FJsonObject> Entry(const FString& Key, const FString& Source, const FString& Extra = FString(), const FString& ExtraName = FString())
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("key"), Key);
		J->SetStringField(TEXT("source"), Source);
		if (!ExtraName.IsEmpty()) { J->SetStringField(ExtraName, Extra); }
		return J;
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
			"keys":{"type":"array","items":{"type":"string"}},
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
			"namespace":{"type":"string"},
			"entries":{"type":"object","description":"Key -> source string.","additionalProperties":{"type":"string"}},
			"remove":{"type":"array","items":{"type":"string"}},
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
					if (!KV.Value.IsValid() || !KV.Value->TryGetString(V)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Entry '%s' must be a string."), *FString(*KV.Key))); }
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
		TEXT("Compare a source String Table with a translation table (registry ids or asset paths), read-only: keys missing from the translation, extra keys, empty translations, translations identical to the source (likely untranslated), and format arguments {x} or rich text tags <x> that differ between source and translation."),
		TEXT(R"({"type":"object","properties":{
			"source":{"type":"string"},"translation":{"type":"string"},
			"ignoreSame":{"type":"array","items":{"type":"string"},"description":"Key wildcards allowed to equal the source (names, numbers)."},
			"limit":{"type":"integer","description":"Max listed per category, default 100."}},
			"required":["source","translation"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FName SId, TId; FString Err; TArray<FString> Sug;
			FStringTableConstPtr S = FindTable(EW::Str(Args, TEXT("source")), SId, Err, Sug);
			if (!S.IsValid()) { return Bad(TEXT("TABLE_NOT_FOUND"), TEXT("source: ") + Err, Sug); }
			FStringTableConstPtr T = FindTable(EW::Str(Args, TEXT("translation")), TId, Err, Sug);
			if (!T.IsValid()) { return Bad(TEXT("TABLE_NOT_FOUND"), TEXT("translation: ") + Err, Sug); }
			if (SId == TId) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("source and translation are the same table.")); }
			const TArray<FString> IgnoreSame = EW::StrList(Args, TEXT("ignoreSame"));
			const int32 Limit = FMath::Clamp((int32)EW::Num(Args, TEXT("limit"), 100), 1, 5000);

			TMap<FString, FString> Src, Tr;
			TArray<FString> SrcOrder;
			S->EnumerateSourceStrings([&](const FString& K, const FString& V) { Src.Add(K, V); SrcOrder.Add(K); return true; });
			T->EnumerateSourceStrings([&](const FString& K, const FString& V) { Tr.Add(K, V); return true; });

			TArray<TSharedPtr<FJsonValue>> Missing, Extra, Empty, Same, Format;
			int32 NMissing = 0, NExtra = 0, NEmpty = 0, NSame = 0, NFormat = 0;
			auto Add = [Limit](TArray<TSharedPtr<FJsonValue>>& L, int32& N, TSharedRef<FJsonObject> J) { ++N; if (L.Num() < Limit) { L.Add(MakeShared<FJsonValueObject>(J)); } };
			for (const FString& K : SrcOrder)
			{
				const FString& V = Src[K];
				const FString* TV = Tr.Find(K);
				if (!TV) { Add(Missing, NMissing, Entry(K, V)); continue; }
				if (TV->TrimStartAndEnd().IsEmpty() && !V.TrimStartAndEnd().IsEmpty()) { Add(Empty, NEmpty, Entry(K, V)); continue; }
				const bool bIgnored = IgnoreSame.ContainsByPredicate([&K](const FString& W) { return K.MatchesWildcard(W); });
				if (*TV == V && !bIgnored && V.Len() > 1 && !V.IsNumeric()) { Add(Same, NSame, Entry(K, V)); }
				const TArray<FString> A = Tokens(V), B = Tokens(*TV);
				if (A != B)
				{
					TSharedRef<FJsonObject> J = Entry(K, V, *TV, TEXT("translation"));
					J->SetStringField(TEXT("sourceTokens"), FString::Join(A, TEXT(" ")));
					J->SetStringField(TEXT("translationTokens"), FString::Join(B, TEXT(" ")));
					Add(Format, NFormat, J);
				}
			}
			for (const TPair<FString, FString>& KV : Tr) { if (!Src.Contains(FString(*KV.Key))) { Add(Extra, NExtra, Entry(FString(*KV.Key), KV.Value)); } }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("source"), SId.ToString());
			Data->SetStringField(TEXT("translation"), TId.ToString());
			Data->SetNumberField(TEXT("sourceEntries"), Src.Num());
			Data->SetNumberField(TEXT("translationEntries"), Tr.Num());
			TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
			Counts->SetNumberField(TEXT("missing"), NMissing);
			Counts->SetNumberField(TEXT("extra"), NExtra);
			Counts->SetNumberField(TEXT("empty"), NEmpty);
			Counts->SetNumberField(TEXT("sameAsSource"), NSame);
			Counts->SetNumberField(TEXT("formatMismatch"), NFormat);
			Data->SetObjectField(TEXT("counts"), Counts);
			Data->SetArrayField(TEXT("missing"), Missing);
			Data->SetArrayField(TEXT("extra"), Extra);
			Data->SetArrayField(TEXT("empty"), Empty);
			Data->SetArrayField(TEXT("sameAsSource"), Same);
			Data->SetArrayField(TEXT("formatMismatch"), Format);
			Data->SetBoolField(TEXT("clean"), NMissing + NExtra + NEmpty + NFormat == 0);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("loc_culture"),
		TEXT("Read or switch the editor's current language (and culture), e.g. to preview translated text in Play In Editor or in a screenshot. Lists the cultures that have game localization data. Switching changes text everywhere in the editor until switched back."),
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
