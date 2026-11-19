// Batch query script for headless Ghidra (see re/scripts/q).
// Commands, one per line, in the command file:
//   str <regex>        defined strings matching regex, with referencing functions
//   dec <addr|name>    decompile the function containing addr (or named)
//   xref <addr>        references to addr (with containing function)
//   bytes <addr> <n>   hex bytes
//   vt <addr> <n>      read n pointers at addr (vtable) and name each target
//   sym <regex>        symbols matching regex
// Addresses are VAs (0x14...) or RVAs (anything below 0x140000000).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class Query extends GhidraScript {
  PrintWriter out;
  DecompInterface di;

  Address addr(String s) {
    s = s.trim();
    if (s.startsWith("0x") || s.matches("[0-9a-fA-F]+")) {
      long v = Long.parseUnsignedLong(s.replace("0x", ""), 16);
      if (v < 0x140000000L) v += 0x140000000L;
      return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
    }
    List<Function> fs = getGlobalFunctions(s);
    if (!fs.isEmpty()) return fs.get(0).getEntryPoint();
    SymbolIterator it = currentProgram.getSymbolTable().getSymbols(s);
    if (it.hasNext()) return it.next().getAddress();
    return null;
  }

  String fn(Address a) {
    Function f = getFunctionContaining(a);
    if (f == null) return "<nofunc>";
    return f.getName() + "@" + f.getEntryPoint();
  }

  public void run() throws Exception {
    String[] args = getScriptArgs();
    List<String> cmds = Files.readAllLines(Paths.get(args[0]));
    out = new PrintWriter(new FileWriter(args[1]));
    di = new DecompInterface();
    di.openProgram(currentProgram);
    Listing L = currentProgram.getListing();
    for (String line : cmds) {
      line = line.trim();
      if (line.isEmpty() || line.startsWith("#")) continue;
      String[] p = line.split("\\s+", 2);
      out.println("=== " + line);
      try {
        switch (p[0]) {
          case "str": {
            Pattern re = Pattern.compile(p[1]);
            DataIterator it = L.getDefinedData(true);
            int n = 0;
            while (it.hasNext() && n < 400) {
              Data d = it.next();
              if (!(d.getValue() instanceof String)) continue;
              String v = (String) d.getValue();
              if (!re.matcher(v).find()) continue;
              n++;
              StringBuilder sb = new StringBuilder();
              for (Reference r : getReferencesTo(d.getAddress()))
                sb.append(" ").append(r.getFromAddress()).append("(").append(fn(r.getFromAddress())).append(")");
              out.println(d.getAddress() + " \"" + v + "\"" + sb);
            }
            break;
          }
          case "dec": {
            Address a = addr(p[1]);
            Function f = a == null ? null : getFunctionContaining(a);
            if (f == null) { out.println("no function"); break; }
            DecompileResults r = di.decompileFunction(f, 180, monitor);
            out.println("// " + f.getName() + " @ " + f.getEntryPoint());
            out.println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC()
                                                          : ("fail: " + r.getErrorMessage()));
            break;
          }
          case "xref": {
            Address a = addr(p[1]);
            for (Reference r : getReferencesTo(a))
              out.println(r.getFromAddress() + " " + r.getReferenceType() + " " + fn(r.getFromAddress()));
            break;
          }
          case "bytes": {
            String[] q = p[1].split("\\s+");
            Address a = addr(q[0]);
            int n = Integer.parseInt(q[1]);
            byte[] b = getBytes(a, n);
            StringBuilder sb = new StringBuilder();
            for (byte x : b) sb.append(String.format("%02x ", x & 0xff));
            out.println(a + ": " + sb);
            break;
          }
          case "vt": {
            String[] q = p[1].split("\\s+");
            Address a = addr(q[0]);
            int n = Integer.parseInt(q[1]);
            for (int i = 0; i < n; i++) {
              long v = getLong(a.add(i * 8L));
              Address t = toAddr(v);
              out.println(String.format("[%d] +0x%x %s %s", i, i * 8, t, fn(t)));
            }
            break;
          }
          case "sym": {
            Pattern re = Pattern.compile(p[1]);
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
            int n = 0;
            while (it.hasNext() && n < 300) {
              Symbol s = it.next();
              String nm = s.getName(true);
              if (re.matcher(nm).find()) { out.println(s.getAddress() + " " + nm); n++; }
            }
            break;
          }
          default:
            out.println("unknown");
        }
      } catch (Exception e) {
        out.println("error: " + e);
      }
      out.flush();
    }
    out.close();
  }
}
