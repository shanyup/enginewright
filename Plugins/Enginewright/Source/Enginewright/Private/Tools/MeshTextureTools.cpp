#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "StaticMeshResources.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 100) / 100), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 100) / 100), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 100) / 100) };
	}

	/** Local bounds (mesh space, pivot at the origin), size and where the pivot sits relative to the bounds. No actor is spawned. */
	void AddBounds(const TSharedRef<FJsonObject>& D, const FBox& Box)
	{
		const FVector Center = Box.GetCenter(), Min = Box.Min, Max = Box.Max;
		D->SetArrayField(TEXT("size"), Vec(Box.GetSize()));
		D->SetArrayField(TEXT("boundsMin"), Vec(Min));
		D->SetArrayField(TEXT("boundsMax"), Vec(Max));
		D->SetArrayField(TEXT("boundsCenter"), Vec(Center));
		// Pivot = the mesh origin. Offsets say where the pivot is, measured from the bounds center and from the bottom center of the bounds.
		const FVector FromCenter = -Center;
		const FVector FromBase(-Center.X, -Center.Y, -Min.Z);
		D->SetArrayField(TEXT("pivotFromCenter"), Vec(FromCenter));
		D->SetArrayField(TEXT("pivotFromBase"), Vec(FromBase));
		const double Tol = FMath::Max(0.01, Box.GetSize().GetMax() * 0.001);
		const bool bCenter = FromCenter.GetAbsMax() <= Tol;
		const bool bBase = FMath::Abs(FromBase.X) <= Tol && FMath::Abs(FromBase.Y) <= Tol && FMath::Abs(FromBase.Z) <= Tol;
		D->SetStringField(TEXT("pivot"), bCenter ? TEXT("center") : bBase ? TEXT("base") : TEXT("other"));
	}

	bool SaveAsset(UObject* A, FEWResult& Res)
	{
		A->MarkPackageDirty();
		bool bDeferred = false;
		const bool bOk = EW::SaveOrDefer(A->GetOutermost(), bDeferred);
		if (!bOk) { Res.Warnings.Add(TEXT("Save failed; the change is in memory only.")); }
		return bOk;
	}

	TArray<TSharedPtr<FJsonValue>> SlotsJson(const TArray<FName>& Names, const TArray<UMaterialInterface*>& Mats)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetNumberField(TEXT("index"), i);
			S->SetStringField(TEXT("name"), Names[i].ToString());
			S->SetStringField(TEXT("material"), Mats.IsValidIndex(i) && Mats[i] ? Mats[i]->GetPathName() : FString());
			Out.Add(MakeShared<FJsonValueObject>(S));
		}
		return Out;
	}
}

