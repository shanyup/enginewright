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
#include "Internationalization/TextLocalizationResource.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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

	/** A translated entry from a String Table, a .po file or a .locres file. */
	struct FTransEntry
	{
		FString Str, PoSource;
		bool bHasPoSource = false, bFuzzy = false, bHasHash = false;
		uint32 Hash = 0;
	};

	/** One entry of a .po file (msgctxt "Namespace,Key", msgid = source text, msgstr = translation). */
	struct FPoItem { FString Ctxt, Id, Str, CommentKey; bool bFuzzy = false; };

	/** Text between the first and the last quote of a po string line, with \n \t \r \" \\ unescaped. */
	FString UnquotePo(const FString& In)
	{
		const int32 A = In.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart);
		const int32 B = In.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (A == INDEX_NONE || B <= A) { return FString(); }
		const FString Body = In.Mid(A + 1, B - A - 1);
		FString Out;
		Out.Reserve(Body.Len());
		for (int32 i = 0; i < Body.Len(); ++i)
		{
			if (Body[i] == TEXT('\\') && i + 1 < Body.Len())
			{
				const TCHAR N = Body[++i];
				Out.AppendChar(N == TEXT('n') ? TEXT('\n') : N == TEXT('t') ? TEXT('\t') : N == TEXT('r') ? TEXT('\r') : N);
			}
			else { Out.AppendChar(Body[i]); }
		}
		return Out;
	}

	void ParsePo(const FString& Text, TArray<FPoItem>& Out)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FPoItem E;
		bool bHave = false;
		int32 Cur = 0; // 1 msgctxt, 2 msgid, 3 msgstr
		auto Flush = [&]() { if (bHave) { Out.Add(E); } E = FPoItem(); bHave = false; Cur = 0; };
		for (FString L : Lines)
		{
			L.TrimStartAndEndInline();
			if (L.IsEmpty()) { Flush(); continue; }
			if (L.StartsWith(TEXT("#~"))) { continue; } // obsolete entry
			if (L.StartsWith(TEXT("#")))
			{
				if (Cur == 3) { Flush(); }
				if (L.StartsWith(TEXT("#,")) && L.Contains(TEXT("fuzzy"))) { E.bFuzzy = true; }
				if (L.StartsWith(TEXT("#.")))
				{
					FString After;
					if (L.Split(TEXT("Key:"), nullptr, &After)) { E.CommentKey = After.TrimStartAndEnd(); }
				}
				continue;
			}
			if (L.StartsWith(TEXT("msgctxt"))) { if (Cur == 3) { Flush(); } Cur = 1; bHave = true; E.Ctxt = UnquotePo(L); }
			else if (L.StartsWith(TEXT("msgid_plural")) || L.StartsWith(TEXT("msgstr["))) { Cur = 0; }
			else if (L.StartsWith(TEXT("msgid"))) { if (Cur == 3) { Flush(); } Cur = 2; bHave = true; E.Id = UnquotePo(L); }
			else if (L.StartsWith(TEXT("msgstr"))) { Cur = 3; E.Str = UnquotePo(L); }
			else if (L.StartsWith(TEXT("\"")))
			{
				if (Cur == 1) { E.Ctxt += UnquotePo(L); } else if (Cur == 2) { E.Id += UnquotePo(L); } else if (Cur == 3) { E.Str += UnquotePo(L); }
			}
		}
		Flush();
	}

	/** Namespace and key of a po entry: UE writes msgctxt "Namespace,Key" and a "#. Key:" comment. */
	void SplitContext(const FPoItem& It, FString& Ns, FString& Key)
	{
		if (!It.CommentKey.IsEmpty())
		{
			Key = It.CommentKey;
			Ns = It.Ctxt;
			if (Ns.EndsWith(TEXT(",") + Key)) { Ns.LeftChopInline(Key.Len() + 1); } else if (Ns == Key) { Ns.Empty(); }
			return;
		}
		int32 Comma;
		if (It.Ctxt.FindChar(TEXT(','), Comma)) { Ns = It.Ctxt.Left(Comma); Key = It.Ctxt.Mid(Comma + 1); }
		else { Ns.Empty(); Key = It.Ctxt; }
	}

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
			"namespace":{"type":"string","description":"Namespace of the entries in the po/locres file. Default: the namespace of the source String Table. '*' = any namespace."},
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
			}

			// Translation: key -> entry.
			TMap<FString, FTransEntry> Tr;
			FString TransName, Kind;
			TArray<FString> Notes;
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
				const bool bAnyNs = NsArg == TEXT("*");
				TMap<FString, FTransEntry> Any;      // every namespace, first entry of a key wins
				TMap<FString, FTransEntry> InNs;     // only NsArg
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

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("source"), SourceName);
			Data->SetStringField(TEXT("translation"), TransName);
			Data->SetStringField(TEXT("translationKind"), Kind);
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
