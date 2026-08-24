#include "Game/MatterFluxPlayableWorldActor.h"

#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "MatterFluxLog.h"
#include "CoreGlobals.h"

namespace
{
	void ActivateTreeCommand(
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
			const bool bActivated =
				It->ApplyMaterialStimulusToFirstGeneratedTree(EventSeed);
			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("mf.Reaction.ActivateTree: %s (seed=%d)"),
				bActivated ? TEXT("activated") : TEXT("no eligible tree"),
				EventSeed);
			if (bActivated && Args.Num() > 1)
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
			TEXT("mf.Reaction.ActivateTree: playable world actor not found."));
	}

	FAutoConsoleCommandWithWorldAndArgs GActivateTreeCommand(
		TEXT("mf.Reaction.ActivateTree"),
		TEXT("Activate the tree nearest the player: mf.Reaction.ActivateTree [event-seed=404] [capture-delay] [capture-multiplier] [quit-after-capture]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&ActivateTreeCommand));
}
