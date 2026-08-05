// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

using UnrealBuildTool;

public class NGSVMCore : ModuleRules
{
	public NGSVMCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
                "NNE",
                "InputCore",
				"EnhancedInput",
				// RHIGPUReadback.h (RHI module) is included directly from this module's public
				// headers (NGSVMManager.h, NGSVMAsyncKeyImage.h) for the TUniquePtr<FRHIGPUTextureReadback>
				// members -- must be Public, not Private, so downstream modules that include those
				// headers (e.g. NGSVMComposure) also get RHI's include paths.
				"RHI"
				// ... add other public dependencies that you statically link with here ...
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"DeveloperSettings",
				"RenderCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
