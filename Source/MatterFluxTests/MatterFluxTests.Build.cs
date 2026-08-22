using UnrealBuildTool;

public class MatterFluxTests : ModuleRules
{
	public MatterFluxTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;
		// Forge acceptance tests exercise real cross-translation-unit entry
		// patching, private access and internal linkage. A Unity translation
		// unit exposes the fixture definitions to the test call sites and lets
		// the optimizer bypass those entry points, invalidating the test seam.
		bUseUnity = false;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EngineSettings",
			"ForgeRuntime",
			"MatterFlux",
			"MatterFluxDeveloper",
			"MatterFluxLua",
			"UnrealEd",
			"AutomationController",
			"GameplayAbilities",
			"GameplayTags",
			"EnhancedInput",
			"InputCore",
			"PhysicsCore",
			"UMG",
			"ProceduralMeshComponent"
		});
	}
}
