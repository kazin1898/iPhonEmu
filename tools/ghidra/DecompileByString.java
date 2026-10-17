// Decompile every function that references a string containing one of the script arguments
// (substring match over the program's defined strings), each function once. Every output line is
// prefixed "DC| " so a headless log can be filtered; a "DC| ==== <function> @ <entry> <- "<string>""
// header names the string that led there. Headless:
//   analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath DIR \
//       -postScript DecompileByString.java "nand_part_init" "IOFlashNVRAM::%s: starting"
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.HashSet;
import java.util.Set;

public class DecompileByString extends GhidraScript {
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
        String[] needles = getScriptArgs();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
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
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
            while (refs.hasNext()) {
                Reference r = refs.next();
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null || done.contains(f.getEntryPoint())) {
                    continue;
                }
                done.add(f.getEntryPoint());
                out("==== " + f.getName() + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses()
                    + " bytes) <- \"" + str.replace("\n", "\\n") + "\"");
                DecompileResults res = ifc.decompileFunction(f, 120, monitor);
                if (res.decompileCompleted()) {
                    out(res.getDecompiledFunction().getC());
                } else {
                    out("decompile failed: " + res.getErrorMessage());
                }
            }
        }
        if (done.isEmpty()) {
            out("==== no function references any of the strings");
        }
        ifc.dispose();
    }
}
