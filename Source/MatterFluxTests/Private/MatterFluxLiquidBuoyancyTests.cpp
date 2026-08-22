#include "Material/MatterFluxLiquidBuoyancy.h"
#include "Misc/AutomationTest.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidDensityBuoyancyTest,
	"MatterFlux.Material.LiquidDensityDeterminesFloatOrSink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidDensityBuoyancyTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Liquid::FLiquidColumn Water;
	Water.MaterialId = TEXT("water");
	Water.Density = 1.0f;
	Water.BottomZ = 0.0f;
	Water.SurfaceZ = 100.0f;

	MatterFlux::Liquid::FBodyState Body;
	Body.BottomZ = 0.0f;
	Body.TopZ = 100.0f;
	Body.GravityZ = -980.0f;
	Body.LinearDrag = 0.0f;

	MatterFlux::Liquid::FBuoyancyResult Result;
	Body.Density = 0.5f;
	TestTrue(TEXT("A valid submerged body can be evaluated"),
		MatterFlux::Liquid::FLiquidBuoyancySolver::Evaluate(
			Body, Water, Result));
	TestTrue(TEXT("A body lighter than water has upward net acceleration"),
		Body.GravityZ + Result.Acceleration.Z > 0.0f);

	Body.Density = 2.0f;
	TestTrue(TEXT("A dense body can be evaluated"),
		MatterFlux::Liquid::FLiquidBuoyancySolver::Evaluate(
			Body, Water, Result));
	TestTrue(TEXT("A body denser than water still sinks"),
		Body.GravityZ + Result.Acceleration.Z < 0.0f);

	Body.Density = 1.0f;
	Body.Velocity = FVector(120.0f, -40.0f, 300.0f);
	Body.LinearDrag = 2.0f;
	TestTrue(TEXT("Moving body can be evaluated"),
		MatterFlux::Liquid::FLiquidBuoyancySolver::Evaluate(
			Body, Water, Result));
	TestTrue(TEXT("Liquid drag opposes horizontal X velocity"),
		Result.Acceleration.X < 0.0f);
	TestTrue(TEXT("Liquid drag opposes horizontal Y velocity"),
		Result.Acceleration.Y > 0.0f);

	Body.Density = 0.0f;
	TestFalse(TEXT("Zero body density is rejected"),
		MatterFlux::Liquid::FLiquidBuoyancySolver::Evaluate(
			Body, Water, Result));
	Water.Density = std::numeric_limits<float>::quiet_NaN();
	Body.Density = 1.0f;
	TestFalse(TEXT("Non-finite liquid density is rejected"),
		MatterFlux::Liquid::FLiquidBuoyancySolver::Evaluate(
			Body, Water, Result));
	return true;
}
