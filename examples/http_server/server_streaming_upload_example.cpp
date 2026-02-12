#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates deferred body mode with chunk-by-chunk reading.
// Unlike the regular upload example (which buffers the entire body before
// the handler runs), this example uses an awaitable callback so the handler
// receives the request BEFORE the body is read. The handler then reads the
// body incrementally via co_await req.read(), applying TCP backpressure
// naturally without buffering the full upload in memory.

int main(int argc, char* argv[]) {
    LOG_INFO("Starting Streaming Upload Server Example");

    http::server server;

    // Serve the upload page
    server.get("/", [](http::response& res) {
        res.html(R"html(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Streaming Upload - Thinger HTTP Server</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; padding: 40px; }
        .container { max-width: 640px; margin: 0 auto; }
        h1 { color: #333; margin-bottom: 8px; }
        .subtitle { color: #666; margin-bottom: 32px; font-size: 14px; }
        .drop-zone {
            border: 2px dashed #ccc; border-radius: 12px; padding: 48px 24px;
            text-align: center; background: #fff; cursor: pointer;
            transition: border-color 0.2s, background 0.2s;
        }
        .drop-zone.dragover { border-color: #4a90d9; background: #f0f7ff; }
        .drop-zone p { color: #888; margin-bottom: 12px; }
        .drop-zone .browse { color: #4a90d9; text-decoration: underline; cursor: pointer; }
        .progress-bar { height: 6px; background: #e0e0e0; border-radius: 3px; margin-top: 24px; display: none; overflow: hidden; }
        .progress-bar .fill { height: 100%; background: #27ae60; border-radius: 3px; width: 0%; transition: width 0.1s; }
        .result {
            margin-top: 24px; background: #fff; border-radius: 12px;
            padding: 24px; display: none; border: 1px solid #e0e0e0;
        }
        .result h3 { margin-bottom: 12px; color: #333; }
        .result pre {
            background: #f8f8f8; padding: 16px; border-radius: 8px;
            font-size: 13px; overflow-x: auto; color: #444;
        }
        .result.error pre { color: #c0392b; }
        .badge { display: inline-block; background: #27ae60; color: #fff; padding: 2px 8px; border-radius: 4px; font-size: 11px; margin-left: 8px; }
    </style>
</head>
<body>
<div class="container">
    <h1>Streaming Upload <span class="badge">deferred body</span></h1>
    <p class="subtitle">Upload processed chunk-by-chunk with TCP backpressure — no full buffering</p>

    <div class="drop-zone" id="dropZone">
        <p>Drag &amp; drop a file here</p>
        <p>or <span class="browse" id="browseBtn">browse</span></p>
        <input type="file" id="fileInput" style="display:none">
    </div>

    <div class="progress-bar" id="progressBar"><div class="fill" id="progressFill"></div></div>

    <div class="result" id="result">
        <h3 id="resultTitle">Upload Result</h3>
        <pre id="resultBody"></pre>
    </div>
</div>

<script>
const dropZone = document.getElementById('dropZone');
const fileInput = document.getElementById('fileInput');
const browseBtn = document.getElementById('browseBtn');
const progressBar = document.getElementById('progressBar');
const progressFill = document.getElementById('progressFill');
const result = document.getElementById('result');
const resultTitle = document.getElementById('resultTitle');
const resultBody = document.getElementById('resultBody');

browseBtn.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => { if (fileInput.files[0]) uploadFile(fileInput.files[0]); });

dropZone.addEventListener('dragover', (e) => { e.preventDefault(); dropZone.classList.add('dragover'); });
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('dragover'));
dropZone.addEventListener('drop', (e) => {
    e.preventDefault();
    dropZone.classList.remove('dragover');
    if (e.dataTransfer.files[0]) uploadFile(e.dataTransfer.files[0]);
});

function uploadFile(file) {
    progressBar.style.display = 'block';
    progressFill.style.width = '0%';
    result.style.display = 'none';
    result.classList.remove('error');

    const xhr = new XMLHttpRequest();
    xhr.open('PUT', '/upload/' + encodeURIComponent(file.name));
    xhr.setRequestHeader('Content-Type', file.type || 'application/octet-stream');

    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) progressFill.style.width = Math.round(e.loaded / e.total * 100) + '%';
    });

    xhr.addEventListener('load', () => {
        progressFill.style.width = '100%';
        result.style.display = 'block';
        try {
            const json = JSON.parse(xhr.responseText);
            resultTitle.textContent = xhr.status === 200 ? 'Streaming Upload Result' : 'Upload Failed (' + xhr.status + ')';
            resultBody.textContent = JSON.stringify(json, null, 2);
            if (xhr.status !== 200) result.classList.add('error');
        } catch (e) {
            resultTitle.textContent = 'Response (' + xhr.status + ')';
            resultBody.textContent = xhr.responseText;
        }
    });

    xhr.addEventListener('error', () => {
        result.style.display = 'block';
        result.classList.add('error');
        resultTitle.textContent = 'Network Error';
        resultBody.textContent = 'Failed to connect to server.';
    });

    xhr.send(file);
}
</script>
</body>
</html>)html");
    });

    // PUT /upload/:filename — deferred body: reads chunk-by-chunk
    // The awaitable callback auto-enables deferred_body mode.
    // The handler receives the request BEFORE the body is read.
    server.put("/upload/:filename", [](http::request& req, http::response& res) -> awaitable<void> {
        auto cl = req.content_length();
        const auto& filename = req["filename"];

        uint8_t buffer[8192];
        size_t total = 0;
        uint8_t checksum = 0;

        while (total < cl) {
            size_t to_read = std::min(sizeof(buffer), cl - total);
            size_t bytes = co_await req.read(buffer, to_read);
            if (bytes == 0) break;

            for (size_t i = 0; i < bytes; i++) {
                checksum ^= buffer[i];
            }
            total += bytes;
        }

        // Format size for display
        auto format_size = [](size_t bytes) -> std::string {
            if (bytes < 1024) return std::to_string(bytes) + " B";
            if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + "." + std::to_string((bytes % 1024) * 10 / 1024) + " KB";
            return std::to_string(bytes / (1024 * 1024)) + "." + std::to_string((bytes % (1024 * 1024)) * 10 / (1024 * 1024)) + " MB";
        };

        res.json({
            {"filename", filename},
            {"bytes_received", total},
            {"size_formatted", format_size(total)},
            {"xor_checksum", checksum},
            {"streaming", true}
        });
    });

    // GET /status — simple non-deferred route to show mixed routing works
    server.get("/status", [](http::response& res) {
        res.json({{"status", "ok"}, {"mode", "streaming"}});
    });

    // Get port from command line or use default
    uint16_t port = 8091;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    std::cout << "Streaming Upload Server (deferred body)" << std::endl;
    std::cout << "  No max body size limit for deferred routes" << std::endl;

    if (!server.start("0.0.0.0", port, [port]() {
        std::cout << "  Listening on http://0.0.0.0:" << port << std::endl;
        std::cout << "  Open http://localhost:" << port << " in your browser" << std::endl;
        std::cout << "  Press Ctrl+C to stop" << std::endl;
    })) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }

    return 0;
}
