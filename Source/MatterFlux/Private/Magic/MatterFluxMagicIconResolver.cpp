#include "Magic/MatterFluxMagicIconResolver.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

FString FMatterFluxMagicIconResolver::GetIconRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("Lua"),
		TEXT("Icons")));
}

bool FMatterFluxMagicIconResolver::TryResolveIconPath(
	const FString& IconKey,
	FString& OutIconPath)
{
	OutIconPath.Reset();
	FString RelativePath = IconKey.TrimStartAndEnd();
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (RelativePath.IsEmpty()
		|| !FPaths::IsRelative(RelativePath)
		|| RelativePath.StartsWith(TEXT("/"))
		|| RelativePath.Contains(TEXT("..")))
	{
		return false;
	}

	for (const TCHAR Character : RelativePath)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-')
			&& Character != TEXT('/')
			&& Character != TEXT('.'))
		{
			return false;
		}
	}

	const FString Extension = FPaths::GetExtension(RelativePath, false);
	if (Extension.IsEmpty())
	{
		RelativePath += TEXT(".png");
	}
	else if (!Extension.Equals(TEXT("png"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const FString Root = GetIconRoot();
	const FString Candidate = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(Root, RelativePath));
	const FString RootPrefix = Root + TEXT("/");
	if (!Candidate.StartsWith(RootPrefix, ESearchCase::IgnoreCase)
		|| !IFileManager::Get().FileExists(*Candidate))
	{
		return false;
	}

	OutIconPath = Candidate;
	return true;
}
