// Multi-camera RTSP Stream Player — HTTP Server
// Serves static files and proxies control commands to 4× FH8862 boards
// Board IPs: .11 .12 .13 .14  — each has TCP control on port 9999

var http = require('http');
var fs = require('fs');
var path = require('path');
var net = require('net');
var url = require('url');
var iconv = require('iconv-lite');

var PORT = process.env.PORT || 8080;
var WEBROOT = __dirname;
var IMAGE_ROOT = path.join(__dirname, '..', 'image');
var BOARD_PORT = 9999;

// Device table — maps camera_id to board IP
var cameras = {
    1: { ip: '192.168.1.11', ctrlPort: 9999 },
    2: { ip: '192.168.1.12', ctrlPort: 9999 },
    3: { ip: '192.168.1.13', ctrlPort: 9999 },
    4: { ip: '192.168.1.14', ctrlPort: 9999 }
};

var MIME_TYPES = {
    '.html':  'text/html; charset=utf-8',
    '.css':   'text/css; charset=utf-8',
    '.js':    'application/javascript; charset=utf-8',
    '.json':  'application/json; charset=utf-8',
    '.png':   'image/png',
    '.jpg':   'image/jpeg',
    '.svg':   'image/svg+xml',
    '.ico':   'image/x-icon',
    '.wasm':  'application/wasm',
    '.yml':   'text/plain',
    '.yaml':  'text/plain'
};

// Send a command to a specific board's TCP control port
function sendBoardCommand(boardIp, command, callback) {
    var client = new net.Socket();
    var response = '';
    var timeout = setTimeout(function() {
        client.destroy();
        callback('timeout');
    }, 3000);

    client.connect(BOARD_PORT, boardIp, function() {
        client.write(command + '\n');
    });

    client.on('data', function(data) {
        response += data.toString();
        clearTimeout(timeout);
        client.destroy();
        callback(null, response.trim());
    });

    client.on('error', function(err) {
        clearTimeout(timeout);
        callback(err.message);
    });
}

// Resolve camera target: return board IP or error
function getCameraIP(camera_id) {
    var cam = cameras[camera_id];
    if (!cam) return null;
    return cam.ip;
}

// Parse request body
function parseBody(req, callback) {
    var body = '';
    req.on('data', function(chunk) { body += chunk; });
    req.on('end', function() { callback(body); });
}

function sendJSON(res, code, data) {
    res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify(data));
}

