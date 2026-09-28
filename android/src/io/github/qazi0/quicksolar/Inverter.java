package io.github.qazi0.quicksolar;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.Locale;

/**
 * Direct, read-only Modbus reader for GoodWe hybrid inverters of the ET family
 * (ES Uniq, ET, EH, BT, BH -- the register map of the goodwe library's et.py).
 *
 * Nothing about the site is built in: the inverter's address and transport come
 * from setup (found by Discovery), the battery size from the user. Two transports,
 * as the dongles differ: Modbus/TCP on port 502 (LAN/WiFi+LAN dongles) and
 * Modbus-RTU over UDP port 8899 (older WiFi sticks; replies start with AA 55).
 * Only function 0x03 (read holding registers) is ever sent.
 * Registers: 35100..35224 runtime, 37000..37023 BMS, 45356/45358 battery reserve.
 */
final class Inverter {
    static final int TCP_PORT = 502, UDP_PORT = 8899, UNIT = 0xF7;

    private volatile String host = "";
    private volatile boolean udp;
    private volatile double capacityKwh = 0;

    private Socket sock;
    private DatagramSocket dsock;
    private int tid;
    private int reserveOn = 10, reserveOff = 10, chargeLimit = -1;   // -1: the inverter doesn't report it
    private boolean haveReserve;
    private double batEma;
    private boolean emaInit;
    private volatile String json = "{\"ok\":0,\"err\":\"not set up\"}";
    private volatile long lastOk;

    String json() { return json; }
    boolean configured() { return host.length() > 0; }

    /** Where the inverter is and how big the battery is (from setup). */
    synchronized void configure(String host, String transport, double capacityKwh) {
        boolean changed = !host.equals(this.host) || udp != "udp".equals(transport);
        this.host = host == null ? "" : host.trim();
        this.udp = "udp".equals(transport);
        this.capacityKwh = capacityKwh;
        if (changed) { close(); haveReserve = false; emaInit = false; json = "{\"ok\":0,\"err\":\"connecting\"}"; }
    }

    /** One poll: read everything and rebuild the /api/now JSON. */
    synchronized void poll(int periodMs) {
        if (!configured()) { json = "{\"ok\":0,\"err\":\"not set up\"}"; return; }
        try {
            if (!haveReserve) {
                int[] v = read(45356, 3);
                if (v[0] <= 100) reserveOn = v[0];
                if (v[2] <= 100) reserveOff = v[2];
                try { int[] u = read(47760, 1); if (u[0] >= 10 && u[0] <= 100) chargeLimit = u[0]; }   // SoC upper limit
                catch (IOException ignored) { }                                                        // not reported: 100 %
                haveReserve = true;
            }
            int[] r = read(35100, 125);
            int[] m = read(37000, 24);
            build(r, m, periodMs);
        } catch (Exception e) {
            close();
            if (System.currentTimeMillis() - lastOk > 15000)
                json = "{\"ok\":0,\"err\":\"" + (e.getMessage() == null ? "no reply" : e.getMessage().replace("\"", "'")) + "\"}";
        }
    }

    synchronized void close() {
        try { if (sock != null) sock.close(); } catch (IOException ignored) { }
        if (dsock != null) dsock.close();
        sock = null; dsock = null;
    }

    /** Discovery check: does a GoodWe ET-family inverter answer at host over this transport? */
    static boolean probe(String host, String transport) {
        Inverter t = new Inverter();
        t.host = host; t.udp = "udp".equals(transport);
        try {
            int[] r = t.read(35100, 10);                 // runtime block: must answer with sane values
            int[] m = t.read(37000, 10);                 // BMS block: the hybrid (battery) family has it
            return r.length == 10 && m.length == 10;
        } catch (Exception e) {
            return false;
        } finally {
            t.close();
        }
    }

    /** The charge limit set in the inverter (SoC upper limit, 47760), or -1 if it doesn't report one. */
    static int readChargeLimit(String host, String transport) {
        Inverter t = new Inverter();
        t.host = host; t.udp = "udp".equals(transport);
        try { int v = t.read(47760, 1)[0]; return v >= 10 && v <= 100 ? v : -1; }
        catch (Exception e) { return -1; }
        finally { t.close(); }
    }

    private int[] read(int addr, int count) throws IOException {
        return udp ? readUdp(addr, count) : readTcp(addr, count);
    }

