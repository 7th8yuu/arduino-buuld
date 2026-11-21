package com.ledctl.app

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.SharedPreferences
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.InputMethodManager
import android.webkit.JavascriptInterface
import android.webkit.RenderProcessGoneDetail
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.io.ByteArrayInputStream

private const val HOST = "app.ledctl"          // địa chỉ giả, mọi yêu cầu đều được app tự trả lời, không ra mạng
private val BG = Color.parseColor("#101114")
private val ACCENT = Color.parseColor("#FFB02E")
private val TEXT = Color.parseColor("#F2F3F5")
private val DIM = Color.parseColor("#A8ACB5")
private val HINT = Color.parseColor("#6C717C")

class MainActivity : Activity() {

    private lateinit var prefs: SharedPreferences
    private lateinit var web: WebView
    private lateinit var overlay: View
    private lateinit var status: TextView
    private lateinit var spinner: ProgressBar
    private lateinit var form: LinearLayout
    private lateinit var ipEdit: EditText
    private lateinit var tokEdit: EditText
    private lateinit var link: Link

    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var connected = false
    private var showPosted = false
    private var reloadPosted = false
    private var page = ByteArray(0)
    private var css = ByteArray(0)
    private var cm: ConnectivityManager? = null
    private var netCb: ConnectivityManager.NetworkCallback? = null
    private var bound: Network? = null

    private val showOverlay = Runnable {
        showPosted = false
        if (!connected) overlay.visibility = View.VISIBLE
    }

    private val cfg = object : Link.Config {
        override val manualHost: String get() = prefs.getString("host", "") ?: ""
        override val savedHost: String get() = prefs.getString("last", "") ?: ""
        override val token: String get() = prefs.getString("token", "") ?: ""
        override fun saveHost(h: String) {
            prefs.edit().putString("last", h).apply()
        }
    }

    private val listener = object : Link.Listener {
        override fun onPhase(phase: Phase, detail: String) {
            ui.post { setPhase(phase, detail) }
        }

        override fun onEvent(json: String) {
            ui.post { reloadSoon(250) }
        }
    }

    inner class Bridge {
        @JavascriptInterface
        fun req(id: Int, url: String, body: String) {
            link.api(url, body) { resp -> ui.post { deliver(id, resp) } }
        }
    }

    // ---------------------------------------------------------------- vòng đời

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("ledctl", Context.MODE_PRIVATE)

        val shim = String(assets.open("shim.js").readBytes(), Charsets.UTF_8)
        var html = String(assets.open("app.html").readBytes(), Charsets.UTF_8)
        html = if (html.contains("<head>")) html.replaceFirst("<head>", "<head><script>$shim</script>")
        else "<script>$shim</script>$html"
        page = html.toByteArray(Charsets.UTF_8)
        css = assets.open("ui.css").readBytes()

