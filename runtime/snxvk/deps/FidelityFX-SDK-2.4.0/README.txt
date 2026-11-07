AMD FSR SDK 2.4.0 ("Redstone"), headers only: the FFX API and frame generation interfaces, from
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX (api/include, framegeneration/include).
These files are MIT (LICENSE.txt; the SDK's docs/license.md lists them under its MIT section).
AMD's signed DLLs are not here: the launcher downloads amd_fidelityfx_loader_vk.dll and
amd_fidelityfx_framegeneration_vk.dll from that release when the player asks for FSR frame generation.