    private int[] readTcp(int addr, int count) throws IOException {
        if (sock == null) {
            sock = new Socket();
            sock.connect(new InetSocketAddress(host, TCP_PORT), 3000);
            sock.setSoTimeout(3000);
            sock.setTcpNoDelay(true);
        }
        tid = (tid + 1) & 0xffff;
        byte[] q = { (byte) (tid >> 8), (byte) tid, 0, 0, 0, 6, (byte) UNIT, 0x03,
                     (byte) (addr >> 8), (byte) addr, (byte) (count >> 8), (byte) count };
        OutputStream out = sock.getOutputStream();
        out.write(q);
        out.flush();
        DataInputStream in = new DataInputStream(sock.getInputStream());
        while (true) {
            byte[] h = new byte[9];
            in.readFully(h);
            int len = ((h[4] & 0xff) << 8) | (h[5] & 0xff);
            if ((h[7] & 0x80) != 0) throw new IOException("modbus exception " + (h[8] & 0xff));
            int bc = h[8] & 0xff;
            if (len < 3 || bc != len - 3) throw new IOException("bad frame");
            byte[] d = new byte[bc];
            in.readFully(d);
            int rid = ((h[0] & 0xff) << 8) | (h[1] & 0xff);
            if (rid != tid) continue;                    // stale reply
            if (bc != count * 2) throw new IOException("short reply");
            return words(d, 0, count);
        }
    }

    /* Modbus-RTU over UDP 8899 (goodwe's ModbusRtuReadCommand): request F7 03 addr count crc,
       reply AA 55 F7 03 <bytes> data.. crc (crc over everything after AA 55) */
    private int[] readUdp(int addr, int count) throws IOException {
        if (dsock == null) { dsock = new DatagramSocket(); dsock.setSoTimeout(2000); }
        byte[] q = { (byte) UNIT, 0x03, (byte) (addr >> 8), (byte) addr, (byte) (count >> 8), (byte) count, 0, 0 };
        int c = crc16(q, 0, 6); q[6] = (byte) c; q[7] = (byte) (c >> 8);
        InetAddress ip = InetAddress.getByName(host);
        byte[] buf = new byte[512];
        IOException last = null;
        for (int attempt = 0; attempt < 3; attempt++) {
            dsock.send(new DatagramPacket(q, q.length, ip, UDP_PORT));
            try {
                while (true) {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    dsock.receive(p);
                    int n = p.getLength();
                    if (n < 7 || (buf[0] & 0xff) != 0xAA || (buf[1] & 0xff) != 0x55) continue;
                    if ((buf[3] & 0x80) != 0) throw new IOException("modbus exception " + (buf[4] & 0xff));
                    int bc = buf[4] & 0xff;
                    if (bc != count * 2 || n < 5 + bc + 2) continue;         // not ours
                    int crc = crc16(buf, 2, 3 + bc);
                    if ((buf[5 + bc] & 0xff) != (crc & 0xff) || (buf[6 + bc] & 0xff) != ((crc >> 8) & 0xff)) continue;
                    return words(buf, 5, count);
                }
            } catch (java.net.SocketTimeoutException e) { last = e; }
        }
        throw last != null ? last : new IOException("no reply");
    }

    private static int[] words(byte[] d, int off, int count) {
        int[] v = new int[count];
        for (int i = 0; i < count; i++) v[i] = ((d[off + 2 * i] & 0xff) << 8) | (d[off + 2 * i + 1] & 0xff);
        return v;
    }

