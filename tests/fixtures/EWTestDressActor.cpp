#include "EWTestDressActor.h"

#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

AEWTestDressActor::AEWTestDressActor()
{
	Base = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Base"));
	RootComponent = Base;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded()) { Base->SetStaticMesh(Cube.Object); }
}

void AEWTestDressActor::BeginPlay()
{
	Super::BeginPlay();
	++BeginPlayRuns;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	for (int32 i = 0; i < RingCount; ++i)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(Cube);
		C->SetupAttachment(RootComponent);
		const double A = 2.0 * PI * i / FMath::Max(1, RingCount);
		C->SetRelativeLocation(FVector(300.0 * FMath::Cos(A), 300.0 * FMath::Sin(A), 0));
		C->SetRelativeScale3D(FVector(0.4));
		C->RegisterComponent();
	}
	UPointLightComponent* L = NewObject<UPointLightComponent>(this);
	L->SetupAttachment(RootComponent);
	L->SetRelativeLocation(FVector(0, 0, 400));
	L->SetIntensity(5000.f);
	L->RegisterComponent();
}
