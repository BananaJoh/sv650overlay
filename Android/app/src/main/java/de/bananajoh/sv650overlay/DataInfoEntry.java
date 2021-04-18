package de.bananajoh.sv650overlay;


public class DataInfoEntry {
    public String label;
    public String unit;
    public int showAtPos;


    public DataInfoEntry(String label, String unit, int showAtPos) {
        this.label = label;
        this.unit = unit;
        this.showAtPos = showAtPos;
    }
}