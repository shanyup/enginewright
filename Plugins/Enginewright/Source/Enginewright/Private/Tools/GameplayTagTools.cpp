// Gameplay Tags: list, add (to a tag ini), remove (refused while content references the tag).
#include "EnginewrightTool.h"
#include "EWCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	TArray<FString> AllTagNames()
	{
		FGameplayTagContainer All;
		UGameplayTagsManager::Get().RequestAllGameplayTags(All, false);
		TArray<FString> Out;
		for (const FGameplayTag& T : All) { Out.Add(T.ToString()); }
		return Out;
	}

	TSharedRef<FJsonObject> TagJson(FName Tag)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("tag"), Tag.ToString());
		FString Comment;
		TArray<FName> Sources;
		bool bExplicit = false, bRestricted = false, bAllowChildren = true;
		if (UGameplayTagsManager::Get().GetTagEditorData(Tag, Comment, Sources, bExplicit, bRestricted, bAllowChildren))
		{
			if (!Comment.IsEmpty()) { J->SetStringField(TEXT("comment"), Comment); }
			TArray<TSharedPtr<FJsonValue>> S;
			for (FName N : Sources) { S.Add(MakeShared<FJsonValueString>(N.ToString())); }
			J->SetArrayField(TEXT("sources"), S);
			J->SetBoolField(TEXT("explicit"), bExplicit);
		}
		return J;
	}

	TArray<FString> TagReferencers(FName Tag)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetIdentifier> Refs;
		AR.GetReferencers(FAssetIdentifier(FGameplayTag::StaticStruct(), Tag), Refs, UE::AssetRegistry::EDependencyCategory::SearchableName);
		TArray<FString> Out;
		for (const FAssetIdentifier& R : Refs) { Out.AddUnique(R.PackageName.ToString()); }
		return Out;
	}

	bool TagExists(const FString& Tag)
	{
		return UGameplayTagsManager::Get().RequestGameplayTag(FName(*Tag), false).IsValid();
	}
}

