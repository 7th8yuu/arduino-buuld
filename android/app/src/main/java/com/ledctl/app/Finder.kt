package com.ledctl.app

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

const val TCP_PORT = 7777

/** Đọc một dòng (kết thúc bằng \n) từ socket, tối đa maxMs mili giây. null nếu hết giờ hoặc bị đóng. */
fun readLine(ins: InputStream, maxMs: Int): String? {
    val sb = ByteArrayOutputStream()
    val end = System.currentTimeMillis() + maxMs
    while (System.currentTimeMillis() < end) {
        val b = try {
            ins.read()
        } catch (e: SocketTimeoutException) {
            continue
        }
        if (b < 0) return null
        if (b == '\n'.code) return sb.toString("UTF-8").trim()
        if (sb.size() > 2000) return null
        sb.write(b)
    }
    return null
}

/**
 * Tìm thiết bị LED trong mạng. Thử lần lượt: địa chỉ người dùng nhập / đã lưu, mDNS (_ledctl._tcp),
 * rồi quét cả dải mạng /24 tìm cổng 7777. Thiết bị nào trả lời HELLO đúng giao thức thì nhận.
 */
class Finder(private val ctx: Context) {

    class Found(val socket: Socket, val host: String, val hello: String) {
        fun close() {
            try {
                socket.close()
            } catch (_: Exception) {
            }
        }
    }

    fun find(hosts: List<String>, alive: () -> Boolean, progress: (String) -> Unit): Found? {
        val prefix = localPrefix()
        val list = ArrayList<String>(hosts)
        // đang nối thẳng vào WiFi LED-Setup của thiết bị
        if (prefix == "192.168.4." && !list.contains("192.168.4.1")) list.add("192.168.4.1")
        for (h in list) {
            if (!alive()) return null
            probe(h, TCP_PORT, 1500)?.let { return it }
        }
        if (!alive()) return null
        progress("Đang tìm bằng mDNS…")
        viaNsd(3500, alive)?.let { return it }
        if (!alive() || prefix == null) return null
        progress("Đang quét mạng WiFi…")
        return scan(prefix, alive)
    }

    private fun probe(host: String, port: Int, connectMs: Int): Found? {
        val s = Socket()
        try {
            s.connect(InetSocketAddress(host, port), connectMs)
            s.tcpNoDelay = true
            s.soTimeout = 1500
            val o = s.getOutputStream()
            o.write("HELLO\n".toByteArray())
            o.flush()
            val line = readLine(s.getInputStream(), 1500) ?: throw IOException("khong co HELLO")
            if (!line.contains("\"proto\"")) throw IOException("khong phai LED Controller")
            return Found(s, host, line)
        } catch (e: Exception) {
            try {
                s.close()
            } catch (_: Exception) {
            }
            return null
        }
    }

    private fun newResolver(q: LinkedBlockingQueue<Pair<String, Int>>) = object : NsdManager.ResolveListener {
        override fun onResolveFailed(info: NsdServiceInfo, code: Int) {}
        override fun onServiceResolved(info: NsdServiceInfo) {
            val a = info.host?.hostAddress ?: return
            if (a.contains(':')) return          // thiết bị chỉ có IPv4
            q.offer(Pair(a, info.port))
        }
    }

    private fun viaNsd(timeoutMs: Long, alive: () -> Boolean): Found? {
        val nsd = ctx.getSystemService(Context.NSD_SERVICE) as? NsdManager ?: return null
        val q = LinkedBlockingQueue<Pair<String, Int>>()
        val disc = object : NsdManager.DiscoveryListener {
            override fun onStartDiscoveryFailed(type: String, code: Int) {}
            override fun onStopDiscoveryFailed(type: String, code: Int) {}
            override fun onDiscoveryStarted(type: String) {}
            override fun onDiscoveryStopped(type: String) {}
            override fun onServiceLost(info: NsdServiceInfo) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                try {
                    nsd.resolveService(info, newResolver(q))
                } catch (_: Exception) {
                }
            }
        }
        try {
            nsd.discoverServices("_ledctl._tcp", NsdManager.PROTOCOL_DNS_SD, disc)
        } catch (e: Exception) {
            return null
        }
        val deadline = System.currentTimeMillis() + timeoutMs
        try {
            while (alive() && System.currentTimeMillis() < deadline) {
                val r = q.poll(200, TimeUnit.MILLISECONDS) ?: continue
                probe(r.first, r.second, 1500)?.let { return it }
            }
        } finally {
            try {
                nsd.stopServiceDiscovery(disc)
            } catch (_: Exception) {
            }
        }
        return null
    }

    private fun scan(prefix: String, alive: () -> Boolean): Found? {
        val pool = Executors.newFixedThreadPool(48)
        val found = AtomicReference<Found?>(null)
        val latch = CountDownLatch(254)
        for (i in 1..254) {
            pool.execute {
                try {
                    if (found.get() == null && alive()) {
                        val f = probe(prefix + i, TCP_PORT, 400)
                        if (f != null && !found.compareAndSet(null, f)) f.close()
                    }
                } finally {
                    latch.countDown()
                }
            }
        }
        val deadline = System.currentTimeMillis() + 8000
        while (found.get() == null && alive() && latch.count > 0 && System.currentTimeMillis() < deadline) {
            latch.await(100, TimeUnit.MILLISECONDS)
        }
        pool.shutdownNow()
        pool.awaitTermination(2, TimeUnit.SECONDS)
        return found.get()
    }

    /** Ba số đầu của địa chỉ IPv4 trên WiFi, ví dụ "192.168.1." (giả định mạng /24). */
    @Suppress("DEPRECATION")
    private fun localPrefix(): String? {
        return try {
            val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            for (n in cm.allNetworks) {
                val caps = cm.getNetworkCapabilities(n) ?: continue
                if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue
                val lp = cm.getLinkProperties(n) ?: continue
                for (la in lp.linkAddresses) {
                    val a = la.address
                    if (a is Inet4Address && !a.isLoopbackAddress) {
                        val b = a.address
                        return "${b[0].toInt() and 255}.${b[1].toInt() and 255}.${b[2].toInt() and 255}."
                    }
                }
            }
            null
        } catch (e: Exception) {
            null
        }
    }
}
