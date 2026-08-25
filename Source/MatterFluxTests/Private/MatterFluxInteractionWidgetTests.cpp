#include "Misc/AutomationTest.h"

#include "Framework/Application/SlateApplication.h"
#include "IMatterFluxScriptRuntime.h"
#include "UI/MatterFluxInteractionWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxShopExitFitsFrameTest,
	"MatterFlux.UI.Shop.ExitButtonFitsInsideFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

namespace
{
	struct FArrangedWidgetEntry
	{
		TSharedRef<SWidget> Widget;
		FGeometry Geometry;
	};

	void CollectArrangedWidgets(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		TArray<FArrangedWidgetEntry>& OutWidgets)
	{
		OutWidgets.Add({Widget, Geometry});
		FArrangedChildren Children(EVisibility::All);
		Widget->ArrangeChildren(Geometry, Children, true);
		for (int32 Index = 0; Index < Children.Num(); ++Index)
		{
			const FArrangedWidget& Child = Children[Index];
			CollectArrangedWidgets(
				Child.Widget, Child.Geometry, OutWidgets);
		}
	}

	bool ContainsLeaveLabel(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			return StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString()
				== TEXT("离开");
		}
		if (const FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				if (ContainsLeaveLabel(
					ConstCastSharedRef<SWidget>(Children->GetChildAt(Index))))
				{
					return true;
				}
			}
		}
		return false;
	}
}

bool FMatterFluxShopExitFitsFrameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("Slate is initialized"),
		FSlateApplication::IsInitialized()))
	{
		return false;
	}

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxCreatureDefinition* Merchant = Registry.IsValid()
		? Registry->Creatures.Find(TEXT("std.merchant_base"))
		: nullptr;
	if (!TestNotNull(TEXT("Story merchant exists"), Merchant))
	{
		return false;
	}
	UMatterFluxInteractionWidget* Widget =
		NewObject<UMatterFluxInteractionWidget>();
	const TSharedRef<SWidget> Root = Widget->RebuildWidgetForTesting();
	Widget->OpenShop(
		nullptr,
		Merchant->DialogueId,
		Merchant->ShopId);
	Root->SlatePrepass(1.0f);

	TArray<FArrangedWidgetEntry> Widgets;
	CollectArrangedWidgets(
		Root,
		FGeometry::MakeRoot(
			FVector2D(1280.0f, 720.0f),
			FSlateLayoutTransform()),
		Widgets);
	TOptional<FGeometry> InteractionFrame;
	TOptional<FGeometry> LeaveButton;
	float LargestFrameArea = 0.0f;
	for (const FArrangedWidgetEntry& Candidate : Widgets)
	{
		const FVector2D Size = Candidate.Geometry.GetAbsoluteSize();
		if (Candidate.Widget->GetType() == TEXT("SBox")
			&& Size.X >= 580.0f && Size.X <= 650.0f
			&& Size.Y >= 500.0f && Size.Y <= 700.0f
			&& Size.X * Size.Y > LargestFrameArea)
		{
			InteractionFrame = Candidate.Geometry;
			LargestFrameArea = Size.X * Size.Y;
		}
		if (Candidate.Widget->GetType() == TEXT("SButton")
			&& ContainsLeaveLabel(Candidate.Widget))
		{
			LeaveButton = Candidate.Geometry;
		}
	}

	if (TestTrue(TEXT("Interaction frame has layout geometry"),
		InteractionFrame.IsSet())
		&& TestTrue(TEXT("Leave button has layout geometry"),
			LeaveButton.IsSet()))
	{
		const FGeometry& FrameGeometry = InteractionFrame.GetValue();
		const FGeometry& LeaveGeometry = LeaveButton.GetValue();
		const float FrameBottom = FrameGeometry.GetAbsolutePosition().Y
			+ FrameGeometry.GetAbsoluteSize().Y;
		const float LeaveBottom = LeaveGeometry.GetAbsolutePosition().Y
			+ LeaveGeometry.GetAbsoluteSize().Y;
		TestTrue(TEXT("Leave button bottom remains inside the shop frame"),
			LeaveBottom <= FrameBottom + KINDA_SMALL_NUMBER);
	}

	return true;
}

#endif
