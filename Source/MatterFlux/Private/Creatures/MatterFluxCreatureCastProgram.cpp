#include "Creatures/MatterFluxCreatureCastProgram.h"

bool FMatterFluxCreatureCastPlanner::Build(
	const FMatterFluxCreatureCastProgramDefinition& Program,
	const FVector& AimDirection,
	const int32 EventSeed,
	TArray<FMatterFluxCreatureCastShot>& OutShots,
	FString& OutError)
{
	OutShots.Reset();
	OutError.Reset();
	if (Program.ProjectileCount < 1 || Program.ProjectileCount > 32
		|| !FMath::IsFinite(Program.SpreadDegrees)
		|| Program.SpreadDegrees < 0.0f || Program.SpreadDegrees > 180.0f
		|| !FMath::IsFinite(Program.ProjectileInterval)
		|| Program.ProjectileInterval < 0.0f)
	{
		OutError = TEXT("creature cast program contains invalid volley data");
		return false;
	}

	FVector Aim = AimDirection;
	Aim.Z = 0.0f;
	Aim = Aim.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
	FRandomStream Random(EventSeed);
	OutShots.Reserve(Program.ProjectileCount);
	for (int32 Index = 0; Index < Program.ProjectileCount; ++Index)
	{
		FMatterFluxCreatureCastShot& Shot = OutShots.AddDefaulted_GetRef();
		if (Program.bRadial)
		{
			Shot.Direction = FVector::ForwardVector.RotateAngleAxis(
				360.0f * static_cast<float>(Index)
					/ static_cast<float>(Program.ProjectileCount),
				FVector::UpVector);
		}
		else
		{
			const float Angle = Program.SpreadDegrees > 0.0f
				? Random.FRandRange(-Program.SpreadDegrees, Program.SpreadDegrees)
				: 0.0f;
			Shot.Direction = Aim.RotateAngleAxis(Angle, FVector::UpVector);
		}
		Shot.Direction.Normalize();
		Shot.DelaySeconds = Program.ProjectileInterval * Index;
		Shot.EventSeed = EventSeed ^ (Index * 0x4f31 + 0x153);
	}
	return true;
}
