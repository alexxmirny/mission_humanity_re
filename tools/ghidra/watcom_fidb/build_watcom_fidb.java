// Create + populate a FID database from every program in the project.
//@category FunctionID
import java.io.File;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.db.*;
import ghidra.feature.fid.service.*;
import ghidra.program.model.lang.LanguageID;
import ghidra.framework.model.*;

public class build_watcom_fidb extends GhidraScript {
    private void walk(DomainFolder folder, ArrayList<DomainFile> out) {
        for (DomainFile df : folder.getFiles()) out.add(df);
        for (DomainFolder sub : folder.getFolders()) walk(sub, out);
    }
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        File dbfile = new File(a[0]);
        String langid = a.length > 1 ? a[1] : "x86:LE:32:watcom";
        String name   = a.length > 2 ? a[2] : "Watcom";
        String ver    = a.length > 3 ? a[3] : "11.0";
        String var    = a.length > 4 ? a[4] : "clib3r";

        FidFileManager fm = FidFileManager.getInstance();
        if (!dbfile.exists()) { fm.createNewFidDatabase(dbfile); println("[build] created " + dbfile); }
        fm.addUserFidFile(dbfile);
        FidFile fidFile = null;
        List<FidFile> added = fm.getUserAddedFiles();
        for (FidFile f : added)
            if (new File(f.getPath()).getName().equalsIgnoreCase(dbfile.getName())) fidFile = f;
        if (fidFile == null) fidFile = added.get(added.size() - 1);

        FidDB fidDb = fidFile.getFidDB(true);
        FidService service = new FidService();
        ArrayList<DomainFile> progs = new ArrayList<>();
        walk(state.getProject().getProjectData().getRootFolder(), progs);
        println("[build] programs: " + progs.size());

        FidPopulateResult result = service.createNewLibraryFromPrograms(
            fidDb, name, ver, var, progs, null, new LanguageID(langid), null, null, monitor);
        println("[build] attempted=" + result.getTotalAttempted()
            + " added=" + result.getTotalAdded()
            + " excluded=" + result.getTotalExcluded());
        fidDb.saveDatabase("build", monitor);
        fidDb.close();
        println("[build] DONE " + dbfile);
    }
}
