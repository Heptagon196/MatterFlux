#include "ForgeMvpFixture.h"
#include "ForgeAuxFixture.h"
#include "ForgeReflectionFixtureObject.h"
#include "ForgeGenerated/MatterFluxTests.forge.manifest.h"
#include "../../MatterFlux/Private/ForgeGenerated/MatterFlux.forge.manifest.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "IForgeRuntimeModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
const FString ForgeMvpSymbolId =
	TEXT("sha256:99eb31ebbb910d8643029ddfbb8721c5ead4b460a01f85938ac9ee7a24b0f7be");
const FString ForgeMemberScaleSymbolId =
	TEXT("sha256:fa641992b9223ca0d7bf61714018168c0ae7aaf76e57ad2928040950c0e74be4");
const FString ForgeStaticClampSymbolId =
	TEXT("sha256:ffc993ee2d71f368f8063652fd0d4f56931256646505a542efaa520c4ddd8231");
const FString ForgeDescribeImpactSymbolId =
	TEXT("sha256:4d2141dcfe25ce233cf5612aea39e680d07c54b3e08351595b7aa6a16a756e63");
const FString ForgeOffsetPositionSymbolId =
	TEXT("sha256:0ee947e2535cd0b373ad1f3c03c39deea9b2e88f81f2e8690c899fddb6e59008");
const FString ForgeBoostImpactSymbolId =
	TEXT("sha256:c3086efbe4aa3306eebd8fcaa57e71c04fc8182016e3eccc78bef5c84c77dff6");
const FString ForgeOppositeImpactSymbolId =
	TEXT("sha256:de31f1e71a339be7b230d80b6f2e312a68bbbb472e99c2268cfddafd17a93488");
const FString ForgeInvokeReflectedFloatSymbolId =
	TEXT("sha256:18b286874f26ca78865dee8cc39c2f24b183e4cda6a4626bdebb5eb0c91a9e54");
const FString ForgePrivateSecretScaleSymbolId =
	TEXT("sha256:3d7c51d2492f7f0dc59420b23f56a94157bd19e8bb67b95c8ed2db03f321380b");
const FString ForgeInternalBiasSymbolId =
	TEXT("sha256:900d6bc06d4c3eb1850a666b801ac34687d7bbd7cec852903a5a39ef78ddb581");
const FString ForgeCrossModuleSymbolId =
	TEXT("sha256:a1f12eae91660732d06e50662480d2a6a4d4f472397a6842111cac5553f545d1");

bool IsForgeWorkspaceConfigured()
{
	FString WorkspaceRoot;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ForgeWorkspace="), WorkspaceRoot))
	{
		WorkspaceRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("FORGE_WORKSPACE"));
	}
	return !WorkspaceRoot.IsEmpty();
}
}

DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(
	FWaitForForgeFixtureValue,
	FAutomationTestBase*, Test,
	float, ExpectedValue,
	double, DeadlineSeconds);

bool FWaitForForgeFixtureValue::Update()
{
	const float ActualValue = ForgeMvpFixture::CalculateDamage(100.0f, 25.0f);
	if (FMath::IsNearlyEqual(ActualValue, ExpectedValue))
	{
		return true;
	}
	if (FPlatformTime::Seconds() >= DeadlineSeconds)
	{
		Test->AddError(FString::Printf(
			TEXT("Timed out waiting for Forge fixture value %.2f; actual %.2f"),
			ExpectedValue,
			ActualValue));
		return true;
	}
	return false;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FLogForgeE2EPhase, const TCHAR*, Phase);