    private static int crc16(byte[] b, int off, int len) {
        int crc = 0xFFFF;
        for (int i = off; i < off + len; i++) {
            crc ^= b[i] & 0xff;
            for (int k = 0; k < 8; k++) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
        return crc;
    }

    private static int u16(int[] b, int base, int a) { return b[a - base]; }
    private static int s16(int[] b, int base, int a) { return (short) b[a - base]; }
    private static long u32(int[] b, int base, int a) { return ((long) b[a - base] << 16) | b[a - base + 1]; }
    private static int s32(int[] b, int base, int a) { return (int) u32(b, base, a); }

    /* labels as in solard.c (from goodwe/const.py); null = unnamed bit, "" = skipped */
    private static final String[] BAT_MODES = { "No battery", "Standby", "Discharging", "Charging", "To be charged", "To be discharged" };
    private static final String[] WORK_MODES = { "Wait Mode", "Normal (On-Grid)", "Normal (Off-Grid)", "Fault Mode", "Flash Mode", "Check Mode" };
    private static final String[] GRID_MODES = { "Not connected to grid", "Connected to grid", "Fault" };
    private static final String[] PV_MODES = { "PV panels not connected", "PV panels connected, no power", "PV panels connected, producing power" };
    private static final String[] ERROR_CODES = new String[32], DIAG_CODES = new String[32], BMS_ALARMS = new String[32], BMS_WARNINGS = new String[32];
    static {
        String[] e = { "GFCI Device Check Failure", "AC HCT Check Failure", "", "DCI Consistency Failure", "GFCI Consistency Failure", "",
            "GFCI Device Failure", "Relay Device Failure", "AC HCT Failure", "Utility Loss", "Ground I Failure", "DC Bus High",
            "InternalFan Failure", "Over Temperature", "Utility Phase Failure", "PV Over Voltage", "External Fan Failure", "Vac Failure",
            "Isolation Failure", "DC Injection High", "Back-Up Over Load", "", "Fac Consistency Failure", "Vac Consistency Failure", "",
            "Relay Check Failure", "", "PhaseAngleFailure", "DSP communication failure", "Fac Failure", "EEPROM R/W Failure",
            "Internal Communication Failure" };
        System.arraycopy(e, 0, ERROR_CODES, 0, 32);
        String[] d = { "Battery voltage low", "Battery SOC low", "Battery SOC in back", "BMS: Discharge disabled", "Discharge time on",
            "Charge time on", "Discharge Driver On", "BMS: Discharge current low", "APP: Discharge current too low",
            "Meter communication failure", "Meter connection reversed", "Self-use load light", "EMS: discharge current is zero",
            "Discharge BUS high PV voltage", "Battery Disconnected", "Battery Overcharged", "BMS: Temperature too high",
            "BMS: Charge too high", "BMS: Charge disabled", "Self-use off", "SOC delta too volatile", "Battery self discharge too high",
            "Battery SOC low (off-grid)", "Grid wave unstable", "Export power limit set", "PF value set", "Real power limit set",
            "DC output on", "SOC protect off", null, "BMS: Emergency charging", null };
        System.arraycopy(d, 0, DIAG_CODES, 0, 32);
        String[] a = { "Charging over-voltage 2", "Discharging under-voltage 2", "Cell temperature high 2", "Cell temperature low 2",
            "Charging over-current 2", "Discharging over-current 2", "Precharge fault", "DC bus fault", "Battery break", "Battery lock",
            "Discharging circuit failure", "Charging circuit failure", "Communication failure 2", "Cell temperature high 3",
            "Discharging under-voltage 3", "Charging over-voltage 3" };
        System.arraycopy(a, 0, BMS_ALARMS, 0, a.length);
        String[] w = { "Charging over-voltage 1", "Discharging under-voltage 1", "Cell temperature high 1", "Cell temperature low 1",
            "Charging over-current 1", "Discharging over-current 1", "Communication failure 1", "System reboot", "Cell imbalance",
            "System temperature low 1", "System temperature low 2", "System temperature high" };
        System.arraycopy(w, 0, BMS_WARNINGS, 0, w.length);
    }
    private static String label(String[] t, int v) { return v >= 0 && v < t.length ? t[v] : "Unknown"; }
    private static String bits(long v, String[] lab) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 32; i++) {
            if (((v >> i) & 1) == 0) continue;
            String s = lab[i] == null ? "err" + i : lab[i];
            if (s.isEmpty()) continue;
            if (sb.length() > 0) sb.append(", ");
            sb.append(s);
        }
        return sb.toString();
    }
    private static double volt10(int raw) { return raw == 0xffff ? 0 : raw / 10.0; }
    /** read_temp: -1 / 32767 = not reported -> JSON null */
    private static String temp10(int raw) { return raw == -1 || raw == 32767 ? "null" : String.format(Locale.US, "%.1f", raw / 10.0); }

    private void build(int[] r, int[] m, int periodMs) {
        int B = 35100, M = 37000;
        double vpv1 = volt10(u16(r, B, 35103)), ipv1 = volt10(u16(r, B, 35104));
        double vpv2 = volt10(u16(r, B, 35107)), ipv2 = volt10(u16(r, B, 35108));
        long ppv1 = u32(r, B, 35105), ppv2 = u32(r, B, 35109), ppv = ppv1 + ppv2;
        int pv1Mode = u16(r, B, 35120) & 0xff, pv2Mode = u16(r, B, 35120) >> 8;
        double vgrid = volt10(u16(r, B, 35121)), igrid = volt10(u16(r, B, 35122)), fgrid = s16(r, B, 35123) / 100.0;
        int pgrid = s16(r, B, 35125), gridMode = u16(r, B, 35136), invTotalW = s16(r, B, 35138), gridW = s16(r, B, 35140);
        double vout = volt10(u16(r, B, 35145)), iout = volt10(u16(r, B, 35146)), fout = s16(r, B, 35147) / 100.0;
        int pout = s16(r, B, 35150);
        int backupW = s16(r, B, 35170), loadW = s16(r, B, 35172), upsLoad = u16(r, B, 35173);
        int tAir = s16(r, B, 35174), tMod = s16(r, B, 35175), tInv = s16(r, B, 35176);
        double vbus = volt10(u16(r, B, 35178)), vnbus = volt10(u16(r, B, 35179));
        double vbat = volt10(u16(r, B, 35180)), ibat = s16(r, B, 35181) / 10.0;
        int batW = s32(r, B, 35182), batMode = u16(r, B, 35184), warnCode = u16(r, B, 35185), workMode = u16(r, B, 35187);
        long errBits = u32(r, B, 35189), diagBits = u32(r, B, 35220);
        double eTotal = u32(r, B, 35191) / 10.0, eDay = u32(r, B, 35193) / 10.0, eExpTotal = u32(r, B, 35195) / 10.0;
        long hTotal = u32(r, B, 35197);
        double eExp = u16(r, B, 35199) / 10.0, eImpTotal = u32(r, B, 35200) / 10.0, eImp = u16(r, B, 35202) / 10.0;
        double eLoadTotal = u32(r, B, 35203) / 10.0, eLoad = u16(r, B, 35205) / 10.0;
        double eChgTotal = u32(r, B, 35206) / 10.0, eChg = u16(r, B, 35208) / 10.0;
        double eDisTotal = u32(r, B, 35209) / 10.0, eDis = u16(r, B, 35211) / 10.0;
        int houseW = Math.max(0, invTotalW - gridW);      // house = inverter AC output + grid import (as solard)
        double batVi = vbat * ibat;
        int bms = u16(m, M, 37000), batStatus = u16(m, M, 37002), batTemp = s16(m, M, 37003);
        int chgLim = u16(m, M, 37004), disLim = u16(m, M, 37005), soc = u16(m, M, 37007), soh = u16(m, M, 37008);
        int modules = u16(m, M, 37009), protocol = u16(m, M, 37011);
        long batErr = ((long) u16(m, M, 37012) << 16) | u16(m, M, 37006);   // (H << 16) | L
        long batWarn = ((long) u16(m, M, 37013) << 16) | u16(m, M, 37010);

        double a = periodMs / 300000.0;        // ~5 min average, like solard
        if (!emaInit) { batEma = batW; emaInit = true; } else batEma += a * (batW - batEma);

        int reserve = gridMode == 1 ? reserveOn : reserveOff;
        double stored = soc / 100.0 * capacityKwh;
        double usable = Math.max(0, (soc - reserve) / 100.0 * capacityKwh);
        double toFull = (100 - soc) / 100.0 * capacityKwh;
        double emptyH = batEma > 30 && capacityKwh > 0 ? usable * 1000.0 / batEma : -1;
        double fullH = batEma < -30 && capacityKwh > 0 ? toFull * 1000.0 / -batEma : -1;
        long now = System.currentTimeMillis();

        StringBuilder j = new StringBuilder(4096);
        j.append(String.format(Locale.US, "{\"ok\":1,\"ts\":%d,\"age_s\":0,\"err\":\"\",\"source\":\"inverter\",", now));
        j.append(String.format(Locale.US, "\"pv\":{\"w\":%d,\"s1\":{\"v\":%.1f,\"a\":%.1f,\"w\":%d,\"mode\":%d,\"mode_label\":\"%s\"},"
            + "\"s2\":{\"v\":%.1f,\"a\":%.1f,\"w\":%d,\"mode\":%d,\"mode_label\":\"%s\"}},",
            ppv, vpv1, ipv1, ppv1, pv1Mode, label(PV_MODES, pv1Mode), vpv2, ipv2, ppv2, pv2Mode, label(PV_MODES, pv2Mode)));
        j.append(String.format(Locale.US, "\"home_w\":%d,\"backup_w\":%d,\"load_w\":%d,\"dc_net_w\":%d,\"loss_w\":%.0f,",
            houseW, backupW, loadW, ppv + batW - gridW, ppv + batVi - gridW - houseW));
        j.append(String.format(Locale.US, "\"grid\":{\"w\":%d,\"mode\":%d,\"mode_label\":\"%s\",\"up\":%s,\"v\":%.1f,\"a\":%.1f,\"hz\":%.2f,"
            + "\"inv_port_w\":%d,\"import_total_kwh\":%.1f},",
            gridW, gridMode, label(GRID_MODES, gridMode), gridMode == 1 ? "true" : "false", vgrid, igrid, fgrid, pgrid, eImpTotal));
        j.append(String.format(Locale.US, "\"battery\":{\"w\":%d,\"w_vi\":%.0f,\"w_avg\":%.0f,\"v\":%.1f,\"a\":%.1f,\"soc\":%d,\"soh\":%d,\"temp\":%s,"
            + "\"mode\":\"%s\",\"capacity_kwh\":%.1f,\"reserve\":%d,\"stored_kwh\":%.2f,\"usable_kwh\":%.2f,"
            + "\"hours_to_empty\":%.2f,\"hours_to_full\":%.2f,\"charge_limit\":%s,",
            batW, batVi, batEma, vbat, ibat, soc, soh, temp10(batTemp),
            label(BAT_MODES, batMode), capacityKwh, reserve, stored, usable, emptyH, fullH, chargeLimit > 0 ? String.valueOf(chargeLimit) : "null"));
        j.append(String.format(Locale.US, "\"charge_limit_a\":%d,\"discharge_limit_a\":%d,\"status\":%d,\"bms\":%d,\"protocol\":%d,\"modules\":%d,"
            + "\"warnings\":\"%s\",\"errors\":\"%s\",\"warning_bits\":%d,\"error_bits\":%d,"
            + "\"cell_v_max\":null,\"cell_v_min\":null,\"cell_t_max\":null,\"cell_t_min\":null,"
            + "\"life_charge_kwh\":%.1f,\"life_discharge_kwh\":%.1f,\"cycles\":%.1f},",
            chgLim, disLim, batStatus, bms, protocol, modules, bits(batWarn, BMS_WARNINGS), bits(batErr, BMS_ALARMS), batWarn, batErr,
            eChgTotal, eDisTotal, capacityKwh > 0 ? eDisTotal / capacityKwh : 0));
        j.append(String.format(Locale.US, "\"inverter\":{\"v\":%.1f,\"a\":%.1f,\"hz\":%.2f,\"w\":%d,\"total_w\":%d,\"ups_load\":%d,"
            + "\"temp_air\":%s,\"temp_radiator\":%s,\"temp_module\":%s,\"bus_v\":%.1f,\"nbus_v\":%.1f,"
            + "\"work_mode\":%d,\"work_mode_label\":\"%s\",\"errors\":\"%s\",\"error_bits\":%d,\"warning_code\":%d,"
            + "\"diag\":\"%s\",\"diag_bits\":%d,\"hours_total\":%d},",
            vout, iout, fout, pout, invTotalW, upsLoad, temp10(tAir), temp10(tInv), tMod > 0 ? temp10(tMod) : "null", vbus, vnbus,
            workMode, label(WORK_MODES, workMode), bits(errBits, ERROR_CODES), errBits, warnCode, bits(diagBits, DIAG_CODES), diagBits, hTotal));
        j.append(String.format(Locale.US, "\"work_mode\":%d,\"inv_temp\":%s,", workMode, temp10(tInv)));
        /* no live integration here: today's totals are the inverter's own counters */
        j.append(String.format(Locale.US, "\"today\":{\"pv\":%.1f,\"load\":%.1f,\"charge\":%.1f,\"discharge\":%.1f,\"import\":%.1f,\"export\":0,"
            + "\"basis\":\"counters\",\"counter\":{\"pv\":%.1f,\"load\":%.1f,\"charge\":%.1f,\"discharge\":%.1f,\"import\":%.1f,\"ac_out_35199\":%.1f}},",
            eDay, eLoad, eChg, eDis, eImp, eDay, eLoad, eChg, eDis, eImp, eExp));
        j.append(String.format(Locale.US, "\"lifetime\":{\"pv\":%.1f,\"load\":%.1f,\"import\":%.1f,\"ac_out_35195\":%.1f,\"charge\":%.1f,\"discharge\":%.1f},"
            + "\"pv_total_kwh\":%.1f}", eTotal, eLoadTotal, eImpTotal, eExpTotal, eChgTotal, eDisTotal, eTotal));
        json = j.toString();
        todaySummary = String.format(Locale.US,
            "{\"pv\":%.1f,\"load\":%.1f,\"charge\":%.1f,\"discharge\":%.1f,\"import\":%.1f,\"soc_min\":-1,\"soc_max\":-1,"
            + "\"peak_pv\":0,\"peak_pv_min\":0,\"peak_load\":0,\"peak_load_min\":0,\"basis\":\"counters\"}", eDay, eLoad, eChg, eDis, eImp);
        lastOk = now;
    }

    /** Today's totals from the live counters (used when no history server is reachable). */
    volatile String todaySummary = "null";
}
