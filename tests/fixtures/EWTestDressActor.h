// Test fixture for actor_preview (tests/test_scene.py): an actor that builds itself in code, like a game's station or deck.
// The constructor makes a cube "Base" and a game camera "Cam" (-800,0,400, fov 60); BeginPlay adds RingCount smaller cubes on a ring (radius 300 cm) and a point light.
// Copy this file and EWTestDressActor.cpp into the test project's module (for example Source/EnginewrightTest/) and rebuild.
// test_scene.py looks for /Script/EnginewrightTest.EWTestDressActor and fails the preview checks with a clear message when it is missing.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EWTestDressActor.generated.h"

UCLASS()
class AEWTestDressActor : public AActor
{
	GENERATED_BODY()

public:
	AEWTestDressActor();

	/** Number of cubes BeginPlay puts on the ring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 RingCount = 6;
	/** Counts how often BeginPlay ran (read back in the checks). */
	UPROPERTY(VisibleAnywhere) int32 BeginPlayRuns = 0;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Base;
	UPROPERTY() TObjectPtr<class UCameraComponent> Cam;
};