void RegisterGameplayTagTools(FEWRegistry& R)
{
	R.Register(TEXT("gameplay_tags_list"),
		TEXT("List Gameplay Tags (optionally under a prefix such as Item.Food) with comment, source ini and whether they are explicit."),
		TEXT(R"({"type":"object","properties":{
			"prefix":{"type":"string","description":"Only this tag and its children."},
			"limit":{"type":"integer","description":"Default 500."}}})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			const FString Prefix = EW::Str(Args, TEXT("prefix"));
			const int32 Limit = FMath::Clamp((int32)EW::Num(Args, TEXT("limit"), 500), 1, 10000);
			TArray<FString> Names = AllTagNames();
			Names.Sort();
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (const FString& N : Names)
			{
				if (!Prefix.IsEmpty() && N != Prefix && !N.StartsWith(Prefix + TEXT("."))) { continue; }
				++Total;
				if (Rows.Num() < Limit) { Rows.Add(MakeShared<FJsonValueObject>(TagJson(FName(*N)))); }
			}
			if (!Prefix.IsEmpty() && Total == 0) { return Bad(TEXT("TAG_NOT_FOUND"), FString::Printf(TEXT("No tag '%s'."), *Prefix), EW::Suggest(Prefix, Names)); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetArrayField(TEXT("tags"), Rows);
			return FEWResult::Ok(Data);
		});

	R.Register(TEXT("gameplay_tag_add"),
		TEXT("Add a Gameplay Tag to a tag ini (default Config/DefaultGameplayTags.ini) and read it back. Fails on an invalid name (with the fixed form suggested) or when the tag already exists explicitly."),
		TEXT(R"({"type":"object","properties":{
			"tag":{"type":"string","description":"e.g. Item.Food.Apple"},
			"comment":{"type":"string"},
			"source":{"type":"string","description":"Tag ini file name, e.g. DefaultGameplayTags.ini (default) or a file under Config/Tags."},
			"allowAnyCharacters":{"type":"boolean","description":"Accept every name the engine accepts (spaces, symbols). Default false."}},
			"required":["tag"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			if (!IGameplayTagsEditorModule::IsAvailable()) { return Bad(TEXT("NOT_AVAILABLE"), TEXT("The GameplayTagsEditor plugin is not loaded.")); }
			UGameplayTagsManager& M = UGameplayTagsManager::Get();
			const FString Tag = EW::Str(Args, TEXT("tag")).TrimStartAndEnd();
			FText Why;
			FString Fixed;
			// Stricter than the engine by default: letters, digits, _ and - between dots (no spaces or symbols).
			if (!EW::Bool(Args, TEXT("allowAnyCharacters"), false))
			{
				bool bPlain = !Tag.IsEmpty() && !Tag.StartsWith(TEXT(".")) && !Tag.EndsWith(TEXT(".")) && !Tag.Contains(TEXT(".."));
				FString Clean;
				for (TCHAR Ch : Tag)
				{
					const bool bOkChar = FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('.');
					bPlain &= bOkChar && Ch < 128;
					Clean.AppendChar(bOkChar && Ch < 128 ? Ch : TEXT('_'));
				}
				if (!bPlain)
				{
					return Bad(TEXT("INVALID_TAG"), FString::Printf(TEXT("'%s' has characters other than letters, digits, _ and - between dots (pass allowAnyCharacters true to use the engine's looser rule)."), *Tag), { Clean });
				}
			}
			if (Tag.IsEmpty() || !M.IsValidGameplayTagString(Tag, &Why, &Fixed))
			{
				TArray<FString> Sug;
				if (!Fixed.IsEmpty() && Fixed != Tag) { Sug.Add(Fixed); }
				return Bad(TEXT("INVALID_TAG"), FString::Printf(TEXT("'%s' is not a valid tag: %s"), *Tag, *Why.ToString()), Sug);
			}
			FString Comment;
			TArray<FName> Sources;
			bool bExplicit = false, bRestricted = false, bAllow = true;
			if (M.GetTagEditorData(FName(*Tag), Comment, Sources, bExplicit, bRestricted, bAllow) && bExplicit)
			{
				return Bad(TEXT("TAG_EXISTS"), FString::Printf(TEXT("'%s' already exists."), *Tag));
			}
			FString Source = EW::Str(Args, TEXT("source"));
			if (Source.IsEmpty()) { Source = TEXT("DefaultGameplayTags.ini"); }
			if (!Source.EndsWith(TEXT(".ini"))) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("source must be an .ini file name, e.g. DefaultGameplayTags.ini.")); }
			if (!IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(Tag, EW::Str(Args, TEXT("comment")), FName(*Source)))
			{
				return Bad(TEXT("ADD_FAILED"), TEXT("The editor refused the tag (see log_read for the reason)."));
			}
			if (!TagExists(Tag)) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The tag was written but is not in the tag tree.")); }
			return FEWResult::Ok(TagJson(FName(*Tag)));
		}, /*bTransactional*/ false);

	R.Register(TEXT("gameplay_tag_remove"),
		TEXT("Remove an explicit Gameplay Tag from its ini. Refused while assets reference it (the reply lists them) or when it is only implied by child tags."),
		TEXT(R"({"type":"object","properties":{"tag":{"type":"string"}},"required":["tag"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			if (!IGameplayTagsEditorModule::IsAvailable()) { return Bad(TEXT("NOT_AVAILABLE"), TEXT("The GameplayTagsEditor plugin is not loaded.")); }
			UGameplayTagsManager& M = UGameplayTagsManager::Get();
			const FString Tag = EW::Str(Args, TEXT("tag")).TrimStartAndEnd();
			TSharedPtr<FGameplayTagNode> Node = M.FindTagNode(FName(*Tag));
			if (!Node.IsValid()) { return Bad(TEXT("TAG_NOT_FOUND"), FString::Printf(TEXT("No tag '%s'."), *Tag), EW::Suggest(Tag, AllTagNames())); }
			if (!Node->IsExplicitTag()) { return Bad(TEXT("TAG_IMPLICIT"), FString::Printf(TEXT("'%s' only exists because of its child tags; remove those."), *Tag)); }
			if (Node->GetChildTagNodes().Num()) { return Bad(TEXT("TAG_HAS_CHILDREN"), FString::Printf(TEXT("'%s' has child tags; remove them first."), *Tag)); }
			const TArray<FString> Refs = TagReferencers(FName(*Tag));
			if (Refs.Num())
			{
				FEWResult Res = Bad(TEXT("TAG_REFERENCED"), FString::Printf(TEXT("'%s' is still used by %d asset(s); nothing was removed."), *Tag, Refs.Num()));
				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> J;
				for (const FString& S : Refs) { J.Add(MakeShared<FJsonValueString>(S)); }
				Data->SetArrayField(TEXT("referencers"), J);
				Res.Data = Data;
				return Res;
			}
			if (!IGameplayTagsEditorModule::Get().DeleteTagFromINI(Node)) { return Bad(TEXT("REMOVE_FAILED"), TEXT("The editor refused to remove the tag (for example a native or data-table tag; see log_read).")); }
			if (TagExists(Tag)) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The tag is still in the tag tree.")); }
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("removed"), Tag);
			return FEWResult::Ok(Data);
		}, /*bTransactional*/ false);
}
