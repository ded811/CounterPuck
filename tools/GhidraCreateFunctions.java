// Ghidra headless pre-script: create functions at the addresses listed in a file (one hex
// address per line), then re-run auto-analysis so the new code gets decompiled too.
// Script arg 0 = path to the address list. Pair with GhidraExport/GhidraXrefDump afterwards.
// @category Counterpuck
import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;

public class GhidraCreateFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        int made = 0, existed = 0, failed = 0;
        Register tmode = currentProgram.getRegister("TMode");
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Address a = toAddr(Long.parseLong(line.replace("0x", ""), 16));
            if (getFunctionAt(a) != null) { existed++; continue; }
            try {
                if (getInstructionAt(a) == null) {
                    if (tmode != null && getDataAt(a) == null) {
                        try {
                            currentProgram.getProgramContext().setValue(tmode, a, a, BigInteger.ONE);
                        } catch (Exception e) { /* context already set by neighbouring code */ }
                    }
                    new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                }
                if (getInstructionAt(a) == null) { failed++; continue; }
                if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) made++; else failed++;
            } catch (Exception e) {
                failed++;
            }
        }
        println("created " + made + " functions, " + existed + " existed, " + failed + " failed");
        analyzeChanges(currentProgram);
    }
}
