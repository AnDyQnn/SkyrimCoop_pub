// Locate Papyrus native function implementations by their registration strings.
// Skyrim registers natives as NativeFunction(name, className, fnPtr, vm): on AArch64 the
// name and class are loaded with ADRP+ADD right before the function pointer, then the
// constructor is called. For every reference to a wanted name string we collect the
// code addresses referenced by the next instructions of the same function.
// Output: notes/natives.tsv  (name, string xref, candidate functions)
//@category SkyrimCoop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class FindPapyrusNatives extends GhidraScript {

    // name -> class it belongs to (for disambiguation in the output only)
    static final String[][] WANTED = {
        {"PlaceAtMe", "ObjectReference"}, {"SetPosition", "ObjectReference"}, {"MoveTo", "ObjectReference"},
        {"GetPositionX", "ObjectReference"}, {"GetAngleZ", "ObjectReference"}, {"SetAngle", "ObjectReference"},
        {"TranslateTo", "ObjectReference"}, {"Disable", "ObjectReference"}, {"Enable", "ObjectReference"},
        {"Delete", "ObjectReference"}, {"GetParentCell", "ObjectReference"}, {"GetWorldSpace", "ObjectReference"},
        {"PlayAnimation", "ObjectReference"}, {"GetFormID", "Form"},
        {"GetPlayer", "Game"}, {"GetForm", "Game"}, {"GetFormFromFile", "Game"},
        {"SendAnimationEvent", "Debug"}, {"Notification", "Debug"}, {"MessageBox", "Debug"}, {"Trace", "Debug"},
        {"IsSneaking", "Actor"}, {"IsRunning", "Actor"}, {"IsSprinting", "Actor"}, {"IsWeaponDrawn", "Actor"},
        {"IsInCombat", "Actor"}, {"IsDead", "Actor"}, {"GetEquippedWeapon", "Actor"}, {"EquipItem", "Actor"},
        {"SetAlpha", "Actor"}, {"KeepOffsetFromActor", "Actor"}, {"PathToReference", "Actor"},
        {"SetGhost", "Actor"}, {"SetRestrained", "Actor"}, {"SetDontMove", "Actor"}, {"EnableAI", "Actor"},
        {"GetActorBase", "Actor"}, {"MoveToPackageLocation", "Actor"},
    };

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();
        File out = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : "natives.tsv");
        try (PrintWriter pw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out), StandardCharsets.UTF_8))) {
            pw.println("name\tclass\tstring\txref\tcandidates");
            for (String[] w : WANTED) {
                byte[] needle = (w[0] + "\0").getBytes(StandardCharsets.US_ASCII);
                List<Address> hits = new ArrayList<>();
                Address start = mem.getMinAddress();
                while (start != null) {
                    Address a = mem.findBytes(start, needle, null, true, monitor);
                    if (a == null) break;
                    // must be the start of the string (previous byte is a NUL)
                    try { if (mem.getByte(a.subtract(1)) == 0) hits.add(a); } catch (Exception e) { hits.add(a); }
                    start = a.add(1);
                }
                boolean any = false;
                for (Address s : hits) {
                    for (Reference r : refs.getReferencesTo(s)) {
                        Address from = r.getFromAddress();
                        Function f = getFunctionContaining(from);
                        if (f == null) continue;
                        // collect code references in the next 12 instructions
                        List<String> cands = new ArrayList<>();
                        Instruction ins = listing.getInstructionAt(from);
                        for (int i = 0; i < 12 && ins != null; i++, ins = ins.getNext()) {
                            for (Reference cr : ins.getReferencesFrom()) {
                                Address to = cr.getToAddress();
                                Function tf = getFunctionAt(to);
                                if (tf != null && !cands.contains(to.toString()) && cr.getReferenceType().isData())
                                    cands.add(to.toString());
                            }
                        }
                        pw.println(w[0] + "\t" + w[1] + "\t" + s + "\t" + from + "\t" + String.join(",", cands));
                        any = true;
                    }
                }
                if (!any) pw.println(w[0] + "\t" + w[1] + "\t" + (hits.isEmpty() ? "-" : hits.get(0).toString()) + "\t-\t");
            }
        }
        println("natives written to " + out.getAbsolutePath());
    }
}
