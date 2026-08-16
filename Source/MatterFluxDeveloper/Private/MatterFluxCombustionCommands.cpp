#include "Game/MatterFluxPlayableWorldActor.h"

#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "MatterFluxLog.h"
#include "CoreGlobals.h"

namespace
{
	void IgniteTreeCommand(
		const TArray<FString>& Args,
		UWorld* World)
	{
		if (!World)
		{
			return;
		}
		int32 EventSeed = 404;
		if (!Args.IsEmpty())
		{
			LexTryParseString(EventSeed, *Args[0]);
		}
		TActorIterator<AMatterFluxPlayableWorldActor> It(World);
		if (It)
		{
			const bool bIgnited =
				It->IgniteFirstGeneratedTree(EventSeed);
			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("mf.Combustion.IgniteTree: %s (seed=%d)"),
				bIgnited ? TEXT("ignited") : TEXT("no eligible tree"),
				EventSeed);
			if (bIgnited && Args.Num() > 1)
			{
				if (IConsoleObject* CaptureObject =
					IConsoleManager::Get().FindConsoleObject(
						TEXT("mf.Visual.Capture")))
				{
					if (IConsoleCommand* CaptureCommand =
						CaptureObject->AsCommand())
					{
						TArray<FString> CaptureArgs;
						for (int32 Index = 1;
							Index < Args.Num();
							++Index)
						{
							CaptureArgs.Add(Args[Index]);
						}
						CaptureCommand->Execute(
							CaptureArgs,
							World,
							*GLog);
					}
				}
			}
			return;
		}
		UE_LOG(
			LogMatterFlux,
			Warning,
			TEXT("mf.Combustion.IgniteTree: playable world actor not found."));
	}

	FAutoConsoleCommandWithWorldAndArgs GIgniteTreeCommand(
		TEXT("mf.Combustion.IgniteTree"),
		TEXT("Ignite the tree nearest the player: mf.Combustion.IgniteTree [event-seed=404] [capture-delay] [capture-multiplier] [quit-after-capture]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&IgniteTreeCommand));
}
