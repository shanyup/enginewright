// scene_dress: set-dress the surroundings of an actor or a point with many meshes in one undo step.
#include "EnginewrightTool.h"
#include "EWCommon.h"
#include "EWProps.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DataTableEditorUtils.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "ScopedTransaction.h"

namespace
{
	FEWResult Bad(const FString& Code, const FString& Msg, const TArray<FString>& Sug = {}) { return FEWResult::Error(Code, Msg, Sug); }

	constexpr int32 MaxInstances = 5000;

	struct FMeshInfo
	{
		UStaticMesh* Mesh = nullptr;
		FString Path;
		FBox Bounds = FBox(ForceInit);
		double Weight = 1.0;
		int32 Placed = 0;
	};

	struct FPlaced
	{
		FTransform World;
		FTransform Local; // relative to the frame (target actor or center)
		FRotator LocalRot; // the rotation as given (pitch 90 would not survive a quaternion round trip)
		int32 MeshIdx = 0;
		FVector2D Center2D = FVector2D::ZeroVector;
		double Radius = 0;
	};

	TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V, double Scale = 10.0)
	{
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * Scale) / Scale), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * Scale) / Scale), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * Scale) / Scale) };
	}

	/** Reads [x,y,z] (or [x,y] when N == 2) arrays strictly. Missing key = bPresent false, no error. */
	bool ReadNums(const TSharedPtr<FJsonObject>& A, const TCHAR* Key, int32 N, TArray<double>& Out, bool& bPresent, FString& Err)
	{
		bPresent = false;
		if (!A->HasField(Key)) { return true; }
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!A->TryGetArrayField(Key, Arr) || Arr->Num() != N)
		{
			Err = FString::Printf(TEXT("%s must be an array of %d numbers."), Key, N);
			return false;
		}
		Out.Reset();
		for (const auto& V : *Arr)
		{
			if (V->Type != EJson::Number) { Err = FString::Printf(TEXT("%s must hold numbers only."), Key); return false; }
			Out.Add(V->AsNumber());
		}
		bPresent = true;
		return true;
	}

	bool ReadVec3(const TSharedPtr<FJsonObject>& A, const TCHAR* Key, FVector& Out, bool& bPresent, FString& Err)
	{
		TArray<double> N;
		if (!ReadNums(A, Key, 3, N, bPresent, Err)) { return false; }
		if (bPresent) { Out = FVector(N[0], N[1], N[2]); }
		return true;
	}

	AActor* FindActor(UWorld* World, const FString& Key)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!IsValid(*It)) { continue; }
			if (It->GetActorLabel() == Key || It->GetName() == Key || It->GetPathName() == Key) { return *It; }
		}
		return nullptr;
	}

	TArray<FString> ActorLabels(UWorld* World)
	{
		TArray<FString> Out;
		for (TActorIterator<AActor> It(World); It; ++It) { Out.Add(It->GetActorLabel()); }
		return Out;
	}

	double YawOfNormal(double Nx, double Ny) { return FMath::RadiansToDegrees(FMath::Atan2(Ny, Nx)); }

	struct FPoint
	{
		FVector Pos = FVector::ZeroVector; // in the frame, before offset
		double FaceYaw = 0;                // yaw that "outward" means for this point
	};

	/** Property name of an export column, from the "columns" object or the default. */
	FString ColumnName(const TSharedPtr<FJsonObject>& Cols, const TCHAR* Key, const TCHAR* Default)
	{
		FString V;
		if (Cols.IsValid() && Cols->TryGetStringField(Key, V) && !V.IsEmpty()) { return V; }
		return Default;
	}
}