var server = http.createServer(function (req, res) {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        res.end();
        return;
    }

    var parsedUrl = url.parse(req.url, true);
    var pathname = parsedUrl.pathname;

    // ─── API Routes ───
    if (pathname.startsWith('/api/')) {
        if (req.method === 'POST') {
            parseBody(req, function(body) {
                var params;
                try { params = JSON.parse(body); } catch(e) { params = {}; }

                var cameraId = parseInt(params.camera_id) || 0;
                var boardIp = getCameraIP(cameraId);
                var cmd = '';

                // ─── Camera List (no board needed) ───
                if (pathname === '/api/cameras') {
                    var list = [];
                    for (var id in cameras) {
                        list.push({ id: parseInt(id), ip: cameras[id].ip });
                    }
                    sendJSON(res, 200, { cameras: list });
                    return;
                }

                // ─── Time Sync — broadcast to ALL boards ───
                if (pathname === '/api/time/sync') {
                    var timestamp = params.timestamp || Date.now();
                    cmd = 'TIME_SYNC ' + timestamp;
                    var pending = Object.keys(cameras).length;
                    var results = [];

                    Object.keys(cameras).forEach(function(id) {
                        var cam = cameras[id];
                        sendBoardCommand(cam.ip, cmd, function(err, resp) {
                            results.push({
                                camera_id: parseInt(id),
                                ok: !err,
                                response: err || resp
                            });
                            if (--pending === 0) {
                                sendJSON(res, 200, { ok: true, results: results });
                            }
                        });
                    });
                    return;
                }

                // ─── All other APIs require a valid camera_id ───
                if (!boardIp) {
                    sendJSON(res, 400, { error: 'Missing or invalid camera_id (1-4)' });
                    return;
                }

                // Build command string based on API path
                if (pathname === '/api/rotate') {
                    cmd = 'ROTATE ' + (params.rotate ? 1 : 0);
                } else if (pathname === '/api/osd/color') {
                    var r = (params.r != null) ? params.r : 255;
                    var g = (params.g != null) ? params.g : 255;
                    var b = (params.b != null) ? params.b : 255;
                    cmd = 'OSD_COLOR ' + r + ' ' + g + ' ' + b;
                } else if (pathname === '/api/osd/text') {
                    var text = (params.text || '');
                    var gbBuf = iconv.encode(text, 'gb2312');
                    var hexStr = '';
                    for (var i = 0; i < gbBuf.length; i++) {
                        var h = gbBuf[i].toString(16);
                        if (h.length === 1) h = '0' + h;
                        hexStr += h;
                    }
                    if (hexStr) {
                        cmd = 'OSD_TEXT_HEX ' + hexStr;
                    } else {
                        cmd = 'OSD_TEXT ' + text;
                    }
                } else if (pathname === '/api/osd/text_enable') {
                    cmd = 'OSD_TEXT_EN ' + (params.enable ? 1 : 0);
                } else if (pathname === '/api/osd/invert') {
                    cmd = 'OSD_INVERT ' + (params.invert || 0);
                } else if (pathname === '/api/mask/color') {
                    var mr = (params.r != null) ? params.r : 0;
                    var mg = (params.g != null) ? params.g : 255;
                    var mb = (params.b != null) ? params.b : 0;
                    cmd = 'MASK_COLOR ' + mr + ' ' + mg + ' ' + mb;
                } else if (pathname === '/api/mask/enable') {
                    cmd = 'MASK_ENABLE ' + (params.enable ? 1 : 0);
                } else if (pathname === '/api/mask/region/set') {
                    var idx = (params.index != null) ? parseInt(params.index) : 0;
                    var en  = (params.enable != null) ? (params.enable ? 1 : 0) : 0;
                    var x   = (params.x != null) ? parseInt(params.x) : 0;
                    var y   = (params.y != null) ? parseInt(params.y) : 0;
                    var w   = (params.w != null) ? parseInt(params.w) : 100;
                    var h   = (params.h != null) ? parseInt(params.h) : 100;
                    cmd = 'MASK_REGION_SET ' + idx + ' ' + en + ' ' + x + ' ' + y + ' ' + w + ' ' + h;
                } else if (pathname === '/api/mask/region/del') {
                    var didx = (params.index != null) ? parseInt(params.index) : 0;
                    cmd = 'MASK_REGION_DEL ' + didx;
                } else if (pathname === '/api/mask/type') {
                    var type = (params.type != null) ? (params.type ? 1 : 0) : 0;
                    cmd = 'MASK_TYPE ' + type;
                } else if (pathname === '/api/mask/mosaic_size') {
                    var size = (params.size != null) ? parseInt(params.size) : 1;
                    cmd = 'MASK_MOSAIC_SIZE ' + size;
                } else if (pathname === '/api/record/start') {
                    cmd = 'RECORD_START ' + (params.duration || 60);
                } else if (pathname === '/api/record/stop') {
                    cmd = 'RECORD_STOP';
                } else if (pathname === '/api/led/record/start') {
                    cmd = 'LED_RECORD_START';
                } else if (pathname === '/api/led/record/stop') {
                    cmd = 'LED_RECORD_STOP';
                } else if (pathname === '/api/led') {
                    cmd = 'LED ' + (params.mode || 0);
                } else {
                    sendJSON(res, 404, { error: 'Unknown API' });
                    return;
                }

                console.log('[API] cam=' + cameraId + ' ip=' + boardIp + ' -> ' + cmd);
                sendBoardCommand(boardIp, cmd, function(err, response) {
                    if (err) {
                        sendJSON(res, 500, { error: 'Board ' + cameraId + ' failed: ' + err });
                    } else {
                        sendJSON(res, 200, { ok: true, camera_id: cameraId, response: response });
                    }
                });
            });
            return;
        } else if (req.method === 'GET') {
            if (pathname === '/api/cameras') {
                var list = [];
                for (var id in cameras) {
                    list.push({ id: parseInt(id), ip: cameras[id].ip });
                }
                sendJSON(res, 200, { cameras: list });
                return;
            }
            if (pathname === '/api/time/sync' || pathname === '/api/time') {
                sendJSON(res, 200, { timestamp: Date.now() });
                return;
            }
        }
    }

    // ─── Static Files ───
    var filePath;
    var staticRoot = WEBROOT;
    if (pathname.indexOf('/image/') === 0) {
        staticRoot = IMAGE_ROOT;
        try {
            filePath = path.join(IMAGE_ROOT, decodeURIComponent(pathname.slice('/image/'.length)));
        } catch (e) {
            res.writeHead(400);
            res.end('Bad Request');
            return;
        }
    } else {
        filePath = pathname === '/' ? '/index.html' : pathname;
        filePath = path.join(WEBROOT, filePath);
    }
    filePath = path.normalize(filePath);

    if (!filePath.startsWith(staticRoot)) {
        res.writeHead(403);
        res.end('Forbidden');
        return;
    }

    var ext = path.extname(filePath).toLowerCase();
    var contentType = MIME_TYPES[ext] || 'application/octet-stream';

    fs.readFile(filePath, function(err, data) {
        if (err) {
            if (err.code === 'ENOENT') {
                res.writeHead(404);
                res.end('Not Found');
            } else {
                res.writeHead(500);
                res.end('Internal Server Error');
            }
            return;
        }
        res.writeHead(200, { 'Content-Type': contentType });
        res.end(data);
    });
});

server.listen(PORT, function () {
    console.log('Multi-camera RTSP Player server running at:');
    console.log('  http://192.168.1.1:' + PORT);
    console.log('');
    console.log('Cameras:');
    for (var id in cameras) {
        console.log('  Camera ' + id + ': ' + cameras[id].ip + ' (RTSP:8554, CTRL:9999)');
    }
    console.log('');
    console.log('Control API endpoints (all require camera_id in POST body):');
    console.log('  POST /api/rotate        - 旋转180度');
    console.log('  POST /api/osd/color     - OSD颜色');
    console.log('  POST /api/osd/text      - OSD文字');
    console.log('  POST /api/osd/text_enable - OSD文字开关');
    console.log('  POST /api/osd/invert    - OSD反色');
    console.log('  POST /api/mask/color    - Mask颜色');
    console.log('  POST /api/mask/enable   - Mask开关');
    console.log('  POST /api/mask/region/set - Mask区域设置');
    console.log('  POST /api/mask/region/del - Mask区域删除');
    console.log('  POST /api/mask/type     - Mask类型');
    console.log('  POST /api/mask/mosaic_size - 马赛克大小');
    console.log('  POST /api/record/start  - 开始录像');
    console.log('  POST /api/record/stop   - 停止录像');
    console.log('  POST /api/led/record/start - 录像LED闪烁开始');
    console.log('  POST /api/led/record/stop  - 录像LED闪烁结束');
    console.log('  POST /api/led           - LED控制');
    console.log('  POST /api/time/sync     - 时间同步(全部)');
    console.log('  GET  /api/cameras       - 摄像头列表');
    console.log('  GET  /api/time          - 获取时间戳');
});