bool FLogForgeE2EPhase::Update()
{
	UE_LOG(LogTemp, Display, TEXT("FORGE_E2E_%s"), Phase);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMvpNativeFixtureTest,
	"MatterFlux.Forge.NativeFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMvpNativeFixtureTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Unpatched fixture uses the native implementation"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		75.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMvpRuntimePatchTest,
	"MatterFlux.Forge.RuntimePatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMvpRuntimePatchTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeMvpSymbolId);
	};

	ForgeRuntime.DisablePatch(ForgeMvpSymbolId);
	TestEqual(
		TEXT("Fixture starts with the native implementation"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		75.0f);

	const FForgePatchResult WrapResult = ForgeRuntime.ApplyPatch(
		ForgeMvpSymbolId,
		TEXT("float Patch_CalculateDamage(float BaseDamage, float Distance) "
			 "{ return Original(BaseDamage, Distance) * 2.0f; }"),
		TEXT("Patch_CalculateDamage"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Wrap Patch compiles and installs"), WrapResult.bSuccess);
	if (!WrapResult.bSuccess)
	{
		AddError(WrapResult.Error);
		return false;
	}
	TestEqual(
		TEXT("Wrap Patch can call Original"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		150.0f);

	const FForgePatchResult ReplaceResult = ForgeRuntime.ApplyPatch(
		ForgeMvpSymbolId,
		TEXT("float Patch_CalculateDamage(float BaseDamage, float Distance) "
			 "{ return BaseDamage + Distance; }"),
		TEXT("Patch_CalculateDamage"),
		EForgePatchMode::Replace,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Replace Patch compiles and replaces native behavior"), ReplaceResult.bSuccess);
	TestEqual(
		TEXT("Replace Patch does not need Original"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		125.0f);

	const FForgePatchResult InvalidReplaceResult = ForgeRuntime.ApplyPatch(
		ForgeMvpSymbolId,
		TEXT("float Patch_CalculateDamage(float BaseDamage, float Distance) "
			 "{ return Original(BaseDamage, Distance); }"),
		TEXT("Patch_CalculateDamage"),
		EForgePatchMode::Replace,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestFalse(TEXT("Replace Patch cannot compile a call to Original"), InvalidReplaceResult.bSuccess);
	TestEqual(
		TEXT("Rejected Replace Patch leaves the previous Replace active"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		125.0f);

	const FForgePatchResult ReloadResult = ForgeRuntime.ApplyPatch(
		ForgeMvpSymbolId,
		TEXT("float Patch_CalculateDamage(float BaseDamage, float Distance) "
			 "{ return Original(BaseDamage, Distance) + 10.0f; }"),
		TEXT("Patch_CalculateDamage"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("A second Patch replaces the active Patch"), ReloadResult.bSuccess);
	if (!ReloadResult.bSuccess)
	{
		AddError(ReloadResult.Error);
		return false;
	}
	TestEqual(
		TEXT("Reloaded Patch is active"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		85.0f);

	const FForgePatchResult InvalidScriptResult = ForgeRuntime.ApplyPatch(
		ForgeMvpSymbolId,
		TEXT("float Patch_CalculateDamage(float BaseDamage, float Distance) { invalid }"),
		TEXT("Patch_CalculateDamage"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestFalse(TEXT("An invalid Patch is rejected"), InvalidScriptResult.bSuccess);
	TestEqual(
		TEXT("A rejected Patch leaves the previous Patch active"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		85.0f);

	const FForgePatchResult StaleFingerprintResult = ForgeRuntime.ApplyPatch(
		ForgeMvpSymbolId,
		TEXT("float Patch_CalculateDamage(float BaseDamage, float Distance) "
			 "{ return Original(BaseDamage, Distance); }"),
		TEXT("Patch_CalculateDamage"),
		EForgePatchMode::Wrap,
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	TestFalse(TEXT("A stale Build Fingerprint is rejected"), StaleFingerprintResult.bSuccess);
	TestEqual(
		TEXT("A stale Patch leaves the previous Patch active"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		85.0f);

	ForgeRuntime.DisablePatch(ForgeMvpSymbolId);
	TestEqual(
		TEXT("Disabling a Patch restores the native implementation"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		75.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMemberFunctionPatchTest,
	"MatterFlux.Forge.MemberFunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMemberFunctionPatchTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeMemberScaleSymbolId);
		ForgeRuntime.DisablePatch(ForgeStaticClampSymbolId);
	};

	ForgeMvpFixture::FDamageCalculator Calculator(2.0f);
	TestEqual(TEXT("Native instance method uses object state"), Calculator.Scale(10.0f), 20.0f);
	const FForgePatchResult MemberResult = ForgeRuntime.ApplyPatch(
		ForgeMemberScaleSymbolId,
		TEXT("float Patch_Scale(float Value) { return Original(Value) + 5.0f; }"),
		TEXT("Patch_Scale"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Const instance member Wrap installs"), MemberResult.bSuccess);
	if (!MemberResult.bSuccess)
	{
		AddError(MemberResult.Error);
		return false;
	}
	TestEqual(TEXT("Instance member Original receives the native this pointer"), Calculator.Scale(10.0f), 25.0f);

	TestEqual(TEXT("Native static member clamps"), ForgeMvpFixture::FDamageCalculator::ClampToInt(120.0), 100);
	const FForgePatchResult StaticResult = ForgeRuntime.ApplyPatch(
		ForgeStaticClampSymbolId,
		TEXT("int Patch_Clamp(double Value) { return Original(Value) - 1; }"),
		TEXT("Patch_Clamp"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Static member Wrap installs"), StaticResult.bSuccess);
	if (!StaticResult.bSuccess)
	{
		AddError(StaticResult.Error);
		return false;
	}
	TestEqual(TEXT("Static member uses the typed Original overload"), ForgeMvpFixture::FDamageCalculator::ClampToInt(120.0), 99);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeValueTypePatchTest,
	"MatterFlux.Forge.ValueTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeValueTypePatchTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeDescribeImpactSymbolId);
		ForgeRuntime.DisablePatch(ForgeOffsetPositionSymbolId);
	};

	const FString NativeDescription = ForgeMvpFixture::DescribeImpact(
		TEXT("impact"), FName(TEXT("Damage")), FVector(5.0, 0.0, 0.0));
	TestEqual(TEXT("Native FString/FName/FVector call works"), NativeDescription, TEXT("impact:Damage:5"));
	const FForgePatchResult DescriptionResult = ForgeRuntime.ApplyPatch(
		ForgeDescribeImpactSymbolId,
		TEXT("FString Patch_Describe(const FString &in Label, const FName &in Category, "
			 "const FVector &in Position) { return Original(Label, Category, Position).ToUpper(); }"),
		TEXT("Patch_Describe"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("UE value-type Wrap installs"), DescriptionResult.bSuccess);
	if (!DescriptionResult.bSuccess)
	{
		AddError(DescriptionResult.Error);
		return false;
	}
	TestEqual(
		TEXT("FString/FName/FVector cross the AngelScript boundary and Original"),
		ForgeMvpFixture::DescribeImpact(TEXT("impact"), FName(TEXT("Damage")), FVector(5.0, 0.0, 0.0)),
		TEXT("IMPACT:DAMAGE:5"));

	TestEqual(
		TEXT("Native FVector return works"),
		ForgeMvpFixture::OffsetPosition(FVector(1.0, 2.0, 3.0)),
		FVector(2.0, 4.0, 6.0));
	const FForgePatchResult VectorResult = ForgeRuntime.ApplyPatch(
		ForgeOffsetPositionSymbolId,
		TEXT("FVector Patch_Offset(const FVector &in Position) "
			 "{ FVector Result = Position; Result.X += 10.0; return Result; }"),
		TEXT("Patch_Offset"),
		EForgePatchMode::Replace,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("FVector Replace installs"), VectorResult.bSuccess);
	if (!VectorResult.bSuccess)
	{
		AddError(VectorResult.Error);
		return false;
	}
	TestEqual(
		TEXT("FVector value can be copied, mutated, and returned by script"),
		ForgeMvpFixture::OffsetPosition(FVector(1.0, 2.0, 3.0)),
		FVector(11.0, 2.0, 3.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeGeneratedTypePatchTest,
	"MatterFlux.Forge.GeneratedTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeGeneratedTypePatchTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeBoostImpactSymbolId);
		ForgeRuntime.DisablePatch(ForgeOppositeImpactSymbolId);
	};

	const ForgeMvpFixture::FImpactData Native = ForgeMvpFixture::BoostImpact(
		ForgeMvpFixture::FImpactData{ 2.0f, ForgeMvpFixture::EImpactKind::Light });
	TestEqual(TEXT("Native reflected struct amount"), Native.Amount, 3.0f);
	const FForgePatchResult StructResult = ForgeRuntime.ApplyPatch(
		ForgeBoostImpactSymbolId,
		TEXT("FImpactData Patch_Boost(const FImpactData &in Value) "
			 "{ FImpactData Result = Original(Value); Result.Amount *= 3.0f; "
			 "Result.Kind = EImpactKind::Heavy; return Result; }"),
		TEXT("Patch_Boost"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Generated struct Wrap installs"), StructResult.bSuccess);
	if (!StructResult.bSuccess)
	{
		AddError(StructResult.Error);
		return false;
	}
	const ForgeMvpFixture::FImpactData Patched = ForgeMvpFixture::BoostImpact(
		ForgeMvpFixture::FImpactData{ 2.0f, ForgeMvpFixture::EImpactKind::Light });
	TestEqual(TEXT("Generated struct field crosses Original and script"), Patched.Amount, 9.0f);
	TestEqual(TEXT("Generated enum field can be assigned in script"), Patched.Kind, ForgeMvpFixture::EImpactKind::Heavy);

	const FForgePatchResult EnumResult = ForgeRuntime.ApplyPatch(
		ForgeOppositeImpactSymbolId,
		TEXT("EImpactKind Patch_Opposite(EImpactKind Value) { return EImpactKind::Heavy; }"),
		TEXT("Patch_Opposite"),
		EForgePatchMode::Replace,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Generated enum Replace installs"), EnumResult.bSuccess);
	if (!EnumResult.bSuccess)
	{
		AddError(EnumResult.Error);
		return false;
	}
	TestEqual(
		TEXT("Generated enum parameter and return use the declared enum type"),
		ForgeMvpFixture::OppositeImpact(ForgeMvpFixture::EImpactKind::Heavy),
		ForgeMvpFixture::EImpactKind::Heavy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeUObjectReflectionAdapterTest,
	"MatterFlux.Forge.UObjectAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeUObjectReflectionAdapterTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeInvokeReflectedFloatSymbolId);
	};
	TStrongObjectPtr<UForgeReflectionFixtureObject> Fixture(
		NewObject<UForgeReflectionFixtureObject>());
	Fixture->Multiplier = 3.0f;
	TestEqual(
		TEXT("Native non-UFUNCTION path remains ordinary C++"),
		ForgeMvpFixture::InvokeReflectedFloat(Fixture.Get(), GET_FUNCTION_NAME_CHECKED(UForgeReflectionFixtureObject, Scale), 4.0f),
		4.0f);

	const FForgePatchResult Result = ForgeRuntime.ApplyPatch(
		ForgeInvokeReflectedFloatSymbolId,
		TEXT("float Patch_Invoke(const UObjectHandle &in Object, const FName &in FunctionName, float Value) "
			 "{ if (!Object.IsValid()) return -2.0f; return Object.CallFloat(FunctionName, Value); }"),
		TEXT("Patch_Invoke"),
		EForgePatchMode::Replace,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Weak UObject handle and UE Reflection adapter Patch installs"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Error);
		return false;
	}
	TestEqual(
		TEXT("UObjectHandle resolves a UFunction through UE Reflection"),
		ForgeMvpFixture::InvokeReflectedFloat(Fixture.Get(), GET_FUNCTION_NAME_CHECKED(UForgeReflectionFixtureObject, Scale), 4.0f),
		12.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgePrivateAccessThunkTest,
	"MatterFlux.Forge.PrivateAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgePrivateAccessThunkTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgePrivateSecretScaleSymbolId);
	};
	ForgeMvpFixture::FDamageCalculator Calculator(2.0f);
	TestEqual(TEXT("Public wrapper reaches native private method"), Calculator.CallSecret(10.0f), 21.0f);
	const FForgePatchResult Result = ForgeRuntime.ApplyPatch(
		ForgePrivateSecretScaleSymbolId,
		TEXT("float Patch_Secret(float Value) { return Original(Value) + 7.0f; }"),
		TEXT("Patch_Secret"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Generated private access thunk installs"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Error);
		return false;
	}
	TestEqual(
		TEXT("Private method is patched without friend, macro, or export annotation"),
		Calculator.CallSecret(10.0f),
		28.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeInternalLinkageInstrumentationTest,
	"MatterFlux.Forge.InternalLinkage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeInternalLinkageInstrumentationTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeInternalBiasSymbolId);
	};
	TestEqual(TEXT("Wrapper reaches native static cpp function"), ForgeMvpFixture::CallInternalBias(5.0f), 7.0f);
	const FForgePatchResult Result = ForgeRuntime.ApplyPatch(
		ForgeInternalBiasSymbolId,
		TEXT("float Patch_Internal(float Value) { return Original(Value) * 4.0f; }"),
		TEXT("Patch_Internal"),
		EForgePatchMode::Wrap,
		MATTERFLUXTESTS_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("Same-TU generated internal bridge installs"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Error);
		return false;
	}
	TestEqual(
		TEXT("Internal-linkage function is patched through automatic cpp instrumentation"),
		ForgeMvpFixture::CallInternalBias(5.0f),
		28.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMultiModuleRegistrationTest,
	"MatterFlux.Forge.MultiModule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMultiModuleRegistrationTest::RunTest(const FString& Parameters)
{
	IForgeRuntimeModule& ForgeRuntime = IForgeRuntimeModule::Get();
	ON_SCOPE_EXIT
	{
		ForgeRuntime.DisablePatch(ForgeCrossModuleSymbolId);
	};
	TestEqual(TEXT("Native function is owned by the game module"), ForgeAuxFixture::CrossModuleValue(4.0f), 7.0f);
	const FForgePatchResult Result = ForgeRuntime.ApplyPatch(
		ForgeCrossModuleSymbolId,
		TEXT("float Patch_CrossModule(float Value) { return Original(Value) * 2.0f; }"),
		TEXT("Patch_CrossModule"),
		EForgePatchMode::Wrap,
		MATTERFLUX_FORGE_BUILD_FINGERPRINT);
	TestTrue(TEXT("A second owner module registers an independent fingerprint"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Error);
		return false;
	}
	TestEqual(TEXT("Patch dispatch crosses the second generated registrar"), ForgeAuxFixture::CrossModuleValue(4.0f), 14.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMvpWorkspacePatchTest,
	"MatterFlux.Forge.WorkspacePatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMvpWorkspacePatchTest::RunTest(const FString& Parameters)
{
	if (!IsForgeWorkspaceConfigured())
	{
		AddInfo(TEXT(
			"Skipped external Forge Workspace Patch assertion because no "
			"-ForgeWorkspace path or FORGE_WORKSPACE environment variable is configured."));
		return true;
	}

	TestEqual(
		TEXT("The Workspace watcher activates the immutable Wrap Patch"),
		ForgeMvpFixture::CalculateDamage(100.0f, 25.0f),
		150.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FForgeMvpWorkspaceHotReloadTest,
	"MatterFlux.Forge.WorkspaceHotReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FForgeMvpWorkspaceHotReloadTest::RunTest(const FString& Parameters)
{
	if (!IsForgeWorkspaceConfigured())
	{
		AddInfo(TEXT(
			"Skipped external Forge Workspace hot-reload sequence because no "
			"-ForgeWorkspace path or FORGE_WORKSPACE environment variable is configured."));
		return true;
	}

	const double Deadline = FPlatformTime::Seconds() + 120.0;
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForForgeFixtureValue(this, 150.0f, Deadline));
	ADD_LATENT_AUTOMATION_COMMAND(FLogForgeE2EPhase(TEXT("READY_FOR_RELOAD")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForForgeFixtureValue(this, 85.0f, Deadline));
	ADD_LATENT_AUTOMATION_COMMAND(FLogForgeE2EPhase(TEXT("READY_FOR_DISABLE")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForForgeFixtureValue(this, 75.0f, Deadline));
	ADD_LATENT_AUTOMATION_COMMAND(FLogForgeE2EPhase(TEXT("COMPLETE")));
	return true;
}
