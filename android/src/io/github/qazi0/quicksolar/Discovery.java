package io.github.qazi0.quicksolar;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Finds a GoodWe inverter (and, if there is one, a solard history server) on the
 * local network, so nothing about the site has to be built into the app.
 *
 *  1. GoodWe's own discovery: the text "WIFIKIT-214028-READ" broadcast to UDP 48899;
 *     WiFi/LAN dongles reply "ip,mac,name" (what the goodwe library does).
 *  2. A scan of this device's /24 for TCP 502 (Modbus/TCP dongles) and TCP 8768
 *     (solard), for dongles that don't answer the broadcast.
 *  3. Every candidate is checked with a real read-only Modbus read (function 0x03)
 *     of the hybrid register blocks -- over TCP 502 first, then UDP 8899.
 *     solard candidates must answer /ping with "solard ok".
 */
final class Discovery {
    private Discovery() { }

    static String run() {
        final Map<String, String> names = Collections.synchronizedMap(new LinkedHashMap<String, String>());
        final List<String> modbus = Collections.synchronizedList(new ArrayList<String>());
        final List<String> servers = Collections.synchronizedList(new ArrayList<String>());
        String self = localIPv4();
        String subnet = self == null ? null : self.substring(0, self.lastIndexOf('.') + 1);

        Thread bc = new Thread(new Runnable() { public void run() { broadcast(names); } }, "goodwe-discover");
        bc.start();

        if (subnet != null) {
            ExecutorService pool = Executors.newFixedThreadPool(48);
            for (int i = 1; i < 255; i++) {
                final String ip = subnet + i;
                if (ip.equals(self)) continue;
                pool.execute(new Runnable() { public void run() {
                    if (open(ip, Inverter.TCP_PORT, 350)) modbus.add(ip);
                    if (open(ip, 8768, 350) && "solard ok".equals(get("http://" + ip + ":8768/ping"))) servers.add("http://" + ip + ":8768");
                } });
            }
            pool.shutdown();
            try { pool.awaitTermination(15, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
        }
        try { bc.join(4000); } catch (InterruptedException ignored) { }

        /* stable order: lowest address first (e.g. a server's wired address before its WiFi one) */
        synchronized (servers) {
            Collections.sort(servers, BY_IP);
            /* one server reachable on several addresses (wired + WiFi) answers with the same
               reading: keep only its lowest address */
            List<String> seen = new ArrayList<String>(), keep = new ArrayList<String>();
            for (String u : servers) {
                String now = get(u + "/api/now"), id = now == null ? u : now.replaceAll("^.*?\"ts\":(\\d+).*$", "$1");
                if (!seen.contains(id)) { seen.add(id); keep.add(u); }
            }
            servers.clear(); servers.addAll(keep);
        }
        synchronized (modbus) { Collections.sort(modbus, BY_IP); }
        /* candidates: broadcast replies first, then open 502 ports */
        List<String> cand = new ArrayList<String>(names.keySet());
        synchronized (modbus) { for (String ip : modbus) if (!cand.contains(ip)) cand.add(ip); }
        StringBuilder j = new StringBuilder("{\"self\":").append(q(self)).append(",\"inverters\":[");
        int k = 0;
        for (String ip : cand) {
            String transport = modbus.contains(ip) && Inverter.probe(ip, "tcp") ? "tcp"
                             : Inverter.probe(ip, "udp") ? "udp" : null;
            String name = names.get(ip);
            int lim = transport == null ? -1 : Inverter.readChargeLimit(ip, transport);
            j.append(k++ > 0 ? "," : "").append("{\"ip\":").append(q(ip)).append(",\"name\":").append(q(name))
             .append(",\"transport\":").append(q(transport)).append(",\"ok\":").append(transport != null)
             .append(",\"charge_limit\":").append(lim > 0 ? String.valueOf(lim) : "null").append('}');
        }
        j.append("],\"servers\":[");
        k = 0;
        synchronized (servers) { for (String s : servers) j.append(k++ > 0 ? "," : "").append(q(s)); }
        return j.append("]}").toString();
    }

    private static long ipNum(String s) {
        String t = s.replaceAll("^https?://", "").replaceAll(":.*$", ""); long v = 0;
        for (String p : t.split("\\.")) { try { v = v * 256 + Integer.parseInt(p); } catch (NumberFormatException e) { return Long.MAX_VALUE; } }
        return v;
    }
    private static final java.util.Comparator<String> BY_IP = new java.util.Comparator<String>() {
        public int compare(String a, String b) { return Long.compare(ipNum(a), ipNum(b)); }
    };

    /** Check one address typed in by hand: {"ip":..,"transport":"tcp"|"udp"|null,"ok":..} */
    static String probeOne(String ip) {
        String t = Inverter.probe(ip, "tcp") ? "tcp" : Inverter.probe(ip, "udp") ? "udp" : null;
        int lim = t == null ? -1 : Inverter.readChargeLimit(ip, t);
        return "{\"ip\":" + q(ip) + ",\"transport\":" + q(t) + ",\"ok\":" + (t != null) + ",\"charge_limit\":" + (lim > 0 ? String.valueOf(lim) : "null") + "}";
    }

    /** GoodWe dongle discovery broadcast; replies look like "192.168.1.50,<MAC>,<serial>". */
    private static void broadcast(Map<String, String> out) {
        DatagramSocket s = null;
        try {
            s = new DatagramSocket();
            s.setBroadcast(true);
            s.setSoTimeout(700);
            byte[] msg = "WIFIKIT-214028-READ".getBytes("US-ASCII");
            List<InetAddress> targets = new ArrayList<InetAddress>();
            targets.add(InetAddress.getByName("255.255.255.255"));
            InetAddress b = localBroadcast();
            if (b != null) targets.add(b);
            long end = System.currentTimeMillis() + 3000;
            int round = 0;
            byte[] buf = new byte[256];
            while (System.currentTimeMillis() < end) {
                if (round++ < 3) for (InetAddress t : targets) s.send(new DatagramPacket(msg, msg.length, t, 48899));
                try {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    s.receive(p);
                    String r = new String(buf, 0, p.getLength(), "US-ASCII").trim();
                    String[] f = r.split(",");
                    String ip = f.length > 0 && f[0].matches("\\d+\\.\\d+\\.\\d+\\.\\d+") ? f[0] : p.getAddress().getHostAddress();
                    if (!out.containsKey(ip)) out.put(ip, f.length > 2 ? f[2] : "GoodWe dongle");
                } catch (SocketTimeoutException ignored) { }
            }
        } catch (Exception ignored) {
        } finally {
            if (s != null) s.close();
        }
    }

    private static boolean open(String ip, int port, int ms) {
        Socket s = new Socket();
        try { s.connect(new InetSocketAddress(ip, port), ms); return true; }
        catch (Exception e) { return false; }
        finally { try { s.close(); } catch (Exception ignored) { } }
    }

    static String get(String url) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(1000); c.setReadTimeout(1500);
            if (c.getResponseCode() != 200) { c.disconnect(); return null; }
            InputStream in = c.getInputStream();
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[512]; int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
            in.close(); c.disconnect();
            return o.toString("UTF-8").trim();
        } catch (Exception e) { return null; }
    }

