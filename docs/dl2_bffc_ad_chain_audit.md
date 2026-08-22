# DL2 BFFC to AD Clone Audit

- Commit: pending
- Scope: read-only producer/consumer diagnostics only.
- Capture: bounded history across command lists and epochs for full-size draw outputs, BFFC/AD input and output resources, and CopyResource/CopyTexture/ResolveTexture operations.
- Report: resource IDs, clone/effective IDs, formats, dimensions, generation, Present, draw serial, command list, and epoch.
- No GPU readback, staging resource, binding rewrite, HDR/ToneMap/Proxy math, or brightness setting changes.
- Resize destroys the armed audit state so no resource handle crosses swapchain generations.

Expected next test: arm `Capture BFFC to AD Clone Chain (4 Presents)`, reproduce one scene frame, then inspect the single `DL2 BFFC->AD clone chain audit` line.
