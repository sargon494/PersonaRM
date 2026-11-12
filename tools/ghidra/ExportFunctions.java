// Exporta de un programa abierto en Ghidra SOLO direcciones y contadores (nada de bytes ni de
// codigo del juego): una fila por funcion con la direccion de entrada, su tamano y cuantas
// referencias la alcanzan por llamada, salto o dato (p. ej. una tabla de punteros).
//
// Uso (interfaz grafica): Window > Script Manager > Create New Script (Java) o abre este fichero
// desde "Script Directories", y ejecutalo con el programa ya analizado.
// Uso (headless): analyzeHeadless <proyecto> <nombre> -process <programa> -postScript ExportFunctions.java <salida.csv>
//
// @category PersonaRM

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.File;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        final String[] args = getScriptArgs();
        final File out = args.length > 0
            ? new File(args[0])
            : askFile("Guardar la lista de funciones (solo direcciones)", "Guardar");

        int count = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("entry,size,calls,jumps,data");
            final FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                final Function f = it.next();
                final Address entry = f.getEntryPoint();
                int calls = 0, jumps = 0, data = 0;
                final ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(entry);
                while (refs.hasNext()) {
                    final Reference r = refs.next();
                    final RefType t = r.getReferenceType();
                    if (t.isCall()) calls++;
                    else if (t.isJump()) jumps++;
                    else if (t.isData()) data++;
                }
                w.printf("0x%08X,%d,%d,%d,%d%n", entry.getOffset(), f.getBody().getNumAddresses(), calls, jumps, data);
                count++;
            }
        }
        println("ExportFunctions: " + count + " funciones escritas en " + out.getAbsolutePath());
    }
}
