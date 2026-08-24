#include "Misc/AutomationTest.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Rendering/MatterFluxGhostFade.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGhostedOccluderOutlineTest,
	"MatterFlux.Rendering.OcclusionPresentation.GhostedOccluderKeepsPlayerOutline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxGhostedOccluderOutlineTest::RunTest(const FString& Parameters)
{
	const UMaterial* RevealOutline = LoadObject<UMaterial>(
		nullptr,
		TEXT("/Game/MatterFlux/Materials/M_PlayerGhostOutline.M_PlayerGhostOutline"));
	if (!TestNotNull(
		TEXT("A stencil-only player outline exists for translucent occluders"),
		RevealOutline))
	{
		return false;
	}

	TestEqual(
		TEXT("The reveal outline is a post-process material"),
		RevealOutline->MaterialDomain,
		MD_PostProcess);

	FString CustomCode;
	for (const UMaterialExpression* Expression : RevealOutline->GetExpressions())
	{
		if (const UMaterialExpressionCustom* Custom =
			Cast<UMaterialExpressionCustom>(Expression))
		{
			CustomCode += Custom->Code;
		}
	}

	TestTrue(
		TEXT("The reveal outline identifies the player stencil"),
		CustomCode.Contains(TEXT("PlayerStencilValue = 1.0")));
	TestFalse(
		TEXT("The reveal outline does not require an opaque scene-depth blocker"),
		CustomCode.Contains(TEXT("CustomDepth > SceneDepth")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxOcclusionFadePolicyTest,
	"MatterFlux.Rendering.OcclusionPresentation.FadesGraduallyAndRemainsReadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxOcclusionFadePolicyTest::RunTest(const FString& Parameters)
{
	using namespace MatterFlux::GhostFade;
	TestTrue(
		TEXT("The default cutaway remains materially visible"),
		DefaultItemOpacity >= MinimumOpacity);

	const float FadingOut = AdvanceItemOpacity(1.0f, true, 0.1f);
	TestTrue(
		TEXT("Fade-out produces an intermediate opacity"),
		FadingOut < 1.0f && FadingOut > DefaultItemOpacity);

	const float Reversing = AdvanceItemOpacity(FadingOut, false, 0.05f);
	TestTrue(
		TEXT("Fade-in reverses continuously without snapping solid"),
		Reversing > FadingOut && Reversing < 1.0f);
	TestEqual(
		TEXT("Authored house values remain independent of item readability"),
		ResolveStructureTargetOpacity(0.025f),
		0.025f);
	return true;
}

#endif
