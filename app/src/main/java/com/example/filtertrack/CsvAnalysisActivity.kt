package com.filtertrack

import android.annotation.SuppressLint
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.ViewGroup
import android.webkit.ConsoleMessage
import android.webkit.JavascriptInterface
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebSettings
import android.webkit.WebView
import android.widget.FrameLayout
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity

/** Offline WebView for inspecting a FilterTrack CSV and comparing two points. */
class CsvAnalysisActivity : AppCompatActivity() {

    private lateinit var webView: WebView
    private var pendingFileCallback: ValueCallback<Array<Uri>>? = null

    private val csvPicker = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        pendingFileCallback?.onReceiveValue(uri?.let { arrayOf(it) })
        pendingFileCallback = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.WHITE)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        webView = WebView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        root.addView(webView)
        setContentView(root)
        applyFilterTrackSystemBars(root)
        setupWebView()
        installWebViewBackHandler()
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            loadWithOverviewMode = true
            useWideViewPort = true
            cacheMode = WebSettings.LOAD_NO_CACHE
        }
        WebView.setWebContentsDebuggingEnabled(BuildConfig.DEBUG)
        webView.webChromeClient = object : WebChromeClient() {
            override fun onConsoleMessage(message: ConsoleMessage): Boolean {
                android.util.Log.d("CsvAnalysis", "${message.message()} @${message.lineNumber()}")
                return true
            }

            override fun onShowFileChooser(
                view: WebView?,
                filePathCallback: ValueCallback<Array<Uri>>?,
                fileChooserParams: FileChooserParams?,
            ): Boolean {
                pendingFileCallback?.onReceiveValue(null)
                pendingFileCallback = filePathCallback
                // Android file managers often label CSV exports as
                // application/octet-stream.  Accept every document here and
                // validate the FilterTrack columns in the offline WebView.
                csvPicker.launch("*/*")
                return true
            }
        }
        webView.addJavascriptInterface(GuideBridge(), "FilterTrackGuide")
        val guideQuery = if (intent.getBooleanExtra(MainActivity.EXTRA_INITIAL_GUIDE, false)) "?guide=1" else ""
        webView.loadUrl("file:///android_asset/csv-analysis.html$guideQuery")
    }

    private inner class GuideBridge {
        @JavascriptInterface
        fun openBiGuide() {
            runOnUiThread {
                startActivity(Intent(this@CsvAnalysisActivity, BiDashboardActivity::class.java).apply {
                    putExtra(MainActivity.EXTRA_INITIAL_GUIDE, true)
                })
            }
        }
    }

    private fun installWebViewBackHandler() {
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (webView.canGoBack()) webView.goBack() else finish()
            }
        })
    }}
