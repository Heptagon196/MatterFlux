#include "ForgeAuxFixture.h"
#include "Fragment/Fragment2DAsset.h"
#include "ForgeMvpFixture.h"

#include "ForgeBindingRegistry.h"
#include "IForgeRuntimeModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const FForgeFunctionBinding* RequireBinding(
	FAutomationTestBase& Test,
	const TCHAR* QualifiedName)
{
	const FForgeFunctionBinding* Binding =
		IForgeRuntimeModule::Get().GetBindingRegistry().FindFunctionByName(QualifiedName);
	if (Binding == nullptr)
	{
		Test.AddError(FString::Printf(
			TEXT("Generated Forge binding is missing or ambiguous: %s"),
			QualifiedName));
	}
	return Binding;
}

FForgePatchResult ApplyPatch(
	const FForgeFunctionBinding& Binding,
	const TCHAR* Script,
	EForgePatchMode Mode)
{
	return IForgeRuntimeModule::Get().ApplyPatch(
		Binding.SymbolId,
		Script,
		TEXT("Patch"),
		Mode,
		Binding.BuildFingerprint);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMatterFluxLatestMultiFileHotReloadTest,
	"MatterFlux.Forge.LatestMultiFileHotReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMatterFluxLatestMultiFileHotReloadTest::RunTest(const FString& Parameters)
{
	const FForgeFunctionBinding* Containers = RequireBinding(
		*this, TEXT("ForgeAuxFixture::AdvanceProjectValues"));
	const FForgeFunctionBinding* Templates = RequireBinding(
		*this, TEXT("ForgeAuxFixture::ApplyProjectTemplate"));
	const FForgeFunctionBinding* AssetWidth = RequireBinding(
		*this, TEXT("UFragment2DAsset::GetClampedWidth"));
	const FForgeFunctionBinding* Damage = RequireBinding(
		*this, TEXT("ForgeMvpFixture::CalculateDamage"));
	if (Containers == nullptr || Templates == nullptr || AssetWidth == nullptr || Damage == nullptr)
	{
		return false;
	}

	IForgeRuntimeModule& Runtime = IForgeRuntimeModule::Get();
	auto DisableAll = [&]()
	{
		Runtime.DisablePatch(Containers->SymbolId);
		Runtime.DisablePatch(Templates->SymbolId);
		Runtime.DisablePatch(AssetWidth->SymbolId);
		Runtime.DisablePatch(Damage->SymbolId);
	};
	ON_SCOPE_EXIT
	{
		DisableAll();
	};
	auto RequireInstalled = [&](const FForgePatchResult& Result, const TCHAR* Label)
	{
		TestTrue(Label, Result.bSuccess);
		if (!Result.bSuccess)
		{
			AddError(Result.Error);
		}
		return Result.bSuccess;
	};

	DisableAll();
	TStrongObjectPtr<UFragment2DAsset> Asset(NewObject<UFragment2DAsset>());
	Asset->MaskWidth = 400;
	TestEqual(TEXT("Native project TArray implementation"),
		ForgeAuxFixture::AdvanceProjectValues({ 1, 4 }), TArray<int32>({ 5, 8 }));
	TestEqual(TEXT("Native concrete project-template call site"),
		ForgeAuxFixture::ApplyProjectTemplate(2, 3), 12);
	TestEqual(TEXT("Native real UObject method"), Asset->GetClampedWidth(), 256);
	TestEqual(TEXT("Native third translation unit"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f), 75.0f);

	FForgePatchResult Result = ApplyPatch(
		*Containers,
		TEXT("TArray_int Patch(const TArray_int &in Values) { "
			 "TArray_int Result = Values; Result.Set(0, Result.At(0) + 10); "
			 "Result.Add(101); return Result; }"),
		EForgePatchMode::Replace);
	if (!RequireInstalled(Result, TEXT("Revision 1 installs for project TArray source"))) return false;
	Result = ApplyPatch(
		*Templates,
		TEXT("int Patch(int Base, int Value) { return Base + Value + 100; }"),
		EForgePatchMode::Replace);
	if (!RequireInstalled(Result, TEXT("Revision 1 installs for project-template source"))) return false;
	Result = ApplyPatch(
		*AssetWidth,
		TEXT("int Patch() { return 17; }"),
		EForgePatchMode::Replace);
	if (!RequireInstalled(Result, TEXT("Revision 1 installs for real UObject source"))) return false;
	Result = ApplyPatch(
		*Damage,
		TEXT("float Patch(float BaseDamage, float Distance) { return BaseDamage + Distance; }"),
		EForgePatchMode::Replace);
	if (!RequireInstalled(Result, TEXT("Revision 1 installs for third translation unit"))) return false;

	TestEqual(TEXT("Revision 1 changes project TArray flow"),
		ForgeAuxFixture::AdvanceProjectValues({ 1, 4 }), TArray<int32>({ 11, 4, 101 }));
	TestEqual(TEXT("Revision 1 changes project-template source"),
		ForgeAuxFixture::ApplyProjectTemplate(2, 3), 105);
	TestEqual(TEXT("Revision 1 changes real UObject method"), Asset->GetClampedWidth(), 17);
	TestEqual(TEXT("Revision 1 changes third translation unit"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f), 125.0f);

	Result = ApplyPatch(
		*Containers,
		TEXT("TArray_int Patch(const TArray_int &in Values) { "
			 "TArray_int Result = Original(Values); "
			 "Result.Set(0, Result.At(0) + 20); Result.Add(202); return Result; }"),
		EForgePatchMode::Wrap);
	if (!RequireInstalled(Result, TEXT("Revision 2 replaces project TArray revision 1"))) return false;
	Result = ApplyPatch(
		*Templates,
		TEXT("int Patch(int Base, int Value) { return Original(Base, Value) + 200; }"),
		EForgePatchMode::Wrap);
	if (!RequireInstalled(Result, TEXT("Revision 2 replaces project-template revision 1"))) return false;
	Result = ApplyPatch(
		*AssetWidth,
		TEXT("int Patch() { return Original() - 6; }"),
		EForgePatchMode::Wrap);
	if (!RequireInstalled(Result, TEXT("Revision 2 replaces real UObject revision 1"))) return false;
	Result = ApplyPatch(
		*Damage,
		TEXT("float Patch(float BaseDamage, float Distance) { "
			 "return Original(BaseDamage, Distance) * 2.0f; }"),
		EForgePatchMode::Wrap);
	if (!RequireInstalled(Result, TEXT("Revision 2 replaces third translation unit revision 1"))) return false;

	TestEqual(TEXT("Revision 2 calls native project TArray original"),
		ForgeAuxFixture::AdvanceProjectValues({ 1, 4 }), TArray<int32>({ 25, 8, 202 }));
	TestEqual(TEXT("Revision 2 calls native project-template original"),
		ForgeAuxFixture::ApplyProjectTemplate(2, 3), 212);
	TestEqual(TEXT("Revision 2 calls native UObject original"), Asset->GetClampedWidth(), 250);
	TestEqual(TEXT("Revision 2 calls third translation unit original"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f), 150.0f);

	DisableAll();
	TestEqual(TEXT("Disable restores project TArray source"),
		ForgeAuxFixture::AdvanceProjectValues({ 1, 4 }), TArray<int32>({ 5, 8 }));
	TestEqual(TEXT("Disable restores project-template source"),
		ForgeAuxFixture::ApplyProjectTemplate(2, 3), 12);
	TestEqual(TEXT("Disable restores real UObject method"), Asset->GetClampedWidth(), 256);
	TestEqual(TEXT("Disable restores third translation unit"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f), 75.0f);
	return true;
}

#endif