void RegisterMeshTextureTools(FEWRegistry& R)
{
	R.Register(TEXT("mesh_info"),
		TEXT("Describe a static or skeletal mesh without spawning an actor: local bounds (boundsMin, boundsMax, boundsCenter, size, in cm, mesh space with the pivot at the origin), where the pivot sits (pivotFromCenter, pivotFromBase = pivot minus the bottom center of the bounds, pivot = center / base / other), LODs with vertex and triangle counts, material slots, collision, Nanite; skeletal: bone count, skeleton, physics asset. Use it to size and place a mesh before you put it in a level."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Resolved, Err;
			UObject* Obj = EW::ResolveObject(EW::Str(Args, TEXT("path")), false, Resolved, Err);
			if (!Obj) { return Bad(TEXT("ASSET_NOT_FOUND"), Err); }
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("path"), Obj->GetPathName());
			if (UStaticMesh* SM = Cast<UStaticMesh>(Obj))
			{
				D->SetStringField(TEXT("kind"), TEXT("static"));
				AddBounds(D, SM->GetBoundingBox());
				TArray<TSharedPtr<FJsonValue>> Lods;
				for (int32 i = 0; i < SM->GetNumLODs(); ++i)
				{
					TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
					L->SetNumberField(TEXT("vertices"), SM->GetNumVertices(i));
					L->SetNumberField(TEXT("triangles"), SM->GetNumTriangles(i));
					Lods.Add(MakeShared<FJsonValueObject>(L));
				}
				D->SetArrayField(TEXT("lods"), Lods);
				TArray<FName> Names; TArray<UMaterialInterface*> Mats;
				for (const FStaticMaterial& M : SM->GetStaticMaterials()) { Names.Add(M.MaterialSlotName); Mats.Add(M.MaterialInterface); }
				D->SetArrayField(TEXT("slots"), SlotsJson(Names, Mats));
				if (UBodySetup* B = SM->GetBodySetup())
				{
					TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
					C->SetNumberField(TEXT("simpleShapes"), B->AggGeom.GetElementCount());
					C->SetStringField(TEXT("traceFlag"), UEnum::GetValueAsString(B->CollisionTraceFlag.GetValue()));
					D->SetObjectField(TEXT("collision"), C);
				}
				D->SetBoolField(TEXT("nanite"), SM->IsNaniteEnabled());
				return FEWResult::Ok(D);
			}
			if (USkeletalMesh* SK = Cast<USkeletalMesh>(Obj))
			{
				D->SetStringField(TEXT("kind"), TEXT("skeletal"));
				AddBounds(D, SK->GetBounds().GetBox());
				D->SetNumberField(TEXT("bones"), SK->GetRefSkeleton().GetNum());
				D->SetNumberField(TEXT("lods"), SK->GetLODNum());
				D->SetStringField(TEXT("skeleton"), SK->GetSkeleton() ? SK->GetSkeleton()->GetPathName() : FString());
				D->SetStringField(TEXT("physicsAsset"), SK->GetPhysicsAsset() ? SK->GetPhysicsAsset()->GetPathName() : FString());
				TArray<FName> Names; TArray<UMaterialInterface*> Mats;
				for (const FSkeletalMaterial& M : SK->GetMaterials()) { Names.Add(M.MaterialSlotName); Mats.Add(M.MaterialInterface); }
				D->SetArrayField(TEXT("slots"), SlotsJson(Names, Mats));
				return FEWResult::Ok(D);
			}
			return Bad(TEXT("NOT_A_MESH"), FString::Printf(TEXT("'%s' is a %s, not a static or skeletal mesh."), *Resolved, *Obj->GetClass()->GetName()));
		});

	R.Register(TEXT("mesh_set_material"),
		TEXT("Set the material of a mesh material slot (by slot name or index) on a static or skeletal mesh asset; read back and saved."),
		TEXT(R"({"type":"object","properties":{
			"path":{"type":"string"},
			"slot":{"type":["string","integer"],"description":"Slot name or index."},
			"material":{"type":"string"}},
			"required":["path","slot","material"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Resolved, Err;
			UObject* Obj = EW::ResolveObject(EW::Str(Args, TEXT("path")), false, Resolved, Err);
			if (!Obj) { return Bad(TEXT("ASSET_NOT_FOUND"), Err); }
			UMaterialInterface* Mat = EW::LoadAs<UMaterialInterface>(EW::Str(Args, TEXT("material")), Err);
			if (!Mat) { return Bad(TEXT("MATERIAL_NOT_FOUND"), Err); }
			TArray<FName> Names;
			UStaticMesh* SM = Cast<UStaticMesh>(Obj);
			USkeletalMesh* SK = Cast<USkeletalMesh>(Obj);
			if (SM) { for (const FStaticMaterial& M : SM->GetStaticMaterials()) { Names.Add(M.MaterialSlotName); } }
			else if (SK) { for (const FSkeletalMaterial& M : SK->GetMaterials()) { Names.Add(M.MaterialSlotName); } }
			else { return Bad(TEXT("NOT_A_MESH"), TEXT("Not a static or skeletal mesh.")); }
			int32 Index = INDEX_NONE;
			const TSharedPtr<FJsonValue> Slot = Args->TryGetField(TEXT("slot"));
			if (Slot->Type == EJson::Number) { Index = (int32)Slot->AsNumber(); }
			else
			{
				for (int32 i = 0; i < Names.Num(); ++i) { if (Names[i].ToString().Equals(Slot->AsString(), ESearchCase::IgnoreCase)) { Index = i; } }
				if (Index == INDEX_NONE)
				{
					TArray<FString> S; for (const FName& N : Names) { S.Add(N.ToString()); }
					return Bad(TEXT("UNKNOWN_SLOT"), FString::Printf(TEXT("No slot '%s'. Slots: %s"), *Slot->AsString(), *FString::Join(S, TEXT(", "))), EW::Suggest(Slot->AsString(), S));
				}
			}
			if (!Names.IsValidIndex(Index)) { return Bad(TEXT("UNKNOWN_SLOT"), FString::Printf(TEXT("Slot index %d is out of range (0-%d)."), Index, Names.Num() - 1)); }
			Obj->Modify();
			if (SM) { SM->SetMaterial(Index, Mat); }
			else { SK->GetMaterials()[Index].MaterialInterface = Mat; SK->PostEditChange(); }
			UMaterialInterface* Back = SM ? SM->GetMaterial(Index) : SK->GetMaterials()[Index].MaterialInterface.Get();
			if (Back != Mat) { return Bad(TEXT("READBACK_MISMATCH"), TEXT("The slot does not read back the new material.")); }
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetNumberField(TEXT("slot"), Index);
			D->SetStringField(TEXT("slotName"), Names[Index].ToString());
			D->SetStringField(TEXT("material"), Mat->GetPathName());
			FEWResult Res = FEWResult::Ok(D);
			Res.Data->SetBoolField(TEXT("saved"), SaveAsset(Obj, Res));
			return Res;
		}, /*bTransactional*/ false);

	R.Register(TEXT("texture_info"),
		TEXT("Describe a texture: size, pixel format, compression, sRGB, LOD group, mip count, streaming. Change settings with object_set (e.g. CompressionSettings, SRGB, LODGroup)."),
		TEXT(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"),
		false,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			FString Err;
			UTexture* T = EW::LoadAs<UTexture>(EW::Str(Args, TEXT("path")), Err);
			if (!T) { return Bad(TEXT("TEXTURE_NOT_FOUND"), Err); }
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("path"), T->GetPathName());
			D->SetStringField(TEXT("class"), T->GetClass()->GetName());
			if (UTexture2D* T2 = Cast<UTexture2D>(T))
			{
				D->SetNumberField(TEXT("width"), T2->GetSizeX());
				D->SetNumberField(TEXT("height"), T2->GetSizeY());
				D->SetNumberField(TEXT("mips"), T2->GetNumMips());
				D->SetStringField(TEXT("pixelFormat"), GetPixelFormatString(T2->GetPixelFormat()));
				D->SetNumberField(TEXT("sourceWidth"), T2->Source.GetSizeX());
				D->SetNumberField(TEXT("sourceHeight"), T2->Source.GetSizeY());
			}
			D->SetStringField(TEXT("compression"), UEnum::GetValueAsString(T->CompressionSettings.GetValue()));
			D->SetBoolField(TEXT("sRGB"), T->SRGB);
			D->SetStringField(TEXT("lodGroup"), UEnum::GetValueAsString(T->LODGroup.GetValue()));
			D->SetBoolField(TEXT("neverStream"), T->NeverStream);
			return FEWResult::Ok(D);
		});
}
