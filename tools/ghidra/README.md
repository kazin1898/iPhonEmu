# Ghidra headless scripts for the iOS kernelcache

Used from `analyzeHeadless` (Ghidra 12.1, JDK 21; the Windows install lives in `../../../toolchain`, see
its `LEIA-ME.md`). The project is kept outside the repository (`runs/n94/ghidra`).

| Script | What |
|---|---|
| `PrelinkAnalyze.java` | The Mach-O loader analyzes the kernel proper but leaves the prelinked kexts in `__PRELINK_TEXT` as bytes. Given a file of `0xVA 0xSIZE` lines (one per kext `__text`), it marks them Thumb, disassembles them and runs the analyzers with the slow whole-program passes (Decompiler Parameter ID, Stack, Switch) off, so the kexts' functions exist. |
| `DecompileAt.java` | Decompiles the function containing each address argument (prefix `+` to create a function there first); every output line starts with `DC| ` so the headless log can be filtered. |
| `DecompileByString.java` | Decompiles every function that references a defined string containing one of the arguments. The prelinked kexts reach their strings with `ldr rd,[pc]; add rd,pc` pairs that the analyzers do not always resolve; `runs/n94/ghidra/strrefs.json` was made by resolving those pairs in Python (the kernel's `__PRELINK_TEXT` maps VA - 0x80041000 to the file) and feeding `DecompileAt +ADDR`. |
| `DecompileCallees.java` | `ADDR:DEPTH`: the function and its callees to that depth, each once. |
| `DecompileRange.java` | Every function whose entry lies in `[START, END)`: a whole small kext from its `__text` bounds. |
| `DecompileRefsToRange.java` | Every function referencing an address in `[START, END)`: for a firmware image, the code behind each MMIO block. |
| `IopFwAnalyze.java` | For an EmbeddedIOP firmware image imported raw at 0 (`-processor ARM:LE:32:v7 -loader BinaryLoader`): disassembles the ARM vectors/handlers (words 0x20..0x3c) and the Thumb `main` (word 0x30c) in their modes, then runs the analyzers. |

```
analyzeHeadless PROJ n94 -import kernelcache.mach -analysisTimeoutPerFile 2400
analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath tools/ghidra -postScript PrelinkAnalyze.java kext-text.txt
analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath tools/ghidra -postScript DecompileAt.java 0x804cf64c 0x8085a73c
```

The kext text ranges come from the kernelcache's embedded Mach-O headers (`0xfeedface`, filetype 11); the
M2 session's `docs/n94/hardware.md` records what was decoded with them.

The 10B329 IOP firmware (`fwFirmwareImage` of `IOP_s5l8940xiop_firmware`, 0x29000 bytes at kernelcache file offset
0xa4a000) was imported as `iopfw-10B329.bin` into the same project; `docs/n94/hardware.md` "The IOP" has what came out.
