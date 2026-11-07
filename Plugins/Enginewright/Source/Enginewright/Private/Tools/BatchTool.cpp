#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace
{
	/** Title of the newest transaction that is not undone, or empty. */
	FString LastTransactionTitle()
	{
		if (!GEditor || !GEditor->Trans) { return FString(); }
		const int32 Index = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - 1;
		const FTransaction* T = Index >= 0 ? GEditor->Trans->GetTransaction(Index) : nullptr;
		return T ? T->GetTitle().ToString() : FString();
	}
}

void RegisterBatchTool(FEWRegistry& R)
{
	R.Register(TEXT("batch"),
		TEXT("Run several tool calls in order as one step: [{tool, args}, ...]. Atomic (default): if a step fails, every earlier step is undone and nothing is saved; saves happen once at the end. Steps that undo cannot revert (asset create/rename/delete/save, settings) are refused in atomic mode."),
		TEXT(R"({"type":"object","properties":{
			"steps":{"type":"array","items":{"type":"object"},"description":"Each step: {\"tool\": name, \"args\": {...}}."},
			"atomic":{"type":"boolean","description":"Default true. False runs every step and reports each result, without rollback."}},
			"required":["steps"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FEWRegistry& Reg = FEWRegistry::Get();
			const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
			Args->TryGetArrayField(TEXT("steps"), Steps);
			bool bAtomic = true;
			Args->TryGetBoolField(TEXT("atomic"), bAtomic);
			if (!Steps || Steps->Num() == 0) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), TEXT("steps must be a non-empty array.")); }
			if (EW::Batch().Depth > 0) { return FEWResult::Error(TEXT("NESTED_BATCH"), TEXT("A batch cannot contain another batch.")); }

			// Check every step before running any.
			struct FStep { FString Tool; TSharedPtr<FJsonObject> Args; };
			TArray<FStep> Plan;
			for (int32 i = 0; i < Steps->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* S = nullptr;
				if (!(*Steps)[i]->TryGetObject(S)) { return FEWResult::Error(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Step %d is not an object."), i)); }
				FStep Step;
				(*S)->TryGetStringField(TEXT("tool"), Step.Tool);
				const TSharedPtr<FJsonObject>* A = nullptr;
				Step.Args = (*S)->TryGetObjectField(TEXT("args"), A) ? *A : MakeShared<FJsonObject>();
				const FEWTool* T = Reg.Find(Step.Tool);
				if (!T) { FEWResult E = Reg.Call(Step.Tool, nullptr); E.Message = FString::Printf(TEXT("Step %d: %s"), i, *E.Message); return E; }
				if (T->Name == TEXT("batch")) { return FEWResult::Error(TEXT("NESTED_BATCH"), TEXT("A batch cannot contain another batch.")); }
				if (bAtomic && T->bWrites && !T->bTransactional)
				{
					return FEWResult::Error(TEXT("NOT_ROLLBACKABLE"),
						FString::Printf(TEXT("Step %d (%s) cannot be undone, so it cannot be part of an atomic batch. Run it on its own, or set atomic false."), i, *T->Name));
				}
				Plan.Add(Step);
			}

			const FString Title = FString::Printf(TEXT("Enginewright batch %s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
			TArray<TSharedPtr<FJsonValue>> Results;
			int32 FailedAt = INDEX_NONE;
			EW::Batch().Depth++;
			EW::Batch().Pending.Reset();
			{
				FScopedTransaction Tx(FText::FromString(Title));
				for (int32 i = 0; i < Plan.Num(); ++i)
				{
					const FEWResult One = Reg.Call(Plan[i].Tool, Plan[i].Args);
					TSharedRef<FJsonObject> J = One.ToJson();
					J->RemoveField(TEXT("editor"));
					J->SetStringField(TEXT("tool"), Plan[i].Tool);
					Results.Add(MakeShared<FJsonValueObject>(J));
					if (!One.bOk)
					{
						FailedAt = i;
						if (bAtomic) { break; }
					}
				}
			}
			EW::Batch().Depth--;

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("results"), Results);

			if (FailedAt != INDEX_NONE && bAtomic)
			{
				bool bRolledBack = false;
				if (LastTransactionTitle() == Title) { bRolledBack = GEditor->UndoTransaction(false); }
				EW::Batch().Pending.Reset();
				Data->SetBoolField(TEXT("rolledBack"), bRolledBack);
				FEWResult Res = FEWResult::Error(TEXT("BATCH_FAILED"),
					FString::Printf(TEXT("Step %d (%s) failed. %s Nothing was saved."), FailedAt, *Plan[FailedAt].Tool,
						bRolledBack ? TEXT("Earlier steps were undone.") : TEXT("Earlier steps made no undoable change.")));
				Res.Data = Data;
				return Res;
			}

			// Save once, at the end.
			TArray<UPackage*> ToSave;
			for (const TWeakObjectPtr<UPackage>& P : EW::Batch().Pending) { if (P.IsValid()) { ToSave.Add(P.Get()); } }
			EW::Batch().Pending.Reset();
			const bool bSaved = ToSave.Num() == 0 || UEditorLoadingAndSavingUtils::SavePackages(ToSave, false);
			Data->SetNumberField(TEXT("savedPackages"), bSaved ? ToSave.Num() : 0);
			FEWResult Res = FailedAt == INDEX_NONE ? FEWResult::Ok(Data)
				: FEWResult::Error(TEXT("STEPS_FAILED"), TEXT("Some steps failed (atomic false); the others were kept."));
			Res.Data = Data;
			if (!bSaved) { Res.Warnings.Add(TEXT("Saving at the end failed; changes are in memory only.")); }
			return Res;
		});
}
