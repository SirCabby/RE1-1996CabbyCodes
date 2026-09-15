// Applies tools/decomp_symbols.py's list to the current program: an "f" line
// creates (and names) a function at its address, a "d" line a primary label.
//   analyzeHeadless ghidra_proj RE1 -process ResidentEvil.fixed.exe -noanalysis \
//       -scriptPath tools/ghidra -postScript ApplyNames.java ghidra_proj/decomp_symbols.txt
//@category RE1
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

public class ApplyNames extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    File file = new File(args.length > 0 ? args[0] : "ghidra_proj/decomp_symbols.txt");
    int funcs = 0, labels = 0, skipped = 0;
    try (BufferedReader r = new BufferedReader(new FileReader(file))) {
      for (String line; (line = r.readLine()) != null;) {
        String[] p = line.trim().split("\\s+");
        if (p.length < 3 || p[0].startsWith("#")) continue;
        try {
          Address a = toAddr(Long.parseLong(p[0], 16));
          if (a == null || getMemoryBlock(a) == null) {
            skipped++;
            continue;
          }
          if (p[1].equals("f")) {
            Function fn = getFunctionAt(a);
            if (fn == null) {
              disassemble(a);
              fn = createFunction(a, p[2]);
            }
            if (fn == null) {
              skipped++;
              continue;
            }
            fn.setName(p[2], SourceType.USER_DEFINED);
            funcs++;
          } else {
            createLabel(a, p[2], true, SourceType.USER_DEFINED);
            labels++;
          }
        } catch (Exception e) {
          skipped++;
        }
      }
    }
    println("ApplyNames: " + funcs + " functions, " + labels + " labels, " + skipped + " skipped");
  }
}
