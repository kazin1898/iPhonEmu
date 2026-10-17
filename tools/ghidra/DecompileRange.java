// Decompile every function whose entry lies in [START, END) (hex, one pair per two arguments). Every
// output line is prefixed "DC| ". For a small kext, the two bounds are its __text from kext-map.txt. Headless:
//   analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath DIR \
//       -postScript DecompileRange.java 0x80d17500 0x80d17be4
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class DecompileRange extends GhidraScript {
    private void out(String s) {
        for (String line : s.split("\n")) {
            println("DC| " + line);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        String[] args = getScriptArgs();
        for (int i = 0; i + 1 < args.length; i += 2) {
            Address start = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
            Address end = toAddr(Long.parseLong(args[i + 1].replace("0x", ""), 16));
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(start, true);
            int n = 0;
            while (it.hasNext()) {
                Function f = it.next();
                if (f.getEntryPoint().compareTo(end) >= 0) {
                    break;
                }
                n++;
                out("==== " + f.getName() + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses()
                    + " bytes)");
                DecompileResults res = ifc.decompileFunction(f, 120, monitor);
                if (res.decompileCompleted()) {
                    out(res.getDecompiledFunction().getC());
                } else {
                    out("decompile failed: " + res.getErrorMessage());
                }
            }
            out("==== range " + start + "-" + end + ": " + n + " functions");
        }
        ifc.dispose();
    }
}
