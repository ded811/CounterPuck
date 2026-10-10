// Ghidra post-analysis script (Java). Runs under analyzeHeadless with no extra deps.
// Exports decompiled C for every function, a function table, and defined strings.
// First script arg = output directory.
// @category Counterpuck
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class GhidraExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outdir = args.length > 0 ? args[0] : ".";

        FunctionManager fm = currentProgram.getFunctionManager();

        PrintWriter ftab = new PrintWriter(new FileWriter(outdir + "/functions.csv"));
        ftab.println("address,name,size,params");
        int n = 0;
        for (Function f : fm.getFunctions(true)) {
            ftab.printf("0x%s,%s,%d,%d%n", f.getEntryPoint(), f.getName(),
                    f.getBody().getNumAddresses(), f.getParameterCount());
            n++;
        }
        ftab.close();

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        PrintWriter c = new PrintWriter(new FileWriter(outdir + "/proteus_decompiled.c"));
        c.println("// Decompiled by Ghidra (" + currentProgram.getLanguageID()
                + "). Personal study only; not for redistribution.\n");
        int i = 0;
        for (Function f : fm.getFunctions(true)) {
            DecompileResults res = d.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                c.println("// ==== 0x" + f.getEntryPoint() + "  " + f.getName() + " ====");
                c.println(res.getDecompiledFunction().getC());
                c.println();
            }
            if (++i % 50 == 0) {
                println("decompiled " + i + "/" + n);
            }
        }
        c.close();

        PrintWriter st = new PrintWriter(new FileWriter(outdir + "/strings.txt"));
        for (Data data : currentProgram.getListing().getDefinedData(true)) {
            DataType dt = data.getDataType();
            String tn = dt.getName().toLowerCase();
            if (tn.contains("string") || tn.contains("char")) {
                String v = data.getDefaultValueRepresentation();
                if (v != null && !v.isEmpty()) {
                    st.println("0x" + data.getAddress() + "\t" + v);
                }
            }
        }
        st.close();

        println("export done: " + n + " functions -> " + outdir);
    }
}
