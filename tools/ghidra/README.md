# Ghidra headless scripts for the iOS kernelcache

Used from `analyzeHeadless` (Ghidra 12.1, JDK 21; the Windows install lives in `../../../toolchain`, see
its `LEIA-ME.md`). The project is kept outside the repository (`runs/n94/ghidra`).

| Script | What |
|---|---|
| `PrelinkAnalyze.java` | The Mach-O loader analyzes the kernel proper but leaves the prelinked kexts in `__PRELINK_TEXT` as bytes. Given a file of `0xVA 0xSIZE` lines (one per kext `__text`), it marks them Thumb, disassembles them and runs the analyzers with the slow whole-program passes (Decompiler Parameter ID, Stack, Switch) off, so the kexts' functions exist. |
| `DecompileAt.java` | Decompiles the function containing each address argument (prefix `+` to create a function there first); every output line starts with `DC| ` so the headless log can be filtered. |

```
analyzeHeadless PROJ n94 -import kernelcache.mach -analysisTimeoutPerFile 2400
analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath tools/ghidra -postScript PrelinkAnalyze.java kext-text.txt
analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath tools/ghidra -postScript DecompileAt.java 0x804cf64c 0x8085a73c
```

The kext text ranges come from the kernelcache's embedded Mach-O headers (`0xfeedface`, filetype 11); the
M2 session's `docs/n94/hardware.md` records what was decoded with them.
