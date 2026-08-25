#include "Misc/AutomationTest.h"
#include "UI/MatterFluxToast.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxToastLifecycleTest,
	"MatterFlux.UI.Toast.IsTransientOverlayContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxToastLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TSharedRef<SMatterFluxToast> Toast = SNew(SMatterFluxToast);
	TestEqual(TEXT("A dormant toast takes no layout space"),
		Toast->GetVisibility(), EVisibility::Collapsed);

	Toast->Show(
		FText::FromString(TEXT("购买成功，物品已放入背包。")),
		EMatterFluxToastTone::Success,
		10.0f);
	TestTrue(TEXT("Showing a toast activates the overlay presentation"),
		Toast->IsShowing());
	TestEqual(TEXT("Toast keeps the purchase feedback outside its caller"),
		Toast->GetMessage().ToString(),
		FString(TEXT("购买成功，物品已放入背包。")));
	TestEqual(TEXT("Purchase success uses success semantics"),
		Toast->GetTone(), EMatterFluxToastTone::Success);
	TestEqual(TEXT("Toast never captures merchant input"),
		Toast->GetVisibility(), EVisibility::HitTestInvisible);

	Toast->Dismiss();
	TestFalse(TEXT("Dismiss stops the toast"), Toast->IsShowing());
	TestEqual(TEXT("Dismissed toast returns to no-layout visibility"),
		Toast->GetVisibility(), EVisibility::Collapsed);
	return true;
}
