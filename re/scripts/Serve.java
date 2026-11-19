// Headless query server: keeps the program open and answers requests dropped into a folder,
// so iterative RE does not pay Ghidra's start-up per question.
//
//   analyzeHeadless re/ghidra-$NAME awr -process Game_f_x64_EOS.exe -noanalysis -readOnly \
//       -scriptPath re/scripts -postScript Serve.java <dir>
//   echo "decomp 0x1401234" > <dir>/q1.req   ->  <dir>/q1.out (written atomically)
//
// One command per request file, several lines allowed (each line runs in order):
//   decomp ADDR            decompile the function containing ADDR
//   disasm ADDR N          N instructions from ADDR
//   xrefs ADDR             references to ADDR (with the containing function)
//   callers ADDR           functions calling the function containing ADDR
//   callees ADDR           functions called by the function containing ADDR
//   str REGEX              defined strings matching REGEX, with their addresses and xrefs
//   sym REGEX              symbols (labels/functions) matching REGEX
//   qwords ADDR N          N little-endian qwords from ADDR (vtables)
//   bytes ADDR N           hex bytes
//   func ADDR              function bounds/name
//   findbytes HEX          addresses where a hex byte pattern occurs (?? wildcards), first 50
//   quit                   stop the server
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class Serve extends GhidraScript {
  DecompInterface dec;

  Address A(String s) {
    s = s.trim();
    if (s.startsWith("0x") || s.matches("[0-9a-fA-F]+")) {
      long v = Long.parseUnsignedLong(s.startsWith("0x") ? s.substring(2) : s, 16);
      if (v < 0x140000000L) v += 0x140000000L;  // accept RVAs
      return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
    }
    List<Function> fs = getGlobalFunctions(s);
    if (!fs.isEmpty()) return fs.get(0).getEntryPoint();
    SymbolIterator it = currentProgram.getSymbolTable().getSymbols(s);
    if (it.hasNext()) return it.next().getAddress();
    throw new RuntimeException("bad address " + s);
  }

  String fname(Address a) {
    Function f = getFunctionContaining(a);
    if (f == null) return "<nofunc>";
    return f.getName() + "@" + f.getEntryPoint();
  }

  void run1(String line, StringBuilder o) throws Exception {
    String[] p = line.trim().split("\\s+", 2);
    String cmd = p[0];
    String arg = p.length > 1 ? p[1] : "";
    Listing L = currentProgram.getListing();
    Memory M = currentProgram.getMemory();
    switch (cmd) {
      case "decomp": {
        Function f = getFunctionContaining(A(arg));
        if (f == null) { o.append("no function\n"); break; }
        DecompileResults r = dec.decompileFunction(f, 120, monitor);
        o.append("// ").append(f.getName()).append(" @ ").append(f.getEntryPoint())
            .append(" size ").append(f.getBody().getNumAddresses()).append("\n");
        if (r.decompileCompleted()) o.append(r.getDecompiledFunction().getC());
        else o.append("decompile failed: ").append(r.getErrorMessage()).append("\n");
        break;
      }
      case "disasm": {
        String[] q = arg.split("\\s+");
        Address a = A(q[0]);
        int n = q.length > 1 ? Integer.parseInt(q[1]) : 30;
        Instruction ins = L.getInstructionAt(a);
        if (ins == null) ins = L.getInstructionAfter(a);
        for (int i = 0; i < n && ins != null; i++, ins = ins.getNext()) {
          StringBuilder hb = new StringBuilder();
          for (byte b : ins.getBytes()) hb.append(String.format("%02x", b));
          o.append(String.format("%s  %-24s %s", ins.getAddress(), hb, ins));
          Reference[] rs = ins.getReferencesFrom();
          for (Reference r : rs) {
            Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(r.getToAddress());
            if (s != null) o.append("   ; ").append(s.getName(true));
          }
          o.append("\n");
        }
        break;
      }
      case "xrefs": {
        Address a = A(arg);
        int n = 0;
        for (Reference r : getReferencesTo(a)) {
          o.append(r.getFromAddress()).append(" ").append(r.getReferenceType()).append(" in ")
              .append(fname(r.getFromAddress())).append("\n");
          if (++n > 400) { o.append("...\n"); break; }
        }
        break;
      }
      case "callers": {
        Function f = getFunctionContaining(A(arg));
        Set<String> seen = new TreeSet<>();
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
          seen.add(fname(r.getFromAddress()) + " from " + r.getFromAddress() + " " + r.getReferenceType());
        }
        for (String s : seen) o.append(s).append("\n");
        break;
      }
      case "callees": {
        Function f = getFunctionContaining(A(arg));
        for (Function c : f.getCalledFunctions(monitor)) o.append(c.getName()).append("@").append(c.getEntryPoint()).append("\n");
        break;
      }
      case "str": {
        Pattern pat = Pattern.compile(arg);
        int n = 0;
        DataIterator it = L.getDefinedData(true);
        while (it.hasNext()) {
          Data d = it.next();
          if (!d.hasStringValue()) continue;
          String v = String.valueOf(d.getValue());
          if (!pat.matcher(v).find()) continue;
          o.append(d.getAddress()).append(" \"").append(v.replace("\n", "\\n")).append("\"");
          int k = 0;
          for (Reference r : getReferencesTo(d.getAddress())) {
            o.append("\n    <- ").append(r.getFromAddress()).append(" ").append(fname(r.getFromAddress()));
            if (++k > 12) break;
          }
          o.append("\n");
          if (++n > 300) { o.append("...\n"); break; }
        }
        break;
      }
      case "sym": {
        Pattern pat = Pattern.compile(arg);
        int n = 0;
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
          Symbol s = it.next();
          String nm = s.getName(true);
          if (!pat.matcher(nm).find()) continue;
          o.append(s.getAddress()).append(" ").append(s.getSymbolType()).append(" ").append(nm).append("\n");
          if (++n > 400) { o.append("...\n"); break; }
        }
        break;
      }
      case "qwords": {
        String[] q = arg.split("\\s+");
        Address a = A(q[0]);
        int n = q.length > 1 ? Integer.parseInt(q[1]) : 16;
        for (int i = 0; i < n; i++) {
          Address x = a.add(8L * i);
          long v = M.getLong(x);
          String s = "";
          try {
            Address t = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
            Function f = getFunctionAt(t);
            if (f != null) s = f.getName();
            else { Symbol sy = currentProgram.getSymbolTable().getPrimarySymbol(t); if (sy != null) s = sy.getName(true); }
          } catch (Exception e) {}
          o.append(String.format("%s [%2d] %016x %s\n", x, i, v, s));
        }
        break;
      }
      case "bytes": {
        String[] q = arg.split("\\s+");
        Address a = A(q[0]);
        int n = q.length > 1 ? Integer.parseInt(q[1]) : 32;
        byte[] b = new byte[n];
        M.getBytes(a, b);
        for (int i = 0; i < n; i++) o.append(String.format("%02x%s", b[i], (i % 16 == 15) ? "\n" : " "));
        o.append("\n");
        break;
      }
      case "func": {
        Function f = getFunctionContaining(A(arg));
        if (f == null) o.append("none\n");
        else o.append(f.getName()).append(" ").append(f.getBody().getMinAddress()).append("-").append(f.getBody().getMaxAddress()).append("\n");
        break;
      }
      case "findbytes": {
        String hex = arg.replace(" ", "");
        byte[] v = new byte[hex.length() / 2], m = new byte[hex.length() / 2];
        for (int i = 0; i < v.length; i++) {
          String h = hex.substring(2 * i, 2 * i + 2);
          if (h.equals("??")) { v[i] = 0; m[i] = 0; }
          else { v[i] = (byte) Integer.parseInt(h, 16); m[i] = (byte) 0xff; }
        }
        Address start = currentProgram.getMinAddress();
        int n = 0;
        while (n < 50) {
          Address f = M.findBytes(start, v, m, true, monitor);
          if (f == null) break;
          o.append(f).append(" ").append(fname(f)).append("\n");
          start = f.add(1);
          n++;
        }
        break;
      }
      default:
        o.append("unknown command ").append(cmd).append("\n");
    }
  }

  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    Path dir = Paths.get(args.length > 0 ? args[0] : "/tmp/ghq");
    Files.createDirectories(dir);
    dec = new DecompInterface();
    DecompileOptions opt = new DecompileOptions();
    dec.setOptions(opt);
    dec.openProgram(currentProgram);
    println("Serve: ready on " + dir);
    Files.writeString(dir.resolve("READY"), "ready\n");
    while (true) {
      File[] reqs = dir.toFile().listFiles((d, n) -> n.endsWith(".req"));
      if (reqs == null || reqs.length == 0) { Thread.sleep(100); continue; }
      Arrays.sort(reqs);
      for (File rq : reqs) {
        String body;
        try {
          body = Files.readString(rq.toPath());
        } catch (IOException e) {
          continue;  // vanished (client gave up): skip it
        }
        rq.delete();
        String base = rq.getName().substring(0, rq.getName().length() - 4);
        StringBuilder o = new StringBuilder();
        boolean quit = false;
        for (String line : body.split("\n")) {
          if (line.isBlank()) continue;
          if (line.trim().equals("quit")) { quit = true; break; }
          o.append("### ").append(line.trim()).append("\n");
          try { run1(line, o); } catch (Exception e) { o.append("error: ").append(e).append("\n"); }
        }
        Path tmp = dir.resolve(base + ".tmp");
        Files.writeString(tmp, o.toString());
        Files.move(tmp, dir.resolve(base + ".out"), StandardCopyOption.ATOMIC_MOVE);
        if (quit) { dec.dispose(); return; }
      }
    }
  }
}
