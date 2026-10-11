// Ghidra headless post-script. Dumps the cross-reference data needed to classify and
// name functions offline: per-function string references, call graph, and data items
// that point at strings/functions (for decoding Zephyr's static tables).
// First script arg = output directory. Writes xrefs.tsv.
// @category Counterpuck
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.app.script.GhidraScript;

public class GhidraXrefDump extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outdir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        PrintWriter out = new PrintWriter(new FileWriter(outdir + "/xrefs.tsv"));

        // Record types:
        //   F <func_addr> <name> <size>
        //   S <func_addr> <string_addr>        function references this string (incl. via literal pool)
        //   C <func_addr> <callee_addr>        direct call
        //   D <func_addr> <data_addr>          function references this non-string data item
        //   P <data_addr> <target_addr> <kind> data item at data_addr holds a pointer to target (kind: S=string, F=function, O=other)
        for (Function f : fm.getFunctions(true)) {
            out.printf("F\t%s\t%s\t%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
            InstructionIterator it = listing.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                for (Reference r : ins.getReferencesFrom()) {
                    Address to = r.getToAddress();
                    if (r.getReferenceType().isCall()) {
                        Function callee = fm.getFunctionAt(to);
                        if (callee != null) out.printf("C\t%s\t%s%n", f.getEntryPoint(), to);
                        continue;
                    }
                    if (!r.getReferenceType().isData()) continue;
                    Data d = listing.getDataAt(to);
                    if (d == null) continue;
                    // Literal pool: a pointer word. Follow it one hop.
                    if (d.isPointer()) {
                        Object v = d.getValue();
                        if (v instanceof Address) {
                            Address tgt = (Address) v;
                            Data td = listing.getDataAt(tgt);
                            if (td != null && isString(td)) out.printf("S\t%s\t%s%n", f.getEntryPoint(), tgt);
                            else if (fm.getFunctionAt(tgt) != null || fm.getFunctionContaining(tgt) != null) { /* function ptr: ignore here */ }
                            else out.printf("D\t%s\t%s%n", f.getEntryPoint(), tgt);
                        }
                    } else if (isString(d)) {
                        out.printf("S\t%s\t%s%n", f.getEntryPoint(), to);
                    } else {
                        out.printf("D\t%s\t%s%n", f.getEntryPoint(), to);
                    }
                }
            }
        }
        // Every pointer-typed data item in the program (for struct table decoding).
        for (Data d : listing.getDefinedData(true)) {
            if (!d.isPointer()) continue;
            Object v = d.getValue();
            if (!(v instanceof Address)) continue;
            Address tgt = (Address) v;
            String kind = "O";
            Data td = listing.getDataAt(tgt);
            if (td != null && isString(td)) kind = "S";
            else if (fm.getFunctionAt(tgt) != null) kind = "F";
            out.printf("P\t%s\t%s\t%s%n", d.getAddress(), tgt, kind);
        }
        out.close();
        println("xref dump done -> " + outdir + "/xrefs.tsv");
    }

    private static boolean isString(Data d) {
        String t = d.getDataType().getName().toLowerCase();
        return t.contains("string") || t.contains("char");
    }
}
