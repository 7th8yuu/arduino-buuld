package com.ledctl.app

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.URL
import java.net.URLDecoder
import java.util.ArrayDeque
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

enum class Phase { SEARCHING, CONNECTED, NEED_TOKEN }

/**
 * Kết nối TCP tới thiết bị theo giao thức dòng trong PROTOCOL.md: mỗi lệnh đúng một dòng trả lời,
 * dòng {"ev":...} là sự kiện. Tự tìm thiết bị, tự nối lại khi mất kết nối.
 */
class Link(private val finder: Finder, private val cfg: Config, private val listener: Listener) {

    interface Listener {
        fun onPhase(phase: Phase, detail: String)
        fun onEvent(json: String)
    }

    interface Config {
        val manualHost: String
        val savedHost: String
        val token: String
        fun saveHost(h: String)
    }

    private class Pending(val cb: ((String?) -> Unit)?, val at: Long)

    private val lock = Any()
    private val pending = ArrayDeque<Pending>()
    private val tx = Executors.newSingleThreadExecutor()
    private val http = Executors.newCachedThreadPool()

    @Volatile private var running = false
    @Volatile private var gen = 0
    @Volatile private var lastSend = 0L
    @Volatile private var kick = false
    private var sock: Socket? = null
    private var out: OutputStream? = null

    @Volatile var host: String = ""
        private set

    fun start() {
        synchronized(this) {
            if (running) return
            running = true
            val g = ++gen
            Thread({ loop(g) }, "link-$g").apply {
                isDaemon = true
                start()
            }
        }
    }

    fun stop() {
        synchronized(this) {
            running = false
            gen++
        }
        closeSock()
    }

    /** Dừng hẳn (khi Activity bị huỷ): đóng kết nối và giải phóng luồng. */
    fun shutdown() {
        stop()
        tx.shutdownNow()
        http.shutdownNow()
    }

    /** Đóng kết nối hiện tại; vòng lặp sẽ tìm và nối lại (dùng khi người dùng đổi IP / mật khẩu). */
    fun reconnect() {
        kick = true
        closeSock()
    }

    private fun alive(g: Int) = running && g == gen

    private fun sleepMs(ms: Long, g: Int) {
        val end = System.currentTimeMillis() + ms
        while (alive(g) && !kick && System.currentTimeMillis() < end) {
            try {
                Thread.sleep(100)
            } catch (_: InterruptedException) {
            }
        }
    }

    private fun loop(g: Int) {
        var wait = 500L
        var rounds = 0
        var keepMsg = false          // giữ nguyên dòng thông báo (ví dụ "Sai mật khẩu") thay vì ghi đè bằng "đang tìm"
        while (alive(g)) {
            kick = false
            if (!keepMsg) {
                listener.onPhase(Phase.SEARCHING, if (rounds >= 2) "Chưa thấy thiết bị. Kiểm tra điện thoại và thiết bị cùng một WiFi, hoặc nhập IP bên dưới." else "")
            }
            keepMsg = false
            val hosts = listOf(cfg.manualHost, cfg.savedHost).filter { it.isNotBlank() }.distinct()
            val f = try {
                finder.find(hosts, { alive(g) && !kick }, { m -> if (alive(g)) listener.onPhase(Phase.SEARCHING, m) })
            } catch (e: Exception) {
                null
            }
            if (!alive(g)) {
                f?.close()
                break
            }
            if (f == null) {
                rounds++
                sleepMs(wait, g)
                wait = minOf(wait * 2, 4000L)
                continue
            }
            wait = 500L
            rounds = 0
            keepMsg = session(g, f)
        }
    }

    /** Chạy một phiên kết nối. Trả về true nếu dừng vì cần mật khẩu (để giao diện giữ lời nhắc). */
    private fun session(g: Int, f: Finder.Found): Boolean {
        val s = f.socket
        var connected = false
        var needToken = false
        try {
            s.soTimeout = 1000
            val o = s.getOutputStream()
            val ins = s.getInputStream()
            host = f.host
            if (f.hello.contains("\"auth\":1")) {
                val tok = cfg.token
                if (tok.isBlank()) {
                    needToken = true
                    listener.onPhase(Phase.NEED_TOKEN, "Thiết bị đặt mật khẩu TCP. Nhập mật khẩu bên dưới.")
                    sleepMs(30000, g)
                    return true
                }
                o.write(("AUTH $tok\n").toByteArray())
                o.flush()
                val r = readLine(ins, 3000)
                if (r == null || !r.contains("\"ok\":1")) {
                    needToken = true
                    listener.onPhase(Phase.NEED_TOKEN, "Sai mật khẩu TCP. Nhập lại bên dưới.")
                    sleepMs(30000, g)
                    return true
                }
            }
            synchronized(lock) {
                sock = s
                out = o
                pending.clear()
            }
            connected = true
            cfg.saveHost(f.host)
            listener.onPhase(Phase.CONNECTED, f.host)
            send("SUB 1", null)          // nhận sự kiện khi nơi khác đổi trạng thái
            readerLoop(g, s, ins)
        } catch (e: Exception) {
            // mất kết nối: xuống finally
        } finally {
            // chỉ dọn phần của chính phiên này (phiên mới có thể đã bắt đầu)
            val mine = synchronized(lock) {
                if (sock === s) {
                    sock = null
                    out = null
                    val l = ArrayList(pending)
                    pending.clear()
                    l
                } else {
                    ArrayList<Pending>()
                }
            }
            try {
                s.close()
            } catch (_: Exception) {
            }
            for (p in mine) p.cb?.invoke(null)
            if (connected && alive(g)) listener.onPhase(Phase.SEARCHING, "Mất kết nối, đang nối lại…")
        }
        return needToken
    }

