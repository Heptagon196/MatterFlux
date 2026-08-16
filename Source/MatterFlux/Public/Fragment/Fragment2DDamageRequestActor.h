#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/FragmentTypes.h"
#include "Fragment2DDamageRequestActor.generated.h"

class AFragment2DSourceActor;

UCLASS(Blueprintable)
class MATTERFLUX_API AFragment2DDamageRequestActor : public AActor
{
	GENERATED_BODY()

public:
	AFragment2DDamageRequestActor();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Fragment")
	bool ExecuteRequest();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TObjectPtr<AFragment2DSourceActor> SourceActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	EFragmentDamageShapeType ShapeType = EFragmentDamageShapeType::Line;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment", meta = (ClampMin = "0.0"))
	float Radius = 180.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FVector2D Extents = FVector2D(2000.0f, 120.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment", meta = (ClampMin = "1.0"))
	float Thickness = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment", meta = (ClampMin = "0.0"))
	float DamagePower = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 EventSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	bool bExecuteRequest = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