        link = Link(Finder(applicationContext), cfg, listener)
        setContentView(buildUi())
        ipEdit.setText(cfg.manualHost)
        tokEdit.setText(cfg.token)
        web.loadUrl("https://$HOST/")
    }

    override fun onStart() {
        super.onStart()
        web.onResume()
        watchWifi()
        link.start()
    }

    override fun onStop() {
        link.stop()                  // trả lại chỗ kết nối TCP cho thiết bị (nó chỉ có 2 chỗ)
        unwatchWifi()
        connected = false
        web.onPause()
        super.onStop()
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        link.shutdown()
        web.destroy()
        super.onDestroy()
    }

    // ---------------------------------------------------------------- mạng

    /** Buộc app dùng WiFi cho mọi kết nối, kể cả khi WiFi đó không có Internet (ví dụ WiFi LED-Setup của thiết bị). */
    private fun watchWifi() {
        val c = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        cm = c
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                try {
                    c.bindProcessToNetwork(network)
                    bound = network
                } catch (_: Exception) {
                }
                if (!connected) link.reconnect()
            }

            override fun onLost(network: Network) {
                if (bound == network) {
                    try {
                        c.bindProcessToNetwork(null)
                    } catch (_: Exception) {
                    }
                    bound = null
                }
            }
        }
        try {
            c.registerNetworkCallback(
                NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build(), cb
            )
            netCb = cb
        } catch (_: Exception) {
        }
    }

    private fun unwatchWifi() {
        val c = cm ?: return
        try {
            netCb?.let { c.unregisterNetworkCallback(it) }
        } catch (_: Exception) {
        }
        netCb = null
        try {
            c.bindProcessToNetwork(null)
        } catch (_: Exception) {
        }
        bound = null
    }

    // ---------------------------------------------------------------- trạng thái kết nối

    private fun setPhase(p: Phase, d: String) {
        when (p) {
            Phase.CONNECTED -> {
                connected = true
                ui.removeCallbacks(showOverlay)
                showPosted = false
                overlay.visibility = View.GONE
                hideKeyboard()
                reloadSoon(0)
            }
            Phase.SEARCHING -> {
                connected = false
                status.text = d.ifEmpty { "Đang tìm thiết bị LED…" }
                spinner.visibility = View.VISIBLE
                if (d.startsWith("Chưa thấy")) form.visibility = View.VISIBLE
                // mất kết nối ngắn thì không che giao diện, chỉ che khi quá 1,5 giây
                if (overlay.visibility != View.VISIBLE && !showPosted) {
                    showPosted = true
                    ui.postDelayed(showOverlay, 1500)
                }
            }
            Phase.NEED_TOKEN -> {
                connected = false
                ui.removeCallbacks(showOverlay)
                showPosted = false
                status.text = d
                spinner.visibility = View.GONE
                form.visibility = View.VISIBLE
                overlay.visibility = View.VISIBLE
            }
        }
    }

    /** Bảo trang web tải lại trạng thái (gộp nhiều sự kiện liên tiếp thành một lần). */
    private fun reloadSoon(delayMs: Long) {
        if (reloadPosted) return
        reloadPosted = true
        ui.postDelayed({
            reloadPosted = false
            js("window.ledReload&&window.ledReload()")
        }, delayMs)
    }

    private fun js(code: String) {
        if (isDestroyed) return
        web.evaluateJavascript(code, null)
    }

    private fun deliver(id: Int, resp: String?) {
        if (resp == null) js("window.__ledResp($id,false,'')")
        else js("window.__ledResp($id,true,${JSONObject.quote(resp)})")
    }

    private fun hideKeyboard() {
        val m = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        m.hideSoftInputFromWindow(overlay.windowToken, 0)
    }

    // ---------------------------------------------------------------- giao diện

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun reply(mime: String, data: ByteArray) = WebResourceResponse(
        mime, "UTF-8", 200, "OK", mapOf("Cache-Control" to "no-store"), ByteArrayInputStream(data)
    )

    private fun notFound() = WebResourceResponse(
        "text/plain", "UTF-8", 404, "Not Found", mapOf<String, String>(), ByteArrayInputStream(ByteArray(0))
    )

    @SuppressLint("SetJavaScriptEnabled")
    private fun buildUi(): View {
        val root = FrameLayout(this)
        root.setBackgroundColor(BG)

        web = WebView(this)
        web.setBackgroundColor(BG)
        web.overScrollMode = View.OVER_SCROLL_NEVER
        web.isLongClickable = false
        web.setOnLongClickListener { true }
        web.isHapticFeedbackEnabled = false
        web.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            allowContentAccess = false
            setSupportZoom(false)
            builtInZoomControls = false
            textZoom = 100
            cacheMode = WebSettings.LOAD_NO_CACHE
        }
        web.addJavascriptInterface(Bridge(), "LedNative")
        web.webViewClient = object : WebViewClient() {
            override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? {
                val u = request.url
                if (u.host != HOST) return notFound()
                return when (u.path) {
                    "/", "/index.html" -> reply("text/html", page)
                    "/ui.css" -> reply("text/css", css)
                    else -> notFound()
                }
            }

            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean =
                request.url.host != HOST

            override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
                ui.post { recreate() }
                return true
            }
        }
        root.addView(web, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        // Lớp phủ: hiện khi chưa nối được thiết bị
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        col.gravity = Gravity.CENTER_HORIZONTAL
        col.setPadding(dp(24), dp(48), dp(24), dp(24))

        val logo = ImageView(this)
        logo.setImageResource(R.drawable.logo)
        col.addView(logo, LinearLayout.LayoutParams(dp(112), dp(112)))

        val title = TextView(this)
        title.text = "LED Controller"
        title.setTextColor(TEXT)
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
        title.setTypeface(title.typeface, Typeface.BOLD)
        title.setPadding(0, dp(16), 0, dp(8))
        col.addView(title)

        status = TextView(this)
        status.text = "Đang tìm thiết bị LED…"
        status.setTextColor(DIM)
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        status.gravity = Gravity.CENTER
        col.addView(status, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        spinner = ProgressBar(this)
        spinner.isIndeterminate = true
        spinner.indeterminateTintList = ColorStateList.valueOf(ACCENT)
        val sp = LinearLayout.LayoutParams(dp(36), dp(36))
        sp.topMargin = dp(16)
        col.addView(spinner, sp)

        val toggle = TextView(this)
        toggle.text = "Nhập IP thủ công"
        toggle.setTextColor(ACCENT)
        toggle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        toggle.setPadding(dp(12), dp(20), dp(12), dp(12))
        toggle.setOnClickListener {
            form.visibility = if (form.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }
        col.addView(toggle)

        form = LinearLayout(this)
        form.orientation = LinearLayout.VERTICAL
        form.visibility = View.GONE

        ipEdit = field("IP thiết bị, ví dụ 192.168.1.40 (để trống = tự tìm)", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI)
        tokEdit = field("Mật khẩu TCP (chỉ khi firmware có đặt)", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
        form.addView(ipEdit, fieldLp())
        form.addView(tokEdit, fieldLp())

        val btn = Button(this)
        btn.text = "Kết nối"
        btn.isAllCaps = false
        btn.setTextColor(Color.parseColor("#1A1200"))
        btn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        val bg = GradientDrawable()
        bg.setColor(ACCENT)
        bg.cornerRadius = dp(10).toFloat()
        btn.background = bg
        btn.setOnClickListener {
            prefs.edit()
                .putString("host", ipEdit.text.toString().trim())
                .putString("token", tokEdit.text.toString().trim())
                .apply()
            hideKeyboard()
            status.text = "Đang kết nối…"
            spinner.visibility = View.VISIBLE
            link.reconnect()
        }
        val bl = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48))
        bl.topMargin = dp(16)
        form.addView(btn, bl)

        col.addView(form, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        val sv = ScrollView(this)
        sv.isFillViewport = true
        sv.setBackgroundColor(BG)
        sv.addView(col, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        overlay = sv
        root.addView(sv, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        return root
    }

    private fun field(hint: String, type: Int): EditText {
        val e = EditText(this)
        e.hint = hint
        e.inputType = type
        e.setSingleLine(true)
        e.setTextColor(TEXT)
        e.setHintTextColor(HINT)
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        e.backgroundTintList = ColorStateList.valueOf(ACCENT)
        return e
    }

    private fun fieldLp(): LinearLayout.LayoutParams {
        val l = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        l.topMargin = dp(8)
        return l
    }
}