    /** This device's LAN address (WiFi or Ethernet), or null. */
    static String localIPv4() {
        InterfaceAddress a = lanAddress();
        return a == null ? null : a.getAddress().getHostAddress();
    }

    private static InetAddress localBroadcast() {
        InterfaceAddress a = lanAddress();
        return a == null ? null : a.getBroadcast();
    }

    /** The LAN interface: real WiFi/Ethernet first (wlan*, eth*, en*), never virtual ones
        (docker/bridges/VPN/mobile data), 192.168.x preferred over other private ranges. */
    private static InterfaceAddress lanAddress() {
        try {
            Enumeration<NetworkInterface> it = NetworkInterface.getNetworkInterfaces();
            InterfaceAddress best = null; int bestScore = -1;
            while (it != null && it.hasMoreElements()) {
                NetworkInterface ni = it.nextElement();
                if (!ni.isUp() || ni.isLoopback() || ni.isVirtual() || ni.isPointToPoint()) continue;
                String n = ni.getName();
                if (n.startsWith("docker") || n.startsWith("br-") || n.startsWith("veth") || n.startsWith("tun")
                        || n.startsWith("tailscale") || n.startsWith("utun") || n.startsWith("rmnet") || n.startsWith("p2p")) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress ad = ia.getAddress();
                    if (!(ad instanceof Inet4Address) || !ad.isSiteLocalAddress()) continue;
                    int score = (n.startsWith("wlan") || n.startsWith("eth") || n.startsWith("en") ? 2 : 0)
                              + (ad.getHostAddress().startsWith("192.168.") ? 1 : 0);
                    if (score > bestScore) { best = ia; bestScore = score; }
                }
            }
            return best;
        } catch (Exception e) { return null; }
    }

    private static String q(String s) { return s == null ? "null" : "\"" + s.replace("\\", "\\\\").replace("\"", "'") + "\""; }
}
