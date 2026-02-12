#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates on-demand body reading with TCP backpressure.
// It serves an HTML page where users can upload files via PUT requests.
// The server reads the body using coroutine-based socket reads, applying
// natural TCP backpressure for large uploads instead of buffering everything
// in memory at once.

int main(int argc, char* argv[]) {
    LOG_INFO("Starting File Upload Server Example");

    http::server server;

    // Configure maximum body size (16 MB for this example)
    server.set_max_body_size(16 * 1024 * 1024);

    // Serve the upload page
    server.get("/", [](http::response& res) {
        res.html(R"html(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>File Upload - Thinger HTTP Server</title>
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
        .progress-bar .fill { height: 100%; background: #4a90d9; border-radius: 3px; width: 0%; transition: width 0.1s; }
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
        .size-limit { margin-top: 16px; font-size: 12px; color: #999; text-align: center; }
    </style>
</head>
<body>
<div class="container">
    <h1>File Upload</h1>
    <p class="subtitle">Upload a file via HTTP PUT with on-demand body reading and TCP backpressure</p>

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

    <p class="size-limit">Maximum file size: 16 MB</p>
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
            resultTitle.textContent = xhr.status === 200 ? 'Upload Result' : 'Upload Failed (' + xhr.status + ')';
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

    // PUT /upload/:filename - receive file upload and return stats
    server.put("/upload/:filename", [](http::request& req, http::response& res) {
        auto http_req = req.get_http_request();
        const auto& body = http_req->get_body();
        const auto& filename = req["filename"];
        auto content_type = http_req->has_header("Content-Type")
            ? http_req->get_header("Content-Type")
            : std::string("unknown");

        // Compute a simple checksum (XOR of all bytes)
        uint8_t checksum = 0;
        for (unsigned char c : body) {
            checksum ^= c;
        }

        // Format size for display
        auto format_size = [](size_t bytes) -> std::string {
            if (bytes < 1024) return std::to_string(bytes) + " B";
            if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + "." + std::to_string((bytes % 1024) * 10 / 1024) + " KB";
            return std::to_string(bytes / (1024 * 1024)) + "." + std::to_string((bytes % (1024 * 1024)) * 10 / (1024 * 1024)) + " MB";
        };

        res.json({
            {"filename", filename},
            {"content_type", content_type},
            {"bytes_received", body.size()},
            {"size_formatted", format_size(body.size())},
            {"xor_checksum", checksum}
        });
    });

    // Get port from command line or use default
    uint16_t port = 8090;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    std::cout << "File Upload Server" << std::endl;
    std::cout << "  Max body size: 16 MB" << std::endl;

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
