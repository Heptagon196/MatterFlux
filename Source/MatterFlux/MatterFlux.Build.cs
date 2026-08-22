// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MatterFlux : ModuleRules
{
	public MatterFlux(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"MatterFluxLua",
			"NetCore",
			"UMG",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"EnhancedInput",
			"ForgeRuntime",
			"GeometryCore",
			"GeometryAlgorithms",
			"InputCore",
			"PhysicsCore",
			"ProceduralMeshComponent"
		});
	}
}