    private fun readerLoop(g: Int, s: Socket, ins: InputStream) {
        val buf = ByteArray(1024)
        val line = ByteArrayOutputStream()
        while (alive(g) && !s.isClosed) {
            val n = try {
                ins.read(buf)
            } catch (e: SocketTimeoutException) {
                -2
            }
            if (n == -1) return                           // thiết bị đóng kết nối
            val now = System.currentTimeMillis()
            if (n == -2) {
                synchronized(lock) {
                    val p = pending.peekFirst()
                    if (p != null && now - p.at > 6000) return   // lệnh không được trả lời: coi như đứt
                }
                if (now - lastSend > 25000) send("PING", null)
                continue
            }
            for (i in 0 until n) {
                val b = buf[i]
                if (b == '\n'.code.toByte()) {
                    handle(line.toString("UTF-8").trim())
                    line.reset()
                } else if (line.size() < 4000) {
                    line.write(b.toInt())
                }
            }
        }
    }

    private fun handle(l: String) {
        if (l.isEmpty()) return
        if (l.startsWith("{\"ev\"")) {
            listener.onEvent(l)
            return
        }
        val p = synchronized(lock) { pending.pollFirst() }
        p?.cb?.invoke(l)
    }

    private fun closeSock() {
        synchronized(lock) {
            try {
                sock?.close()
            } catch (_: Exception) {
            }
            sock = null
        }
    }

    /** Gửi một dòng lệnh; cb nhận dòng trả lời (hoặc null nếu mất kết nối / hết giờ). */
    private fun send(line: String, cb: ((String?) -> Unit)?) {
        try {
            sendNow(line, cb)
        } catch (e: RejectedExecutionException) {
            cb?.invoke(null)
        }
    }

    private fun sendNow(line: String, cb: ((String?) -> Unit)?) {
        tx.execute {
            var fail = false
            synchronized(lock) {
                val o = out
                if (o == null) {
                    fail = true
                } else {
                    pending.addLast(Pending(cb, System.currentTimeMillis()))
                    try {
                        o.write((line + "\n").toByteArray(Charsets.UTF_8))
                        o.flush()
                        lastSend = System.currentTimeMillis()
                    } catch (e: Exception) {
                        pending.removeLast()
                        fail = true
                        try {
                            sock?.close()
                        } catch (_: Exception) {
                        }
                    }
                }
            }
            if (fail) cb?.invoke(null)
        }
    }

    /** Cầu nối: giao diện web gọi fetch('/api/...'), ở đây đổi thành lệnh TCP. */
    fun api(path: String, body: String, cb: (String?) -> Unit) {
        when (path.substringBefore('?')) {
            "/api/state" -> send("STATE", cb)
            "/api/set" -> send("SET " + kv(body), cb)
            "/api/apply" -> send("APPLY " + kv(body), cb)
            "/api/test" -> send("TEST " + kv(body), cb)
            "/api/refresh" -> send("REFRESH", cb)
            // đổi WiFi / khởi động lại / ngủ sâu chỉ có ở web (xem PROTOCOL.md), gọi HTTP tới thiết bị
            "/api/restart", "/api/deepsleep", "/api/forget" -> httpPost(path.substringBefore('?'), cb)
            else -> cb(null)
        }
    }

    private fun httpPost(path: String, cb: (String?) -> Unit) {
        val h = host
        if (h.isEmpty()) {
            cb(null)
            return
        }
        try {
            http.execute { httpRun(h, path, cb) }
        } catch (e: RejectedExecutionException) {
            cb(null)
        }
    }

    private fun httpRun(h: String, path: String, cb: (String?) -> Unit) {
        val c = URL("http://$h$path").openConnection() as HttpURLConnection
        try {
            c.requestMethod = "POST"
            c.connectTimeout = 3000
            c.readTimeout = 4000
            c.doOutput = true
            try {
                c.connect()
            } catch (e: Exception) {
                cb(null)
                return
            }
            c.outputStream.use { it.write(ByteArray(0)) }
            val resp = try {
                (if (c.responseCode < 400) c.inputStream else c.errorStream)?.readBytes()?.toString(Charsets.UTF_8)
            } catch (e: Exception) {
                null          // thiết bị khởi động lại trước khi kịp trả lời: lệnh đã gửi đi
            }
            cb(if (resp.isNullOrBlank()) "{\"ok\":1}" else resp)
        } catch (e: Exception) {
            cb("{\"ok\":1}")
        } finally {
            c.disconnect()
        }
    }

    private val keyRe = Regex("^[a-z0-9_]{1,8}$")
    private val valRe = Regex("^[A-Za-z0-9._-]{0,16}$")

    /** "a=1&b=ff0000" (đã mã hoá URL) -> "a=1 b=ff0000". Bỏ khoá/giá trị lạ để không chèn được lệnh khác. */
    private fun kv(body: String): String {
        val sb = StringBuilder()
        try {
            for (part in body.split('&')) {
                val i = part.indexOf('=')
                if (i <= 0) continue
                val k = URLDecoder.decode(part.substring(0, i), "UTF-8").lowercase()
                val v = URLDecoder.decode(part.substring(i + 1), "UTF-8").removePrefix("#")
                if (keyRe.matches(k) && valRe.matches(v)) {
                    if (sb.isNotEmpty()) sb.append(' ')
                    sb.append(k).append('=').append(v)
                }
            }
        } catch (e: Exception) {
            return ""
        }
        return sb.toString()
    }
}
