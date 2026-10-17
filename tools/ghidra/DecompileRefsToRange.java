// Decompile every function that references an address inside [START, END) (hex pairs as arguments), each
// function once, with a "DC| ==== <function> <- <address>" header naming the first address that led
// there. For a firmware image that pokes MMIO through absolute addresses this lists the code behind
// each hardware block. Every output line is prefixed "DC| ". Headless:
//   analyzeHeadless PROJ n94 -process iopfw-10B329.bin -noanalysis -scriptPath DIR \
//       -postScript DecompileRefsToRange.java 0x3f200000 0x3f210000 0x3f300000 0x3f301000
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import java.util.HashSet;
import java.util.Set;

public class DecompileRefsToRange extends GhidraScript {
    private void out(String s) {
        for (String line : s.split("\n")) {
            println("DC| " + line);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        ReferenceManager rm = currentProgram.getReferenceManager();
        Set<Address> done = new HashSet<>();
        String[] args = getScriptArgs();
        for (int i = 0; i + 1 < args.length; i += 2) {
            Address start = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
            Address end = toAddr(Long.parseLong(args[i + 1].replace("0x", ""), 16) - 1);
            AddressSet set = new AddressSet(start, end);
            AddressIterator it = rm.getReferenceDestinationIterator(set, true);
            while (it.hasNext()) {
                Address to = it.next();
                ReferenceIterator refs = rm.getReferencesTo(to);
                while (refs.hasNext()) {
                    Reference r = refs.next();
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) {
                        out("---- " + to + " referenced outside any function at " + r.getFromAddress());
                        continue;
                    }
                    if (done.contains(f.getEntryPoint())) {
                        out("---- " + to + " also in " + f.getName() + " at " + r.getFromAddress());
                        continue;
                    }
                    done.add(f.getEntryPoint());
                    out("==== " + f.getName() + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses()
                        + " bytes) <- " + to + " at " + r.getFromAddress());
                    DecompileResults res = ifc.decompileFunction(f, 120, monitor);
                    if (res.decompileCompleted()) {
                        out(res.getDecompiledFunction().getC());
                    } else {
                        out("decompile failed: " + res.getErrorMessage());
                    }
                }
            }
        }
        ifc.dispose();
    }
}
