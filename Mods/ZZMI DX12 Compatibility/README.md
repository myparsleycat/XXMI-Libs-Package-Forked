# ZZMI DX12 RabbitFX compatibility

This adapter adds the native ZZZ toon glow path needed by RabbitFX 7.10 when
ZZMI is running through the project's DirectX 12 backend.

It does not contain or modify RabbitFX. Install RabbitFX 7.10 from its original
author first, then extract the `ZZMI DX12 Compatibility` folder into ZZMI's
`Mods` folder. RabbitFX can subsequently be replaced or reinstalled without
overwriting this adapter.

The adapter is version-specific because RabbitFX may change shader matching or
glow calculations in future releases. Check for an updated adapter before using
it with a RabbitFX version newer than 7.10.
