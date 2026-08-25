using UnrealBuildTool;

public class MatterFluxDeveloper : ModuleRules
{
	public MatterFluxDeveloper(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"Json",
			"MatterFlux",
			"MatterFluxLua",
			"ProceduralMeshComponent"
		});
	}
}
