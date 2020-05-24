package de.bananajoh.sv650overlay;


public class DataInfoEntry {
    public String label;
    public String unit;
    public boolean show;

    public DataInfoEntry(String label, String unit, boolean show) {
        this.label = label;
        this.unit = unit;
        this.show = show;
    }
}