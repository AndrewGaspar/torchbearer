// Find each string given in the TB_STRINGS env var (|-separated), list its references, and
// decompile each referencing function once. Output to stdout.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.*;
import java.util.*;

public class XrefDecomp extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] needles = System.getenv("TB_STRINGS").split("\\|");
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    Set<Function> done = new HashSet<>();
    Memory mem = currentProgram.getMemory();
    for (String n : needles) {
      byte[] pat = (n + "\0").getBytes("UTF-8");
      Address a = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
      while (a != null) {
        println("STRING '" + n + "' @ " + a);
        for (Reference r : getReferencesTo(a)) {
          Function f = getFunctionContaining(r.getFromAddress());
          println("  ref from " + r.getFromAddress() + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
          if (f != null && done.add(f)) {
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            println("==== " + f.getName() + " @ " + f.getEntryPoint());
            println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "(decompile failed)");
          }
        }
        a = mem.findBytes(a.add(1), pat, null, true, monitor);
      }
    }
  }
}
