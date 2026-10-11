// Ghidra headless post-script: apply a naming table and make the listing readable.
//   arg0 = names.json  {"functions": {"0xaddr": "name"}, "labels": {"0xaddr": "name"},
//                        "devices": {"0xaddr": "name"}, "ram_block": [start, size],
//                        "code_end": "0xaddr"}
//   arg1 = output directory for the re-export (proteus_named.c, functions_named.csv)
// Also: adds a RAM block so RAM symbols can be labelled, marks flash read-only so the
// decompiler propagates literal-pool constants, and types literal-pool words as pointers.
// @category Counterpuck
import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Map;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class GhidraApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        JsonObject cfg = JsonParser.parseString(new String(Files.readAllBytes(Paths.get(getScriptArgs()[0])))).getAsJsonObject();
        String outdir = getScriptArgs()[1];
        Memory mem = currentProgram.getMemory();

        // 1. RAM block (uninitialised) so RAM addresses can carry symbols.
        JsonArray ram = cfg.getAsJsonArray("ram_block");
        Address ramStart = toAddr(ram.get(0).getAsLong());
        if (mem.getBlock(ramStart) == null) {
            mem.createUninitializedBlock("ram", ramStart, ram.get(1).getAsLong(), false);
            mem.getBlock(ramStart).setRead(true); mem.getBlock(ramStart).setWrite(true);
        }
        // 2. Flash is read-only: lets the decompiler fold literal-pool loads into constants.
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.getName().equals("ram")) b.setWrite(false);
        }
        // 3. struct device, applied to device symbols.
        StructureDataType dev = new StructureDataType(new CategoryPath("/zephyr"), "device", 0);
        dev.add(new PointerDataType(CharDataType.dataType), "name", null);
        dev.add(new PointerDataType(), "config", null);
        dev.add(new PointerDataType(), "api", null);
        dev.add(new PointerDataType(), "state", null);
        dev.add(new PointerDataType(), "data", null);
        DataType devT = currentProgram.getDataTypeManager().addDataType(dev, null);

        int fn = 0, lb = 0, dv = 0;
        for (Map.Entry<String, JsonElement> e : cfg.getAsJsonObject("functions").entrySet()) {
            Address a = toAddr(Long.decode(e.getKey()));
            Function f = getFunctionAt(a);
            if (f == null) { new CreateFunctionCmd(a).applyTo(currentProgram, monitor); f = getFunctionAt(a); }
            if (f == null) continue;
            try { f.setName(e.getValue().getAsString(), SourceType.USER_DEFINED); fn++; } catch (Exception ex) { }
        }
        for (Map.Entry<String, JsonElement> e : cfg.getAsJsonObject("labels").entrySet()) {
            Address a = toAddr(Long.decode(e.getKey()));
            try { createLabel(a, e.getValue().getAsString(), true, SourceType.USER_DEFINED); lb++; } catch (Exception ex) { }
        }
        for (Map.Entry<String, JsonElement> e : cfg.getAsJsonObject("devices").entrySet()) {
            Address a = toAddr(Long.decode(e.getKey()));
            try {
                clearListing(a, a.add(19));
                createData(a, devT);
                createLabel(a, e.getValue().getAsString(), true, SourceType.USER_DEFINED);
                dv++;
            } catch (Exception ex) { }
        }
        // 4. Literal pools: type every pointer-looking word in the code region as a pointer.
        long codeEnd = Long.decode(cfg.get("code_end").getAsString());
        Address base = currentProgram.getMinAddress();
        long imgEnd = currentProgram.getMaxAddress().getOffset();
        int pools = 0;
        for (long off = base.getOffset(); off < codeEnd; off += 4) {
            Address a = toAddr(off);
            if (getInstructionContaining(a) != null) continue;
            Data d = getDataAt(a);
            if (d != null && !d.getDataType().getName().startsWith("undefined")) continue;
            long w;
            try { w = mem.getInt(a) & 0xffffffffL; } catch (Exception ex) { continue; }
            boolean ok = (w >= base.getOffset() && w <= imgEnd) || (w >= ramStart.getOffset() && w < ramStart.getOffset() + ram.get(1).getAsLong());
            if (!ok) continue;
            try {
                if (d != null) clearListing(a, a.add(3));
                createData(a, new PointerDataType());
                pools++;
            } catch (Exception ex) { }
        }
        println("named " + fn + " functions, " + lb + " labels, " + dv + " devices, typed " + pools + " pool pointers");
        analyzeChanges(currentProgram);

        // 5. Re-export.
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter c = new PrintWriter(new FileWriter(outdir + "/proteus_named.c"));
        PrintWriter t = new PrintWriter(new FileWriter(outdir + "/functions_named.csv"));
        t.println("address,name,size");
        c.println("// Ghidra decompilation with recovered names. Personal study only; not for redistribution.\n");
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            t.printf("0x%s,%s,%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            if (r != null && r.decompileCompleted()) {
                c.println("// ==== 0x" + f.getEntryPoint() + "  " + f.getName() + " ====");
                c.println(r.getDecompiledFunction().getC());
                c.println();
            }
        }
        c.close(); t.close();
        println("export done -> " + outdir);
    }
}
