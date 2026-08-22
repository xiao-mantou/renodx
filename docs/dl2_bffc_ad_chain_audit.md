# DL2 BFFC to AD Clone Audit

- Commit: pending
- Scope: read-only producer/consumer diagnostics only.
- Capture: bounded history across command lists and epochs for full-size draw outputs, BFFC/AD input and output resources, and CopyResource/CopyTexture/ResolveTexture operations.
- Report: resource IDs, clone/effective IDs, formats, dimensions, generation, Present, draw serial, command list, and epoch.
- No GPU readback, staging resource, binding rewrite, HDR/ToneMap/Proxy math, or brightness setting changes.
- Resize destroys the armed audit state so no resource handle crosses swapchain generations.

Expected next test: arm `Capture BFFC to AD Clone Chain (4 Presents)`, reproduce one scene frame, then inspect the single `DL2 BFFC->AD clone chain audit` line.

## Result

- Build tested: `d5093f6`.
- Valid replacement event `#185`: BFFC output `0x17F4C337500 => 0x17F4C33B7F0`, format `27 => 10`.
- AD event `#250` t0 is exactly `0x17F4C337500 => 0x17F4C33B7F0`, same Present `12200`, generation `1`, command list `0x17F475A9CB8`, epoch `0`.
- AD output is `0x17C939C6310 => 0x17C939CCC40`, format `10`, clone enabled. No CopyResource/CopyTexture/Resolve was recorded.
- Therefore the replacement path is direct: `BFFC output -> AD t0 -> AD clone output`; the previously printed summary `bffc_output` was overwritten by later ordinary BFFC draws, so event `#185` is authoritative.
