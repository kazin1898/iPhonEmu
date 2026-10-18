// Decompile every function that uses a string through one level of indirection: an Objective-C
// binary refers to a CFString constant (__cfstring: isa, flags, pointer to the C string, length),
// not to the C string, so DecompileByString finds nothing there. For each defined string containing
// an argument: every memory word holding the string's address (the CFString), then every function
// referencing that word, each once. Output lines are prefixed "DC| " as DecompileByString's.
//   analyzeHeadless PROJ n94 -process backboardd.macho -noanalysis -scriptPath DIR \
//       -postScript DecompileByCFString.java "No job" "You're on your own"
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.HashSet;
import java.util.Set;

public class DecompileByCFString extends GhidraScript {
    private void out(String s) {
        for (String line : s.split("\n")) {
            println("DC| " + line);
        }
    }

    private void decompileRefs(DecompInterface ifc, Address target, String why, Set<Address> done) {
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(target);
        while (refs.hasNext()) {
            Reference r = refs.next();
            Function f = getFunctionContaining(r.getFromAddress());
            if (f == null || done.contains(f.getEntryPoint())) {
                continue;
            }
            done.add(f.getEntryPoint());
            out("==== " + f.getName() + " @ " + f.getEntryPoint() + " <- " + why);
            DecompileResults res = ifc.decompileFunction(f, 120, monitor);
            if (res != null && res.getDecompiledFunction() != null) {
                out(res.getDecompiledFunction().getC());
            } else {
                out("(decompile failed)");
            }
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        Set<Address> done = new HashSet<>();
        String[] needles = getScriptArgs();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        int found = 0;
        while (it.hasNext()) {
            Data d = it.next();
            if (!(d.getValue() instanceof String)) {
                continue;
            }
            String str = (String) d.getValue();
            String hit = null;
            for (String n : needles) {
                if (str.contains(n)) {
                    hit = n;
                    break;
                }
            }
            if (hit == null) {
                continue;
            }
            found++;
            String why = "\"" + str.replace("\n", "\\n") + "\"";
            decompileRefs(ifc, d.getAddress(), why, done);
            long a = d.getAddress().getOffset();
            byte[] ptr = { (byte) a, (byte) (a >> 8), (byte) (a >> 16), (byte) (a >> 24) };
            for (MemoryBlock blk : currentProgram.getMemory().getBlocks()) {
                if (!blk.isInitialized()) {
                    continue;
                }
                Address at = blk.getStart();
                while (at != null && at.compareTo(blk.getEnd()) < 0) {
                    at = currentProgram.getMemory().findBytes(at, blk.getEnd(), ptr, null, true, monitor);
                    if (at == null) {
                        break;
                    }
                    decompileRefs(ifc, at, why + " via " + at, done);
                    at = at.add(4);
                }
            }
        }
        if (found == 0) {
            out("==== no defined string contains any of the arguments");
        } else if (done.isEmpty()) {
            out("==== strings found, but no function references them or their CFStrings");
        }
    }
}