void RegisterSceneDressTools(FEWRegistry& R)
{
	R.Register(TEXT("scene_dress"),
		TEXT("Set-dress an actor or a point with many meshes in one call and one undo step: a ring or ellipse (shields on a ship, stones around a camp), a line (fence), a grid (crates) or a random scatter. ")
		TEXT("Give an actor (target) or a point (center). The pattern is laid out around it; meshes can be several (random or in turn), with seed, rotation and scale jitter, ground snapping (trace down), overlap check (against each other and against listed actors). ")
		TEXT("Output: instanced (default, one actor with one hierarchical instanced mesh per mesh: fast for hundreds), actors (one StaticMeshActor each) or none (compute only). Running again with the same name replaces the earlier result. ")
		TEXT("export writes the placements as rows (Scene, Mesh, Location, Rotation, Scale relative to the target) into a DataTable, e.g. a game's dressing table; with output none it only fills the table. Use actor_spawn for single actors and datatable_get to check the table."),
		TEXT(R"({"type":"object","properties":{
			"name":{"type":"string","description":"Name of this dressing, default Dress. The result is tagged EWDress=<name>; a second call with the same name replaces it (replace:false to keep both)."},
			"target":{"type":"string","description":"Actor label, name or path. The pattern follows its location and rotation (relative:false: location only)."},
			"center":{"type":"array","items":{"type":"number"},"description":"[x,y,z] world point instead of a target."},
			"offset":{"type":"array","items":{"type":"number"},"description":"[x,y,z] shift of the pattern center, in the target's space (world space with center)."},
			"relative":{"type":"boolean","description":"Follow the target's rotation (default true)."},
			"meshes":{"type":"array","items":{"type":"string"},"description":"Static mesh paths, e.g. /Game/Art/Shield.Shield. At least one."},
			"weights":{"type":"array","items":{"type":"number"},"description":"Optional weight per mesh for the random pick."},
			"pick":{"type":"string","enum":["random","cycle"],"description":"random (default, seeded) or cycle: mesh 1, 2, 1, 2 ..."},
			"pattern":{"type":"string","enum":["ring","line","grid","scatter"],"description":"Layout: ring, line, grid or scatter."},
			"count":{"type":"integer","description":"ring, line, scatter: number of meshes (max 5000)."},
			"radius":{"type":"number","description":"ring, scatter: radius (X radius of an ellipse) in cm."},
			"radiusY":{"type":"number","description":"ring, scatter: Y radius for an ellipse; default = radius."},
			"innerRadius":{"type":"number","description":"scatter: keep this much of the middle free (cm)."},
			"startAngle":{"type":"number","description":"ring: first angle in degrees, default 0 (+X, turning toward +Y)."},
			"endAngle":{"type":"number","description":"ring: last angle, default startAngle+360 (full circle, no double mesh at the seam)."},
			"to":{"type":"array","items":{"type":"number"},"description":"line: end point [x,y,z] in the target's space; the line starts at the pattern center."},
			"rows":{"type":"integer","description":"grid: rows along X."},
			"cols":{"type":"integer","description":"grid: columns along Y."},
			"spacing":{"type":"array","items":{"type":"number"},"description":"grid: [dx,dy] in cm."},
			"minDistance":{"type":"number","description":"scatter: smallest distance between two centers (cm)."},
			"facing":{"type":"string","enum":["none","outward","inward","tangent"],"description":"Which way each mesh turns. ring/scatter: relative to the center; line: relative to the line direction. Default outward for ring, none otherwise."},
			"yaw":{"type":"number","description":"Extra yaw in degrees added to every mesh."},
			"pitch":{"type":"number","description":"Base pitch in degrees for every mesh (e.g. 90 for planks that stand on their edge)."},
			"roll":{"type":"number","description":"Base roll in degrees for every mesh."},
			"seed":{"type":"integer","description":"Random seed (default 1). Same seed = same result."},
			"scale":{"type":["number","array"],"description":"Base scale: a number or [x, y, z]. Default 1."},
			"scaleJitter":{"type":"number","description":"Random size change, 0 to 1: 0.2 means 80% to 120%."},
			"rotationJitter":{"type":"array","items":{"type":"number"},"description":"[pitch,yaw,roll] random swing in degrees (plus or minus)."},
			"positionJitter":{"type":"number","description":"Random shift in X and Y, plus or minus cm (ring, line, grid)."},
			"snapToGround":{"type":"boolean","description":"Trace down from each point and sit the mesh on the hit; points without ground are skipped."},
			"align":{"type":"string","enum":["pivot","base"],"description":"With snapToGround: pivot (default) puts the mesh origin on the ground; base puts the bottom of its bounds there."},
			"traceUp":{"type":"number","description":"Start of the ground trace above the point, default 500 cm."},
			"traceDown":{"type":"number","description":"Length of the trace below the point, default 2000 cm."},
			"avoidOverlap":{"type":"boolean","description":"Skip a mesh whose footprint overlaps one placed before."},
			"clearance":{"type":"number","description":"Extra gap in cm for avoidOverlap and avoid."},
			"avoid":{"type":"array","items":{"type":"string"},"description":"Actor labels whose footprint (XY bounds) stays free."},
			"output":{"type":"string","enum":["instanced","actors","none"],"description":"instanced (default), actors or none (compute only)."},
			"collide":{"type":"boolean","description":"Give the meshes BlockAll collision (default false: none)."},
			"replace":{"type":"boolean","description":"Remove the result of an earlier call with the same name first (default true)."},
			"label":{"type":"string","description":"Label of the instanced actor, or prefix of the actor labels. Default <name>."},
			"folder":{"type":"string","description":"Outliner folder, default Dress/<name>."},
			"export":{"type":"object","description":"Write the placements to a DataTable: {table (path), scene (value of the scene column), prefix (row name prefix, default name), mode: replace (default, removes rows with this prefix first) or add (fails if a row exists), columns: {scene,mesh,location,rotation,scale -> property names; defaults Scene, Mesh, Location, Rotation, Scale}, fields: {property: value} for every row, save (default true)}."}},
			"required":["pattern","meshes"]})"),
		true,
		[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { return Bad(TEXT("NO_LEVEL"), TEXT("No editor level is open.")); }
			FString Err;
			bool bHas = false;

			// ---- read and check every argument first
			FString Name = EW::Str(Args, TEXT("name"));
			if (Name.IsEmpty()) { Name = TEXT("Dress"); }
			const FString Pattern = EW::Str(Args, TEXT("pattern"));
			FString Output = EW::Str(Args, TEXT("output"));
			if (Output.IsEmpty()) { Output = TEXT("instanced"); }
			FString Facing = EW::Str(Args, TEXT("facing"));
			if (Facing.IsEmpty()) { Facing = Pattern == TEXT("ring") ? TEXT("outward") : TEXT("none"); }
			const bool bReplace = EW::Bool(Args, TEXT("replace"), true);
			const bool bSnap = EW::Bool(Args, TEXT("snapToGround"), false);
			const bool bBaseAlign = EW::Str(Args, TEXT("align")) == TEXT("base");
			const bool bAvoidOverlap = EW::Bool(Args, TEXT("avoidOverlap"), false);
			const bool bCollide = EW::Bool(Args, TEXT("collide"), false);
			const double Clearance = EW::Num(Args, TEXT("clearance"), 0);
			const double TraceUp = EW::Num(Args, TEXT("traceUp"), 500), TraceDown = EW::Num(Args, TEXT("traceDown"), 2000);
			const int32 Seed = (int32)EW::Num(Args, TEXT("seed"), 1);
			const double ScaleJitter = EW::Num(Args, TEXT("scaleJitter"), 0);
			const double PosJitter = EW::Num(Args, TEXT("positionJitter"), 0);
			const double YawExtra = EW::Num(Args, TEXT("yaw"), 0);
			const double BasePitch = EW::Num(Args, TEXT("pitch"), 0), BaseRoll = EW::Num(Args, TEXT("roll"), 0);
			const bool bCycle = EW::Str(Args, TEXT("pick")) == TEXT("cycle");
			if (ScaleJitter < 0 || ScaleJitter > 1) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("scaleJitter must be 0 to 1.")); }
			if (PosJitter < 0 || Clearance < 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("positionJitter and clearance cannot be negative.")); }
			if (TraceUp < 0 || TraceDown <= 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("traceUp must be 0 or more and traceDown more than 0.")); }

			FVector Offset = FVector::ZeroVector, CenterArg = FVector::ZeroVector, To = FVector::ZeroVector, RotJitter = FVector::ZeroVector;
			bool bOffset = false, bCenter = false, bTo = false, bRotJitter = false;
			if (!ReadVec3(Args, TEXT("offset"), Offset, bOffset, Err) || !ReadVec3(Args, TEXT("center"), CenterArg, bCenter, Err)
				|| !ReadVec3(Args, TEXT("to"), To, bTo, Err) || !ReadVec3(Args, TEXT("rotationJitter"), RotJitter, bRotJitter, Err))
			{
				return Bad(TEXT("INVALID_ARGUMENT"), Err);
			}
			FVector BaseScale = FVector::OneVector;
			if (Args->HasField(TEXT("scale")))
			{
				double Uniform = 1;
				if (Args->TryGetNumberField(TEXT("scale"), Uniform)) { BaseScale = FVector(Uniform); }
				else if (!ReadVec3(Args, TEXT("scale"), BaseScale, bHas, Err)) { return Bad(TEXT("INVALID_ARGUMENT"), Err); }
				if (BaseScale.X <= 0 || BaseScale.Y <= 0 || BaseScale.Z <= 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("scale must be above 0.")); }
			}

			// Frame: target actor or center point.
			const FString TargetKey = EW::Str(Args, TEXT("target"));
			AActor* Target = nullptr;
			if (!TargetKey.IsEmpty() && bCenter) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Give target or center, not both. Use offset to shift the pattern from the target.")); }
			if (TargetKey.IsEmpty() && !bCenter) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("Give an actor (target) or a point (center) to dress.")); }
			FTransform Frame = FTransform::Identity;
			if (!TargetKey.IsEmpty())
			{
				Target = FindActor(World, TargetKey);
				if (!Target)
				{
					return Bad(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("No actor '%s' in the open level (use the label shown in the Outliner)."), *TargetKey), EW::Suggest(TargetKey, ActorLabels(World)));
				}
				Frame = FTransform(EW::Bool(Args, TEXT("relative"), true) ? Target->GetActorRotation() : FRotator::ZeroRotator, Target->GetActorLocation());
			}
			else { Frame = FTransform(FRotator::ZeroRotator, CenterArg); }

			// Meshes.
			const TArray<FString> MeshPaths = EW::StrList(Args, TEXT("meshes"));
			if (MeshPaths.Num() == 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("meshes needs at least one static mesh path.")); }
			TArray<FMeshInfo> Meshes;
			for (const FString& P : MeshPaths)
			{
				FString MErr;
				UStaticMesh* M = EW::LoadAs<UStaticMesh>(P, MErr);
				if (!M) { return Bad(TEXT("ASSET_NOT_FOUND"), MErr); }
				FMeshInfo Info;
				Info.Mesh = M;
				Info.Path = M->GetPathName();
				Info.Bounds = M->GetBoundingBox();
				Meshes.Add(Info);
			}
			{
				const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
				if (Args->TryGetArrayField(TEXT("weights"), W))
				{
					if (W->Num() != Meshes.Num()) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("weights has %d numbers but meshes has %d."), W->Num(), Meshes.Num())); }
					double Sum = 0;
					for (int32 i = 0; i < W->Num(); ++i)
					{
						Meshes[i].Weight = (*W)[i]->AsNumber();
						if (Meshes[i].Weight < 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("weights cannot be negative.")); }
						Sum += Meshes[i].Weight;
					}
					if (Sum <= 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("weights add up to 0.")); }
				}
			}

			// Pattern arguments.
			int32 Count = (int32)EW::Num(Args, TEXT("count"), 0);
			const double Radius = EW::Num(Args, TEXT("radius"), 0);
			const double RadiusY = Args->HasField(TEXT("radiusY")) ? EW::Num(Args, TEXT("radiusY"), 0) : Radius;
			const double InnerRadius = EW::Num(Args, TEXT("innerRadius"), 0);
			const double StartAngle = EW::Num(Args, TEXT("startAngle"), 0);
			const double EndAngle = Args->HasField(TEXT("endAngle")) ? EW::Num(Args, TEXT("endAngle"), 360) : StartAngle + 360.0;
			const int32 Rows = (int32)EW::Num(Args, TEXT("rows"), 0), Cols = (int32)EW::Num(Args, TEXT("cols"), 0);
			const double MinDist = EW::Num(Args, TEXT("minDistance"), 0);
			TArray<double> Spacing;
			bool bSpacing = false;
			if (!ReadNums(Args, TEXT("spacing"), 2, Spacing, bSpacing, Err)) { return Bad(TEXT("INVALID_ARGUMENT"), Err); }
			if (Pattern == TEXT("grid"))
			{
				if (Rows < 1 || Cols < 1 || !bSpacing) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("grid needs rows, cols (1 or more) and spacing [dx,dy].")); }
				if (Spacing[0] <= 0 || Spacing[1] <= 0) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("spacing must be above 0.")); }
				Count = Rows * Cols;
			}
			else
			{
				if (Count < 1) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s needs count of 1 or more."), *Pattern)); }
				if (Pattern == TEXT("ring") || Pattern == TEXT("scatter"))
				{
					if (Radius <= 0 || RadiusY <= 0) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%s needs radius above 0 (radiusY too, when given)."), *Pattern)); }
				}
				if (Pattern == TEXT("line") && (!bTo || To.IsNearlyZero())) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("line needs to: [x,y,z], an end point away from the start.")); }
				if (InnerRadius < 0 || (Pattern == TEXT("scatter") && InnerRadius >= Radius)) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("innerRadius must be 0 or more and smaller than radius.")); }
			}
			if (Count > MaxInstances) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("%d meshes is too many; the limit is %d per call."), Count, MaxInstances)); }

			// Avoid list.
			TArray<FBox> AvoidBoxes;
			for (const FString& L : EW::StrList(Args, TEXT("avoid")))
			{
				AActor* A = FindActor(World, L);
				if (!A) { return Bad(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("avoid: no actor '%s' in the open level."), *L), EW::Suggest(L, ActorLabels(World))); }
				FVector O, E;
				A->GetActorBounds(false, O, E);
				AvoidBoxes.Add(FBox(O - E, O + E));
			}

			// Export target.
			const TSharedPtr<FJsonObject>* ExportObj = nullptr;
			UDataTable* ExportTable = nullptr;
			FString ExScene, ExPrefix = Name, ExMode = TEXT("replace");
			bool bExSave = true;
			TSharedPtr<FJsonObject> ExCols, ExFields;
			FString ColScene, ColMesh, ColLoc, ColRot, ColScale;
			if (Args->TryGetObjectField(TEXT("export"), ExportObj))
			{
				static const TArray<FString> Known = { TEXT("table"), TEXT("scene"), TEXT("prefix"), TEXT("mode"), TEXT("columns"), TEXT("fields"), TEXT("save") };
				for (const auto& Pair : (*ExportObj)->Values)
				{
					const FString K(*Pair.Key);
					if (!Known.Contains(K)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("export has no field '%s'."), *K), EW::Suggest(K, Known)); }
				}
				FString TablePath;
				if (!(*ExportObj)->TryGetStringField(TEXT("table"), TablePath) || TablePath.IsEmpty()) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("export.table is the DataTable path.")); }
				FString TErr;
				ExportTable = EW::LoadAs<UDataTable>(TablePath, TErr);
				if (!ExportTable) { return Bad(TEXT("TABLE_NOT_FOUND"), TErr); }
				if (!ExportTable->GetRowStruct()) { return Bad(TEXT("TABLE_HAS_NO_STRUCT"), TEXT("The table has no row struct (its C++ struct may be missing).")); }
				(*ExportObj)->TryGetStringField(TEXT("scene"), ExScene);
				FString Pf;
				if ((*ExportObj)->TryGetStringField(TEXT("prefix"), Pf) && !Pf.IsEmpty()) { ExPrefix = Pf; }
				FString Md;
				if ((*ExportObj)->TryGetStringField(TEXT("mode"), Md) && !Md.IsEmpty()) { ExMode = Md; }
				if (ExMode != TEXT("replace") && ExMode != TEXT("add")) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("export.mode is replace or add.")); }
				(*ExportObj)->TryGetBoolField(TEXT("save"), bExSave);
				const TSharedPtr<FJsonObject>* C = nullptr;
				if ((*ExportObj)->TryGetObjectField(TEXT("columns"), C)) { ExCols = *C; }
				const TSharedPtr<FJsonObject>* F = nullptr;
				if ((*ExportObj)->TryGetObjectField(TEXT("fields"), F)) { ExFields = *F; }
				ColScene = ColumnName(ExCols, TEXT("scene"), TEXT("Scene"));
				ColMesh = ColumnName(ExCols, TEXT("mesh"), TEXT("Mesh"));
				ColLoc = ColumnName(ExCols, TEXT("location"), TEXT("Location"));
				ColRot = ColumnName(ExCols, TEXT("rotation"), TEXT("Rotation"));
				ColScale = ColumnName(ExCols, TEXT("scale"), TEXT("Scale"));
				const UScriptStruct* RS = ExportTable->GetRowStruct();
				TArray<FString> Need = { ColMesh, ColLoc, ColRot, ColScale };
				if (!ExScene.IsEmpty()) { Need.Add(ColScene); }
				for (const FString& N : Need)
				{
					if (!EW::FindPropertyLoose(RS, N))
					{
						return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("The row struct %s has no property '%s'. Name the right column in export.columns."), *RS->GetName(), *N), EW::Suggest(N, EW::PropertyNames(RS)));
					}
				}
				if (ExFields.IsValid())
				{
					for (const auto& Pair : ExFields->Values)
					{
						const FString K(*Pair.Key);
						if (!EW::FindPropertyLoose(RS, K)) { return Bad(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("export.fields: the row struct %s has no property '%s'."), *RS->GetName(), *K), EW::Suggest(K, EW::PropertyNames(RS))); }
					}
				}
			}
			if (Output == TEXT("none") && !ExportTable) { return Bad(TEXT("INVALID_ARGUMENT"), TEXT("output none computes only; add export to write the rows somewhere, or choose instanced or actors.")); }

			// ---- compute the placements (no change to the level yet)
			const FName DressTag(*FString::Printf(TEXT("EWDress=%s"), *Name));
			TArray<AActor*> OldDress;
			if (bReplace && Output != TEXT("none"))
			{
				for (TActorIterator<AActor> It(World); It; ++It) { if (IsValid(*It) && It->ActorHasTag(DressTag)) { OldDress.Add(*It); } }
			}

			FRandomStream Rnd(Seed);
			TArray<FPoint> Points;
			Points.Reserve(Count);
			if (Pattern == TEXT("ring"))
			{
				const double Span = EndAngle - StartAngle;
				const bool bFull = FMath::Abs(Span) >= 360.0 - KINDA_SMALL_NUMBER;
				const double Step = bFull ? Span / Count : (Count > 1 ? Span / (Count - 1) : 0.0);
				for (int32 i = 0; i < Count; ++i)
				{
					const double Th = FMath::DegreesToRadians(StartAngle + i * Step);
					FPoint P;
					P.Pos = FVector(Radius * FMath::Cos(Th), RadiusY * FMath::Sin(Th), 0);
					P.FaceYaw = YawOfNormal(FMath::Cos(Th) / Radius, FMath::Sin(Th) / RadiusY);
					Points.Add(P);
				}
			}
			else if (Pattern == TEXT("line"))
			{
				const double Yaw = YawOfNormal(To.X, To.Y);
				for (int32 i = 0; i < Count; ++i)
				{
					const double T = Count > 1 ? (double)i / (Count - 1) : 0.5;
					FPoint P;
					P.Pos = To * T;
					P.FaceYaw = Yaw - 90.0; // "outward" = the right-hand side of the walking direction
					Points.Add(P);
				}
			}
			else if (Pattern == TEXT("grid"))
			{
				for (int32 r = 0; r < Rows; ++r)
				{
					for (int32 c = 0; c < Cols; ++c)
					{
						FPoint P;
						P.Pos = FVector((r - (Rows - 1) * 0.5) * Spacing[0], (c - (Cols - 1) * 0.5) * Spacing[1], 0);
						P.FaceYaw = 0;
						Points.Add(P);
					}
				}
			}
			else // scatter
			{
				const double InFrac = InnerRadius / Radius;
				int32 Attempts = 0;
				const int32 MaxAttempts = Count * 40 + 40;
				while (Points.Num() < Count && Attempts < MaxAttempts)
				{
					++Attempts;
					const double Th = Rnd.FRand() * 2.0 * PI;
					const double Rr = FMath::Sqrt(FMath::Lerp(InFrac * InFrac, 1.0, Rnd.FRand()));
					FPoint P;
					P.Pos = FVector(Radius * Rr * FMath::Cos(Th), RadiusY * Rr * FMath::Sin(Th), 0);
					P.FaceYaw = YawOfNormal(FMath::Cos(Th) / Radius, FMath::Sin(Th) / RadiusY);
					if (MinDist > 0)
					{
						bool bClose = false;
						for (const FPoint& Q : Points) { if (FVector::DistXY(Q.Pos, P.Pos) < MinDist) { bClose = true; break; } }
						if (bClose) { continue; }
					}
					Points.Add(P);
				}
			}

			FCollisionQueryParams Query(SCENE_QUERY_STAT(EWSceneDress), false);
			for (AActor* Old : OldDress) { Query.AddIgnoredActor(Old); }

			double TotalWeight = 0;
			for (const FMeshInfo& M : Meshes) { TotalWeight += M.Weight; }

			TArray<FPlaced> Placed;
			int32 SkippedGround = 0, SkippedOverlap = 0, SkippedAvoid = 0, Candidates = Points.Num();
			for (int32 i = 0; i < Points.Num(); ++i)
			{
				const FPoint& P = Points[i];
				int32 MeshIdx = 0;
				if (Meshes.Num() > 1)
				{
					if (bCycle) { MeshIdx = i % Meshes.Num(); }
					else
					{
						double Pickv = Rnd.FRand() * TotalWeight;
						for (int32 m = 0; m < Meshes.Num(); ++m) { if (Pickv < Meshes[m].Weight || m == Meshes.Num() - 1) { MeshIdx = m; break; } Pickv -= Meshes[m].Weight; }
					}
				}
				FVector Local = P.Pos;
				if (PosJitter > 0) { Local.X += (Rnd.FRand() * 2 - 1) * PosJitter; Local.Y += (Rnd.FRand() * 2 - 1) * PosJitter; }
				Local += Offset;
				double Yaw = YawExtra;
				if (Facing == TEXT("outward")) { Yaw += P.FaceYaw; }
				else if (Facing == TEXT("inward")) { Yaw += P.FaceYaw + 180.0; }
				else if (Facing == TEXT("tangent")) { Yaw += P.FaceYaw + 90.0; }
				FRotator Rot(BasePitch, Yaw, BaseRoll);
				if (bRotJitter)
				{
					Rot.Pitch += (Rnd.FRand() * 2 - 1) * RotJitter.X;
					Rot.Yaw += (Rnd.FRand() * 2 - 1) * RotJitter.Y;
					Rot.Roll += (Rnd.FRand() * 2 - 1) * RotJitter.Z;
				}
				const double SJ = ScaleJitter > 0 ? 1.0 + (Rnd.FRand() * 2 - 1) * ScaleJitter : 1.0;
				const FVector Scale = BaseScale * SJ;

				FTransform W = FTransform(Rot, Local) * Frame;
				const FMeshInfo& MI = Meshes[MeshIdx];
				if (bSnap)
				{
					FHitResult Hit;
					const FVector From = W.GetLocation() + FVector(0, 0, TraceUp), ToP = W.GetLocation() - FVector(0, 0, TraceDown);
					if (!World->LineTraceSingleByChannel(Hit, From, ToP, ECC_Visibility, Query)) { ++SkippedGround; continue; }
					FVector L = W.GetLocation();
					L.Z = Hit.ImpactPoint.Z + (bBaseAlign && MI.Bounds.IsValid ? -MI.Bounds.Min.Z * Scale.Z : 0.0);
					W.SetLocation(L);
				}
				W.SetScale3D(Scale);

				// Footprint: a circle around the mesh bounds center, big enough for any turn.
				const FVector BC = MI.Bounds.IsValid ? MI.Bounds.GetCenter() : FVector::ZeroVector;
				const FVector BE = MI.Bounds.IsValid ? MI.Bounds.GetExtent() : FVector::ZeroVector;
				const FVector WC = W.TransformPosition(BC);
				const double Rad = FVector2D(BE.X * Scale.X, BE.Y * Scale.Y).Size();
				bool bAvoided = false;
				for (const FBox& B : AvoidBoxes)
				{
					const double Pad = Rad + Clearance;
					if (WC.X >= B.Min.X - Pad && WC.X <= B.Max.X + Pad && WC.Y >= B.Min.Y - Pad && WC.Y <= B.Max.Y + Pad) { bAvoided = true; break; }
				}
				if (bAvoided) { ++SkippedAvoid; continue; }
				if (bAvoidOverlap)
				{
					bool bHit = false;
					for (const FPlaced& Q : Placed)
					{
						if (FVector2D::Distance(Q.Center2D, FVector2D(WC.X, WC.Y)) < Q.Radius + Rad + Clearance) { bHit = true; break; }
					}
					if (bHit) { ++SkippedOverlap; continue; }
				}
				FPlaced Pl;
				Pl.World = W;
				Pl.Local = W.GetRelativeTransform(Frame);
				Pl.LocalRot = Rot;
				Pl.MeshIdx = MeshIdx;
				Pl.Center2D = FVector2D(WC.X, WC.Y);
				Pl.Radius = Rad;
				Placed.Add(Pl);
				Meshes[MeshIdx].Placed++;
			}
			if (Placed.Num() == 0)
			{
				return Bad(TEXT("NOTHING_PLACED"), FString::Printf(TEXT("All %d candidates were skipped (no ground %d, overlap %d, avoid %d). Nothing was changed."), Candidates, SkippedGround, SkippedOverlap, SkippedAvoid));
			}

			// ---- write: one transaction for the level and the table
			FScopedTransaction Tx(FText::FromString(TEXT("Enginewright scene_dress")));
			TArray<TSharedPtr<FJsonValue>> ActorNames;
			int32 RemovedOld = 0;
			int32 ReadBackCount = 0;
			FString Label = EW::Str(Args, TEXT("label"));
			if (Label.IsEmpty()) { Label = Name; }
			FString Folder = EW::Str(Args, TEXT("folder"));
			if (Folder.IsEmpty()) { Folder = FString::Printf(TEXT("Dress/%s"), *Name); }

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			FEWResult Res = FEWResult::Ok();

			// Export first: it is the step that can still fail on a bad value, and it restores itself.
			TArray<TSharedPtr<FJsonValue>> SampleRows;
			int32 RowsWritten = 0, RowsRemoved = 0;
			if (ExportTable)
			{
				const UScriptStruct* RS = ExportTable->GetRowStruct();
				const int32 Digits = FMath::Max(3, FString::FromInt(Placed.Num()).Len());
				const FString RowPrefix = ExPrefix + TEXT("_");
				if (ExMode == TEXT("add"))
				{
					// Check before anything is touched: the table stays exactly as it was.
					for (int32 i = 0; i < Placed.Num(); ++i)
					{
						const FName RowName(*FString::Printf(TEXT("%s%0*d"), *RowPrefix, Digits, i + 1));
						if (ExportTable->FindRowUnchecked(RowName))
						{
							EW::CancelUnlessBatch(Tx);
							return Bad(TEXT("ROW_EXISTS"), FString::Printf(TEXT("Row '%s' already exists (export.mode add). Nothing was placed or written; use mode replace or another prefix."), *RowName.ToString()));
						}
					}
				}
				const FString Snapshot = ExportTable->GetTableAsJSON();
				ExportTable->Modify();
				FDataTableEditorUtils::BroadcastPreChange(ExportTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
				auto Restore = [&]()
				{
					ExportTable->CreateTableFromJSONString(Snapshot);
					FDataTableEditorUtils::BroadcastPostChange(ExportTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
					EW::CancelUnlessBatch(Tx);
				};
				if (ExMode == TEXT("replace"))
				{
					TArray<FName> Gone;
					for (const FName& N : ExportTable->GetRowNames()) { if (N.ToString().StartsWith(RowPrefix)) { Gone.Add(N); } }
					for (const FName& N : Gone) { FDataTableEditorUtils::RemoveRow(ExportTable, N); }
					RowsRemoved = Gone.Num();
				}
				for (int32 i = 0; i < Placed.Num(); ++i)
				{
					const FName RowName(*FString::Printf(TEXT("%s%0*d"), *RowPrefix, Digits, i + 1));
					if (ExMode == TEXT("add") && ExportTable->FindRowUnchecked(RowName))
					{
						Restore();
						return Bad(TEXT("ROW_EXISTS"), FString::Printf(TEXT("Row '%s' already exists (export.mode add). The table was restored and nothing was placed."), *RowName.ToString()));
					}
					uint8* Mem = ExportTable->FindRowUnchecked(RowName);
					if (!Mem) { Mem = FDataTableEditorUtils::AddRow(ExportTable, RowName); }
					if (!Mem) { Restore(); return Bad(TEXT("ADD_ROW_FAILED"), FString::Printf(TEXT("Could not add row '%s'. The table was restored."), *RowName.ToString())); }
					const FTransform& L = Placed[i].Local;
					const FRotator LR = Placed[i].LocalRot;
					const FVector LS = L.GetScale3D();
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					if (ExFields.IsValid()) { for (const auto& Pair : ExFields->Values) { Row->SetField(FString(*Pair.Key), Pair.Value); } }
					if (!ExScene.IsEmpty()) { Row->SetStringField(ColScene, ExScene); }
					Row->SetStringField(ColMesh, Meshes[Placed[i].MeshIdx].Path);
					TSharedRef<FJsonObject> JL = MakeShared<FJsonObject>(), JR = MakeShared<FJsonObject>(), JS = MakeShared<FJsonObject>();
					JL->SetNumberField(TEXT("X"), L.GetLocation().X); JL->SetNumberField(TEXT("Y"), L.GetLocation().Y); JL->SetNumberField(TEXT("Z"), L.GetLocation().Z);
					JR->SetNumberField(TEXT("Pitch"), LR.Pitch); JR->SetNumberField(TEXT("Yaw"), LR.Yaw); JR->SetNumberField(TEXT("Roll"), LR.Roll);
					JS->SetNumberField(TEXT("X"), LS.X); JS->SetNumberField(TEXT("Y"), LS.Y); JS->SetNumberField(TEXT("Z"), LS.Z);
					Row->SetObjectField(ColLoc, JL);
					Row->SetObjectField(ColRot, JR);
					Row->SetObjectField(ColScale, JS);
					FEWResult One = EW::WriteValues(RS, Mem, nullptr, Row);
					if (!One.bOk)
					{
						Restore();
						One.Message = FString::Printf(TEXT("Row '%s': %s The table was restored and nothing was placed."), *RowName.ToString(), *One.Message);
						return One;
					}
					++RowsWritten;
					if (SampleRows.Num() < 3)
					{
						TSharedPtr<FJsonObject> Back;
						EW::ReadValues(RS, Mem, {}, Back);
						TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
						S->SetStringField(TEXT("row"), RowName.ToString());
						if (Back.IsValid()) { S->SetObjectField(TEXT("values"), Back); }
						SampleRows.Add(MakeShared<FJsonValueObject>(S));
					}
				}
				FDataTableEditorUtils::BroadcastPostChange(ExportTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
				ExportTable->MarkPackageDirty();
				bool bDeferred = false, bSaved = false;
				if (bExSave)
				{
					bSaved = EW::SaveOrDefer(ExportTable->GetOutermost(), bDeferred);
					if (!bSaved) { Res.Warnings.Add(TEXT("The table was changed but saving failed; the change is in memory only.")); }
				}
				TSharedRef<FJsonObject> Ex = MakeShared<FJsonObject>();
				Ex->SetStringField(TEXT("table"), ExportTable->GetPathName());
				Ex->SetNumberField(TEXT("rowsWritten"), RowsWritten);
				Ex->SetNumberField(TEXT("rowsRemoved"), RowsRemoved);
				Ex->SetNumberField(TEXT("rowsInTable"), ExportTable->GetRowNames().Num());
				Ex->SetBoolField(TEXT("saved"), bSaved && !bDeferred);
				Ex->SetArrayField(TEXT("sample"), SampleRows);
				Data->SetObjectField(TEXT("export"), Ex);
			}

			if (Output != TEXT("none"))
			{
				for (AActor* Old : OldDress) { World->EditorDestroyActor(Old, true); ++RemovedOld; }

				if (Output == TEXT("instanced"))
				{
					FActorSpawnParameters P;
					P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					const FTransform ActorXf(Frame.GetRotation(), Frame.GetLocation());
					AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), ActorXf, P);
					if (!A) { EW::CancelUnlessBatch(Tx); return Bad(TEXT("SPAWN_FAILED"), TEXT("The engine did not spawn the dressing actor. Nothing was changed.")); }
					A->Modify();
					USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("DressRoot"), RF_Transactional);
					A->SetRootComponent(Root);
					Root->SetWorldTransform(ActorXf);
					Root->RegisterComponent();
					A->AddInstanceComponent(Root);
					for (int32 m = 0; m < Meshes.Num(); ++m)
					{
						if (Meshes[m].Placed == 0) { continue; }
						UHierarchicalInstancedStaticMeshComponent* C = NewObject<UHierarchicalInstancedStaticMeshComponent>(A, *FString::Printf(TEXT("Dress_%s"), *Meshes[m].Mesh->GetName()), RF_Transactional);
						C->SetStaticMesh(Meshes[m].Mesh);
						C->SetupAttachment(Root);
						C->SetCollisionProfileName(bCollide ? TEXT("BlockAll") : TEXT("NoCollision"));
						C->SetCanEverAffectNavigation(false);
						C->RegisterComponent();
						A->AddInstanceComponent(C);
						TArray<FTransform> Xfs;
						for (const FPlaced& Pl : Placed) { if (Pl.MeshIdx == m) { Xfs.Add(Pl.Local); } }
						C->AddInstances(Xfs, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false);
						ReadBackCount += C->GetInstanceCount();
					}
					A->Tags.AddUnique(DressTag);
					A->SetActorLabel(Label);
					A->SetFolderPath(FName(*Folder));
					A->MarkPackageDirty();
					ActorNames.Add(MakeShared<FJsonValueString>(A->GetActorLabel()));
				}
				else // actors
				{
					int32 Index = 0;
					const int32 Digits = FMath::Max(3, FString::FromInt(Placed.Num()).Len());
					for (const FPlaced& Pl : Placed)
					{
						FActorSpawnParameters P;
						P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
						AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Pl.World, P);
						if (!A) { EW::CancelUnlessBatch(Tx); return Bad(TEXT("SPAWN_FAILED"), TEXT("The engine did not spawn a mesh actor. The undo step was cancelled.")); }
						UStaticMeshComponent* C = A->GetStaticMeshComponent();
						C->SetMobility(EComponentMobility::Movable);
						C->SetStaticMesh(Meshes[Pl.MeshIdx].Mesh);
						C->SetMobility(EComponentMobility::Static);
						C->SetCollisionProfileName(bCollide ? TEXT("BlockAll") : TEXT("NoCollision"));
						A->Tags.AddUnique(DressTag);
						A->SetActorLabel(FString::Printf(TEXT("%s_%0*d"), *Label, Digits, ++Index));
						A->SetFolderPath(FName(*Folder));
						A->MarkPackageDirty();
						++ReadBackCount;
						if (ActorNames.Num() < 5) { ActorNames.Add(MakeShared<FJsonValueString>(A->GetActorLabel())); }
					}
				}
			}

			// ---- reply (read back where the engine holds the result)
			Data->SetStringField(TEXT("name"), Name);
			Data->SetStringField(TEXT("pattern"), Pattern);
			Data->SetStringField(TEXT("output"), Output);
			Data->SetNumberField(TEXT("candidates"), Candidates);
			Data->SetNumberField(TEXT("placed"), Placed.Num());
			if (Output != TEXT("none")) { Data->SetNumberField(TEXT("inLevel"), ReadBackCount); Data->SetNumberField(TEXT("replacedActors"), RemovedOld); Data->SetArrayField(TEXT("actors"), ActorNames); }
			TSharedRef<FJsonObject> Skipped = MakeShared<FJsonObject>();
			Skipped->SetNumberField(TEXT("noGround"), SkippedGround);
			Skipped->SetNumberField(TEXT("overlap"), SkippedOverlap);
			Skipped->SetNumberField(TEXT("avoid"), SkippedAvoid);
			Data->SetObjectField(TEXT("skipped"), Skipped);
			TSharedRef<FJsonObject> PerMesh = MakeShared<FJsonObject>();
			for (const FMeshInfo& M : Meshes) { PerMesh->SetNumberField(M.Path, M.Placed); }
			Data->SetObjectField(TEXT("perMesh"), PerMesh);
			FBox Area(ForceInit);
			for (const FPlaced& Pl : Placed) { Area += Pl.World.GetLocation(); }
			Data->SetArrayField(TEXT("areaMin"), Vec(Area.Min));
			Data->SetArrayField(TEXT("areaMax"), Vec(Area.Max));
			Data->SetArrayField(TEXT("frameLocation"), Vec(Frame.GetLocation()));
			if (Placed.Num() < Candidates) { Res.Warnings.Add(FString::Printf(TEXT("%d of %d candidates were skipped (see skipped)."), Candidates - Placed.Num(), Candidates)); }
			if (Pattern == TEXT("scatter") && Candidates < Count) { Res.Warnings.Add(FString::Printf(TEXT("Only %d of %d scatter points fit with minDistance %.0f."), Candidates, Count, MinDist)); }
			if (Output != TEXT("none") && ReadBackCount != Placed.Num()) { Res.Warnings.Add(FString::Printf(TEXT("Read-back: the level holds %d meshes, %d were placed."), ReadBackCount, Placed.Num())); }
			Res.Data = Data;
			return Res;
		});
}
