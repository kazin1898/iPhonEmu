// Decompile every function whose name contains one of the arguments (substring match; for an
// Objective-C binary the analyzers name methods "Class::selector_"), each once. Output lines are
// prefixed "DC| " as DecompileByString's.
//   analyzeHeadless PROJ n94 -process backboardd.macho -noanalysis -scriptPath DIR \
//       -postScript DecompileByName.java launchSystemApp readyForSystemApp
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class DecompileByName extends GhidraScript {
    private void out(String s) {
        for (String line : s.split("\n")) {
            println("DC| " + line);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        String[] needles = getScriptArgs();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            String name = f.getName(true);
            String hit = null;
            for (String s : needles) {
                if (name.contains(s)) {
                    hit = s;
                    break;
                }
            }
            if (hit == null) {
                continue;
            }
            n++;
            out("==== " + name + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses() + " bytes)");
            DecompileResults res = ifc.decompileFunction(f, 120, monitor);
            if (res != null && res.getDecompiledFunction() != null) {
                out(res.getDecompiledFunction().getC());
            } else {
                out("(decompile failed)");
            }
        }
        if (n == 0) {
            out("==== no function name contains any of the arguments");
        }
    }
}
