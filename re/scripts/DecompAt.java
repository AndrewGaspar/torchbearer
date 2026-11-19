// Decompile the functions at the addresses in TB_FUNCS (comma-separated hex VAs).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;

public class DecompAt extends GhidraScript {
  @Override
  public void run() throws Exception {
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    for (String s : System.getenv("TB_FUNCS").split(",")) {
      Function f = getFunctionContaining(toAddr(s.trim()));
      if (f == null) { println("NOFUNC " + s); continue; }
      DecompileResults r = di.decompileFunction(f, 120, monitor);
      println("==== " + f.getName() + " @ " + f.getEntryPoint());
      println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "(failed)");
    }
  }
}
