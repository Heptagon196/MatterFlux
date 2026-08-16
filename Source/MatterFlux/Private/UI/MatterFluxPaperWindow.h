#pragma once

#include "Input/Reply.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Canonical black-keyline/white-surface window used by every large runtime UI.
 *
 * Callers provide only a header and page body. Border thickness, surface,
 * content spacing, and tab presentation stay behind this private Slate seam.
 */
class SMatterFluxPaperWindow final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMatterFluxPaperWindow)
		: _ContentPadding(10.0f)
	{}
		SLATE_NAMED_SLOT(FArguments, Header)
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_ARGUMENT(FMargin, ContentPadding)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

/** Canonical tab/action used in a paper window header. */
class SMatterFluxPaperTab final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMatterFluxPaperTab)
		: _Label(FText::GetEmpty())
		, _ToolTip(FText::GetEmpty())
		, _bSelected(false)
		, _Padding(18.0f, 8.0f)
	{}
		SLATE_ARGUMENT(FText, Label)
		SLATE_ARGUMENT(FText, ToolTip)
		SLATE_ARGUMENT(bool, bSelected)
		SLATE_ARGUMENT(FMargin, Padding)
		SLATE_ARGUMENT(TFunction<FReply()>, OnClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};
