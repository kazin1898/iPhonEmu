// Decompile the function at each address argument and, transitively, the functions it calls, down to a
// depth given as "ADDR:DEPTH" (default 0 = the function alone); an address prefixed "+" has a function
// created there first (Thumb state follows the program's context). Each function is printed once, every
// line prefixed "DC| ". Headless:
//   analyzeHeadless PROJ n94 -process iopfw-10B329.bin -noanalysis -scriptPath DIR \
//       -postScript DecompileCallees.java 0xa7c:2 +0xcaa0:1
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.Set;

public class DecompileCallees extends GhidraScript {
    private void out(String s) {
        for (String line : s.split("\n")) {
            println("DC| " + line);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        Set<Address> done = new HashSet<>();
        for (String arg : getScriptArgs()) {
            boolean create = arg.startsWith("+");
            String[] parts = arg.replace("+", "").split(":");
            int depth = parts.length > 1 ? Integer.parseInt(parts[1]) : 0;
            Address a = toAddr(Long.parseLong(parts[0].replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null && create) {
                new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
                f = getFunctionContaining(a);
            }
            if (f == null) {
                out("==== " + arg + ": no function (prefix the address with + to create one there)");
                continue;
            }
            ArrayDeque<Object[]> queue = new ArrayDeque<>();
            queue.add(new Object[] { f, depth });
            while (!queue.isEmpty()) {
                Object[] item = queue.poll();
                Function g = (Function) item[0];
                int d = (Integer) item[1];
                if (done.contains(g.getEntryPoint())) {
                    continue;
                }
                done.add(g.getEntryPoint());
                out("==== " + g.getName() + " @ " + g.getEntryPoint() + " (" + g.getBody().getNumAddresses()
                    + " bytes)");
                DecompileResults res = ifc.decompileFunction(g, 120, monitor);
                if (res.decompileCompleted()) {
                    out(res.getDecompiledFunction().getC());
                } else {
                    out("decompile failed: " + res.getErrorMessage());
                }
                if (d > 0) {
                    for (Function c : g.getCalledFunctions(monitor)) {
                        if (!c.isThunk() && !c.isExternal()) {
                            queue.add(new Object[] { c, d - 1 });
                        }
                    }
                }
            }
        }
        ifc.dispose();
    }
}
