// Multi-camera WebRTC WHEP Player + Board Control API
// 4× FH8862 boards → Mediamtx → WebRTC → Browser
// Each camera independently: stream toggle, OSD, mask, record, LED

(function () {
    'use strict';

    var mediamtxPort = 8889;
    var config = {
        iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
    };

    // Camera definitions
    var cameras = [
        { id: 1, name: 'Camera 1', ip: '192.168.1.11' },
        { id: 2, name: 'Camera 2', ip: '192.168.1.12' },
        { id: 3, name: 'Camera 3', ip: '192.168.1.13' },
        { id: 4, name: 'Camera 4', ip: '192.168.1.14' }
    ];

    // State per camera: { stream: 'main'|'sub', connected: bool }
    var camState = {};
    var peerConnections = {};   // key: 'cam1_main', 'cam1_sub', ...
    var selectedCam = 1;        // which camera the control panel targets

    // Per-camera control state (decoupled across tabs)
    var camControls = {};

    // Recording
    var deviceRecordTimers = {};   // per camera
    var pcRecordTimer = null;
    var pcRecorder = null;
    var pcRecordedChunks = [];
    var pcRecording = false;
    var pcRecordCameraId = null;

    var els = {};

    function $(id) {
        return document.getElementById(id);
    }

    function initDOMElements() {
        els.localIp = $('localIp');
        els.globalStatus = $('globalStatus');
        els.connectionInfo = $('connectionInfo');
        els.videoWall = $('videoWall');
        els.camTabs = $('camTabs');
        els.controlPanel = $('controlPanel');
        els.ctrlTitle = $('ctrlTitle');
        els.rotateToggle = $('rotateToggle');
        els.osdColorR = $('osdColorR');
        els.osdColorG = $('osdColorG');
        els.osdColorB = $('osdColorB');
        els.osdColorPreview = $('osdColorPreview');
        els.osdTextInput = $('osdTextInput');
        els.osdTextEnable = $('osdTextEnable');
        els.osdInvert = $('osdInvert');
        els.maskColorR = $('maskColorR');
        els.maskColorG = $('maskColorG');
        els.maskColorB = $('maskColorB');
        els.maskColorPreview = $('maskColorPreview');
        els.maskEnable = $('maskEnable');
        els.maskMosaicMode = $('maskMosaicMode');
        els.maskMosaicSize = $('maskMosaicSize');
        els.maskRegionList = $('maskRegionList');
        els.recordDuration = $('recordDuration');
        els.btnRecordStart = $('btnRecordStart');
        els.btnRecordStop = $('btnRecordStop');
        els.recordStatus = $('recordStatus');
        els.btnPcRecordStart = $('btnPcRecordStart');
        els.btnPcRecordStop = $('btnPcRecordStop');
        els.pcRecordStatus = $('pcRecordStatus');
        els.pcRecordDuration = $('pcRecordDuration');
        els.timeSyncResult = $('timeSyncResult');
        els.themeLight = $('themeLight');
        els.themeDark = $('themeDark');
    }

    function applyThemeMode(mode) {
        var theme = (mode === 'light') ? 'light' : 'dark';
        document.body.setAttribute('data-theme', theme);
        if (els.themeLight) els.themeLight.className = theme === 'light' ? 'active' : '';
        if (els.themeDark) els.themeDark.className = theme === 'dark' ? 'active' : '';
    }

    window.setThemeMode = applyThemeMode;

    // ─── API helpers ───

    function apiCall(endpoint, data, callback) {
        var xhr = new XMLHttpRequest();
        xhr.open('POST', endpoint, true);
        xhr.setRequestHeader('Content-Type', 'application/json');
        xhr.onload = function() {
            if (callback) callback(null, JSON.parse(xhr.responseText));
        };
        xhr.onerror = function() {
            if (callback) callback(xhr.statusText);
        };
        xhr.send(JSON.stringify(data));
    }

    // Send a control command to the currently selected camera
    function boardApiForCamera(endpoint, data, cameraId) {
        data = data || {};
        data.camera_id = cameraId;
        apiCall(endpoint, data);
    }

    function boardApi(endpoint, data) {
        boardApiForCamera(endpoint, data, selectedCam);
    }

    // ─── Build Video Wall ───

    function buildVideoWall() {
        if (!els.videoWall) return;

        cameras.forEach(function(cam) {
            camState[cam.id] = { stream: 'main', connected: false };

            var panel = document.createElement('div');
            panel.className = 'cam-panel';
            panel.id = 'panel-cam' + cam.id;
            panel.setAttribute('data-cam', cam.id);
            panel.innerHTML =
                '<div class="cam-header">' +
                    '<h3>' + cam.name + '</h3>' +
                    '<span class="cam-ip">' + cam.ip + '</span>' +
                    '<span class="cam-status" id="status-cam' + cam.id + '">● 等待</span>' +
                '</div>' +
                '<div class="cam-toolbar">' +
                    '<div class="stream-toggle">' +
                        '<button id="btnMain-cam' + cam.id + '" class="toggle-btn active" onclick="switchStream(' + cam.id + ',\'main\')">主码流</button>' +
                        '<button id="btnSub-cam' + cam.id + '" class="toggle-btn" onclick="switchStream(' + cam.id + ',\'sub\')">子码流</button>' +
                    '</div>' +
                '</div>' +
                '<div class="video-wrapper">' +
                    '<video id="video-cam' + cam.id + '" autoplay playsinline muted></video>' +
                    '<div class="video-placeholder" id="placeholder-cam' + cam.id + '">' +
                        '<span>等待视频流...</span>' +
                    '</div>' +
                '</div>';
            els.videoWall.appendChild(panel);
        });
    }

    // ─── Camera Selector Tabs ───

    function buildCameraTabs() {
        if (!els.camTabs) return;

        cameras.forEach(function(cam) {
            var tab = document.createElement('button');
            tab.className = 'cam-tab' + (cam.id === selectedCam ? ' active' : '');
            tab.id = 'tab-cam' + cam.id;
            tab.textContent = cam.name;
            tab.onclick = function() { selectCamera(cam.id); };
            els.camTabs.appendChild(tab);
        });
    }

    function saveControlState(camId) {
        var s = camControls[camId] || (camControls[camId] = {});
        if (!els.rotateToggle) return;

        s.rotate       = els.rotateToggle.checked;
        s.osdR         = els.osdColorR.value;
        s.osdG         = els.osdColorG.value;
        s.osdB         = els.osdColorB.value;
        s.osdText      = els.osdTextInput.value;
        s.osdTextEn    = els.osdTextEnable.checked;
        s.osdInvert    = els.osdInvert.value;
        s.maskR        = els.maskColorR.value;
        s.maskG        = els.maskColorG.value;
        s.maskB        = els.maskColorB.value;
        s.maskEn       = els.maskEnable.checked;
        s.maskMosaic   = els.maskMosaicMode.checked;
        s.maskMosaicSz = els.maskMosaicSize.value;
        s.maskMosaicVis = els.maskMosaicSize.style.display;

        // Save mask regions
        s.maskRegions = [];
        var rows = els.maskRegionList ? els.maskRegionList.querySelectorAll('.mask-region-row') : [];
        for (var i = 0; i < rows.length; i++) {
            var row = rows[i];
            var idx = parseInt(row.getAttribute('data-index'));
            var en = row.querySelector('.region-enable') ? row.querySelector('.region-enable').checked : false;
            var x = parseInt(row.querySelector('.region-x').value) || 0;
            var y = parseInt(row.querySelector('.region-y').value) || 0;
            var w = parseInt(row.querySelector('.region-w').value) || 100;
            var h = parseInt(row.querySelector('.region-h').value) || 100;
            s.maskRegions.push({ idx: idx, en: en, x: x, y: y, w: w, h: h });
        }

        s.recDuration  = els.recordDuration ? els.recordDuration.value : '60';
        s.recStartDisabled = els.btnRecordStart ? els.btnRecordStart.disabled : false;
        s.recStopDisabled  = els.btnRecordStop ? els.btnRecordStop.disabled : false;
        s.recStatusText    = els.recordStatus ? els.recordStatus.textContent : '';
        s.recStatusCls     = els.recordStatus ? els.recordStatus.className : '';
    }

    function restoreControlState(camId) {
        var s = camControls[camId];
        if (!els.rotateToggle) return;

        // When no saved state, apply defaults (for first-time tab switch)
        var def = {
            rotate: false, osdR: '255', osdG: '255', osdB: '255',
            osdText: '影流队', osdTextEn: true, osdInvert: '0',
            maskR: '0', maskG: '255', maskB: '0', maskEn: true,
            maskMosaic: false, maskMosaicSz: '1', maskMosaicVis: 'none',
            maskRegions: [
                { idx: 0, en: true, x: 360, y: 360, w: 900, h: 480 },
                { idx: 1, en: true, x: 2250, y: 1140, w: 900, h: 480 }
            ],
            recDuration: '60', recStartDisabled: false, recStopDisabled: true,
            recStatusText: '未录像', recStatusCls: 'record-status'
        };
        if (!s) { s = def; }

        els.rotateToggle.checked = s.rotate || false;
        els.osdColorR.value = s.osdR || '255';
        els.osdColorG.value = s.osdG || '255';
        els.osdColorB.value = s.osdB || '255';
        if (els.osdColorPreview) {
            els.osdColorPreview.style.backgroundColor =
                'rgb(' + (s.osdR || 255) + ',' + (s.osdG || 255) + ',' + (s.osdB || 255) + ')';
        }
        els.osdTextInput.value = s.osdText || '影流队';
        els.osdTextEnable.checked = (s.osdTextEn !== undefined) ? s.osdTextEn : true;
        els.osdInvert.value = s.osdInvert || '0';
        els.maskColorR.value = s.maskR || '0';
        els.maskColorG.value = s.maskG || '255';
        els.maskColorB.value = s.maskB || '0';
        if (els.maskColorPreview) {
            els.maskColorPreview.style.backgroundColor =
                'rgb(' + (s.maskR || 0) + ',' + (s.maskG || 255) + ',' + (s.maskB || 0) + ')';
        }
        els.maskEnable.checked = (s.maskEn !== undefined) ? s.maskEn : true;
        els.maskMosaicMode.checked = s.maskMosaic || false;
        els.maskMosaicSize.value = s.maskMosaicSz || '1';
        els.maskMosaicSize.style.display = s.maskMosaicVis || 'none';
        els.recordDuration.value = s.recDuration || '60';
        if (els.btnRecordStart) els.btnRecordStart.disabled = s.recStartDisabled || false;
        if (els.btnRecordStop) els.btnRecordStop.disabled = s.recStopDisabled || false;
        if (els.recordStatus) {
            els.recordStatus.textContent = s.recStatusText || '未录像';
            els.recordStatus.className = s.recStatusCls || 'record-status';
        }

        // Restore mask regions
        clearMaskRegions();
        if (s.maskRegions) {
            for (var i = 0; i < s.maskRegions.length; i++) {
                var r = s.maskRegions[i];
                addRegionToUI(r.idx, r.en, r.x, r.y, r.w, r.h);
            }
        }
    }

    window.selectCamera = function(camId) {
        // Save current camera's control state before switching
        saveControlState(selectedCam);

        selectedCam = camId;

        // Update tab styles
        cameras.forEach(function(cam) {
            var tab = $('tab-cam' + cam.id);
            if (tab) {
                tab.className = 'cam-tab' + (cam.id === selectedCam ? ' active' : '');
            }
        });

        // Update control panel title
        if (els.ctrlTitle) {
            var cam = cameras.find(function(c) { return c.id === camId; });
            els.ctrlTitle.textContent = (cam ? cam.name : 'Camera ' + camId) + ' 控制';
        }

        // Restore target camera's control state
        restoreControlState(camId);
    };

    // ─── WebRTC Connection ───

    function getStreamPath(camId, streamType) {
        return 'cam' + camId + '_' + streamType;
    }

    function connectStream(camId, streamType) {
        var path = getStreamPath(camId, streamType);
        var localIp = els.localIp.value || window.location.hostname;
        var whepUrl = 'http://' + localIp + ':' + mediamtxPort + '/' + path + '/whep';
        var videoEl = $('video-cam' + camId);
        var placeholderEl = $('placeholder-cam' + camId);
        var statusEl = $('status-cam' + camId);

        if (!videoEl) return;

        setStatus(statusEl, '● 连接中...', 'pending');

        // Close old connection for this camera+type
        if (peerConnections[path]) {
            peerConnections[path].pc.close();
            delete peerConnections[path];
        }

        var pc = new RTCPeerConnection(config);
        pc.addTransceiver('video', { direction: 'recvonly' });
        pc.addTransceiver('audio', { direction: 'recvonly' });

        var connData = { pc: pc, connected: false };
        peerConnections[path] = connData;

        pc.oniceconnectionstatechange = function() {
            var state = pc.iceConnectionState;
            if (state === 'connected' || state === 'completed') {
                connData.connected = true;
                camState[camId].connected = true;
                camState[camId].stream = streamType;
                setStatus(statusEl, '● 已连接', 'connected');
                updateGlobalStatus();
            } else if (state === 'failed' || state === 'disconnected') {
                connData.connected = false;
                camState[camId].connected = false;
                setStatus(statusEl, '● 断开', 'error');
                updateGlobalStatus();
                setTimeout(function() {
                    if (peerConnections[path] === connData && !connData.connected) {
                        connectStream(camId, streamType);
                    }
                }, 3000);
            } else if (state === 'closed') {
                connData.connected = false;
                camState[camId].connected = false;
                updateGlobalStatus();
            }
        };

        pc.ontrack = function(event) {
            if (event.streams && event.streams[0]) {
                videoEl.srcObject = event.streams[0];
            }
            videoEl.classList.add('active');
            if (placeholderEl) placeholderEl.classList.add('hidden');
            setStatus(statusEl, '● 播放中', 'connected');
        };

        pc.createOffer()
            .then(function(offer) { return pc.setLocalDescription(offer); })
            .then(function() { return waitForIceGathering(pc); })
            .then(function() {
                return fetch(whepUrl, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/sdp' },
                    body: pc.localDescription.sdp
                });
            })
            .then(function(response) {
                if (!response.ok) throw new Error('WHEP failed: ' + response.status);
                return response.text();
            })
            .then(function(answerSdp) {
                return pc.setRemoteDescription({ type: 'answer', sdp: answerSdp });
            })
            .catch(function(err) {
                console.error('[' + path + '] Error:', err);
                connData.connected = false;
                camState[camId].connected = false;
                setStatus(statusEl, '● 连接失败', 'error');
                updateGlobalStatus();
            });
    }

    function disconnectStream(camId, streamType) {
        var path = getStreamPath(camId, streamType);
        if (peerConnections[path]) {
            peerConnections[path].pc.close();
            delete peerConnections[path];
        }
        var videoEl = $('video-cam' + camId);
        if (videoEl) {
            videoEl.srcObject = null;
            videoEl.classList.remove('active');
        }
        var placeholderEl = $('placeholder-cam' + camId);
        if (placeholderEl) placeholderEl.classList.remove('hidden');
        var statusEl = $('status-cam' + camId);
        setStatus(statusEl, '● 已断开', '');
        camState[camId].connected = false;
        updateGlobalStatus();
    }

    window.switchStream = function(camId, streamType) {
        if (!camState[camId]) return;
        if (camState[camId].stream === streamType && camState[camId].connected) return;

        // Disconnect the other stream type for this camera
        var otherType = (streamType === 'main') ? 'sub' : 'main';
        disconnectStream(camId, otherType);

        // Update toggle button styles
        var btnMain = $('btnMain-cam' + camId);
        var btnSub = $('btnSub-cam' + camId);
        if (btnMain && btnSub) {
            if (streamType === 'main') {
                btnMain.classList.add('active');
                btnSub.classList.remove('active');
            } else {
                btnSub.classList.add('active');
                btnMain.classList.remove('active');
            }
        }

        // Connect new stream
        connectStream(camId, streamType);
    };

    function waitForIceGathering(pc) {
        return new Promise(function(resolve) {
            if (pc.iceGatheringState === 'complete') { resolve(); return; }
            var timeout = setTimeout(function() { resolve(); }, 2000);
            pc.addEventListener('icegatheringstatechange', function handler() {
                if (pc.iceGatheringState === 'complete') {
                    clearTimeout(timeout);
                    pc.removeEventListener('icegatheringstatechange', handler);
                    resolve();
                }
            });
        });
    }

    // ─── Global Connect / Disconnect ───

    window.connectAll = function() {
        detectLocalIP();
        // Connect all cameras, default to main stream
        cameras.forEach(function(cam) {
            if (!camState[cam.id]) camState[cam.id] = { stream: 'main', connected: false };
            var streamType = camState[cam.id].stream || 'main';
            connectStream(cam.id, streamType);
            // Update toggle buttons
            var btnMain = $('btnMain-cam' + cam.id);
            var btnSub = $('btnSub-cam' + cam.id);
            if (btnMain && btnSub) {
                if (streamType === 'main') {
                    btnMain.classList.add('active');
                    btnSub.classList.remove('active');
                } else {
                    btnSub.classList.add('active');
                    btnMain.classList.remove('active');
                }
            }
        });
    };

    window.disconnectAll = function() {
        cameras.forEach(function(cam) {
            disconnectStream(cam.id, 'main');
            disconnectStream(cam.id, 'sub');
        });
    };

    // ─── Status Helpers ───

    function setStatus(el, text, cls) {
        if (!el) return;
        el.textContent = text;
        el.className = 'cam-status ' + (cls || '');
    }

    function updateGlobalStatus() {
        if (!els.globalStatus || !els.connectionInfo) return;
        var total = cameras.length;
        var connected = 0;
        cameras.forEach(function(cam) {
            if (camState[cam.id] && camState[cam.id].connected) connected++;
        });

        if (connected === total) {
            setStatus(els.globalStatus, '全部已连接 (' + connected + '/' + total + ')', 'connected');
            els.connectionInfo.textContent = 'WebRTC: ' + connected + '/' + total + ' 路在线';
        } else if (connected > 0) {
            setStatus(els.globalStatus, '部分连接 (' + connected + '/' + total + ')', 'connected');
            els.connectionInfo.textContent = 'WebRTC: ' + connected + '/' + total + ' 路在线';
        } else {
            setStatus(els.globalStatus, '未连接', '');
            els.connectionInfo.textContent = 'WebRTC: 未连接';
        }
    }

    function detectLocalIP() {
        var host = window.location.hostname;
        if (els.localIp) els.localIp.value = host;
        return host;
    }

    // ─── Control Functions (send to selectedCam) ───

    window.ctrlRotate = function() {
        var on = els.rotateToggle ? els.rotateToggle.checked : false;
        boardApi('/api/rotate', { rotate: on });
    };

    window.ctrlOsdColor = function() {
        var r = parseInt(els.osdColorR.value);
        var g = parseInt(els.osdColorG.value);
        var b = parseInt(els.osdColorB.value);
        if (els.osdColorPreview) {
            els.osdColorPreview.style.backgroundColor = 'rgb(' + r + ',' + g + ',' + b + ')';
        }
        boardApi('/api/osd/color', { r: r, g: g, b: b });
    };

    window.ctrlOsdText = function() {
        var text = els.osdTextInput.value.trim();
        if (!text) return;
        boardApi('/api/osd/text', { text: text });
    };

    window.ctrlOsdTextEnable = function() {
        boardApi('/api/osd/text_enable', { enable: els.osdTextEnable.checked });
    };

    window.ctrlOsdInvert = function() {
        boardApi('/api/osd/invert', { invert: parseInt(els.osdInvert.value) });
    };

    window.ctrlMaskColor = function() {
        var r = parseInt(els.maskColorR.value);
        var g = parseInt(els.maskColorG.value);
        var b = parseInt(els.maskColorB.value);
        if (els.maskColorPreview) {
            els.maskColorPreview.style.backgroundColor = 'rgb(' + r + ',' + g + ',' + b + ')';
        }
        boardApi('/api/mask/color', { r: r, g: g, b: b });
    };

    window.ctrlMaskEnable = function() {
        boardApi('/api/mask/enable', { enable: els.maskEnable.checked });
    };

    window.ctrlMaskType = function() {
        var type = els.maskMosaicMode.checked ? 1 : 0;
        boardApi('/api/mask/type', { type: type });
        if (els.maskMosaicSize) {
            els.maskMosaicSize.style.display = type ? 'inline-block' : 'none';
        }
    };

    window.ctrlMaskMosaicSize = function() {
        boardApi('/api/mask/mosaic_size', { size: parseInt(els.maskMosaicSize.value) });
    };

    function findFirstFreeMaskSlot() {
        var rows = els.maskRegionList ? els.maskRegionList.querySelectorAll('.mask-region-row') : [];
        var used = {};
        for (var i = 0; i < rows.length; i++) {
            used[parseInt(rows[i].getAttribute('data-index'))] = true;
        }
        for (var i = 0; i < 8; i++) {
            if (!used[i]) return i;
        }
        return -1;
    }

    function sendMaskRegion(idx, enable, x, y, w, h) {
        boardApi('/api/mask/region/set', { index: idx, enable: enable, x: x, y: y, w: w, h: h });
    }

    window.deleteMaskRegion = function(idx) {
        boardApi('/api/mask/region/del', { index: idx });
        var row = els.maskRegionList.querySelector('.mask-region-row[data-index="' + idx + '"]');
        if (row) row.remove();
    };

    window.ctrlMaskAddRegion = function() {
        var idx = findFirstFreeMaskSlot();
        if (idx < 0) { alert('所有8个区域已使用'); return; }
        sendMaskRegion(idx, 1, 100, 100, 200, 200);
        addRegionToUI(idx, true, 100, 100, 200, 200);
    };

    function addRegionToUI(idx, enable, x, y, w, h) {
        var existing = els.maskRegionList.querySelector('.mask-region-row[data-index="' + idx + '"]');
        if (existing) existing.remove();

        var row = document.createElement('div');
        row.className = 'mask-region-row';
        row.setAttribute('data-index', idx);
        row.innerHTML =
            '<span class="region-index">#' + idx + '</span>' +
            '<label><input type="checkbox" class="region-enable" ' + (enable ? 'checked' : '') +
            ' onchange="onRegionEnableChange(' + idx + ', this.checked)"> 启用</label>' +
            'X:<input type="number" class="region-x" value="' + x +
            '" onchange="onRegionChange(' + idx + ')">' +
            'Y:<input type="number" class="region-y" value="' + y +
            '" onchange="onRegionChange(' + idx + ')">' +
            'W:<input type="number" class="region-w" value="' + w +
            '" onchange="onRegionChange(' + idx + ')">' +
            'H:<input type="number" class="region-h" value="' + h +
            '" onchange="onRegionChange(' + idx + ')">' +
            '<button class="btn-del" onclick="deleteMaskRegion(' + idx + ')">删除</button>';
        els.maskRegionList.appendChild(row);
    }

    window.onRegionChange = function(idx) {
        var row = els.maskRegionList.querySelector('.mask-region-row[data-index="' + idx + '"]');
        if (!row) return;
        var en = row.querySelector('.region-enable').checked ? 1 : 0;
        var x = parseInt(row.querySelector('.region-x').value) || 0;
        var y = parseInt(row.querySelector('.region-y').value) || 0;
        var w = parseInt(row.querySelector('.region-w').value) || 100;
        var h = parseInt(row.querySelector('.region-h').value) || 100;
        sendMaskRegion(idx, en, x, y, w, h);
    };

    window.onRegionEnableChange = function(idx, checked) {
        var row = els.maskRegionList.querySelector('.mask-region-row[data-index="' + idx + '"]');
        if (!row) return;
        var x = parseInt(row.querySelector('.region-x').value) || 0;
        var y = parseInt(row.querySelector('.region-y').value) || 0;
        var w = parseInt(row.querySelector('.region-w').value) || 100;
        var h = parseInt(row.querySelector('.region-h').value) || 100;
        sendMaskRegion(idx, checked ? 1 : 0, x, y, w, h);
    };

    function clearMaskRegions() {
        if (els.maskRegionList) els.maskRegionList.innerHTML = '';
    }

    function initDefaultMaskRegions() {
        addRegionToUI(0, true, 360, 360, 900, 480);
        addRegionToUI(1, true, 2250, 1140, 900, 480);
    }

    window.ctrlRecordStart = function() {
        var camId = selectedCam;
        var duration = parseInt(els.recordDuration.value) || 60;
        boardApiForCamera('/api/record/start', { duration: duration }, camId);
        els.btnRecordStart.disabled = true;
        els.btnRecordStop.disabled = false;
        els.recordStatus.textContent = '录像中... (' + duration + 's)';
        els.recordStatus.className = 'record-status recording';
        camControls[camId] = camControls[camId] || {};
        camControls[camId].recDuration = String(duration);
        camControls[camId].recStartDisabled = true;
        camControls[camId].recStopDisabled = false;
        camControls[camId].recStatusText = els.recordStatus.textContent;
        camControls[camId].recStatusCls = els.recordStatus.className;
        if (deviceRecordTimers[camId]) clearTimeout(deviceRecordTimers[camId]);
        deviceRecordTimers[camId] = setTimeout(function() { ctrlRecordStop(camId); }, duration * 1000);
    };

    window.ctrlRecordStop = function(cameraId) {
        var camId = cameraId || selectedCam;
        boardApiForCamera('/api/record/stop', {}, camId);
        camControls[camId] = camControls[camId] || {};
        camControls[camId].recStartDisabled = false;
        camControls[camId].recStopDisabled = true;
        camControls[camId].recStatusText = '已停止';
        camControls[camId].recStatusCls = 'record-status';
        if (camId === selectedCam) {
            els.btnRecordStart.disabled = false;
            els.btnRecordStop.disabled = true;
            els.recordStatus.textContent = '已停止';
            els.recordStatus.className = 'record-status';
        }
        if (deviceRecordTimers[camId]) {
            clearTimeout(deviceRecordTimers[camId]);
            deviceRecordTimers[camId] = null;
        }
    };

    // PC-side recording (records first available camera's video)
    window.ctrlPcRecordStart = function() {
        var stream = null;
        var recordCameraId = null;
        for (var i = 0; i < cameras.length; i++) {
            var v = $('video-cam' + cameras[i].id);
            if (v && v.srcObject) {
                stream = v.srcObject;
                recordCameraId = cameras[i].id;
                break;
            }
        }
        if (!stream) { alert('请先连接视频流'); return; }

        try {
            pcRecorder = new MediaRecorder(stream, { mimeType: 'video/webm;codecs=vp9' });
        } catch(e) {
            try {
                pcRecorder = new MediaRecorder(stream, { mimeType: 'video/webm;codecs=vp8' });
            } catch(e2) {
                pcRecorder = new MediaRecorder(stream, { mimeType: 'video/webm' });
            }
        }

        pcRecordedChunks = [];
        pcRecorder.ondataavailable = function(e) {
            if (e.data && e.data.size > 0) pcRecordedChunks.push(e.data);
        };

        pcRecorder.onstop = function() {
            var blob = new Blob(pcRecordedChunks, { type: 'video/webm' });
            var url = URL.createObjectURL(blob);
            var a = document.createElement('a');
            a.href = url;
            a.download = 'pc_record_' + new Date().toISOString().replace(/[:.]/g, '-') + '.webm';
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
            pcRecordedChunks = [];
        };

        var duration = parseInt(els.pcRecordDuration.value) || 60;
        pcRecorder.start(1000);
        pcRecording = true;
        pcRecordCameraId = recordCameraId;
        if (pcRecordCameraId) {
            apiCall('/api/led/record/start', { camera_id: pcRecordCameraId });
        }
        els.btnPcRecordStart.disabled = true;
        els.btnPcRecordStop.disabled = false;
        els.pcRecordStatus.textContent = 'PC录像中... (' + duration + 's)';
        els.pcRecordStatus.className = 'record-status recording';
        if (pcRecordTimer) clearTimeout(pcRecordTimer);
        pcRecordTimer = setTimeout(function() { ctrlPcRecordStop(); }, duration * 1000);
    };

    window.ctrlPcRecordStop = function() {
        if (pcRecorder && pcRecording) { pcRecorder.stop(); pcRecording = false; }
        if (pcRecordCameraId) {
            apiCall('/api/led/record/stop', { camera_id: pcRecordCameraId });
            pcRecordCameraId = null;
        }
        els.btnPcRecordStart.disabled = false;
        els.btnPcRecordStop.disabled = true;
        els.pcRecordStatus.textContent = '已保存';
        els.pcRecordStatus.className = 'record-status';
        if (pcRecordTimer) { clearTimeout(pcRecordTimer); pcRecordTimer = null; }
    };

    // Time sync — broadcasts to ALL boards
    window.ctrlTimeSync = function() {
        var ts = Date.now();
        apiCall('/api/time/sync', { timestamp: ts }, function(err, resp) {
            var el = $('timeSyncResult');
            if (!el) return;
            if (err) {
                el.textContent = '同步失败: ' + err;
                el.style.color = '#f85149';
            } else {
                var d = new Date(ts);
                el.textContent = '已同步全部: ' + d.toLocaleTimeString();
                el.style.color = '#3fb950';
            }
        });
    };

    window.ctrlLed = function(mode) {
        boardApi('/api/led', { mode: mode });
        var btns = document.querySelectorAll('#controlPanel .led-btn');
        btns.forEach(function(b) { b.style.opacity = '0.6'; });
        if (mode === 0 && btns[0]) btns[0].style.opacity = '1';
        else if (mode === 1 && btns[1]) btns[1].style.opacity = '1';
        else if (mode === 2 && btns[2]) btns[2].style.opacity = '1';
    };

    // ─── Init ───

    function initPreviews() {
        if (els.osdColorPreview) els.osdColorPreview.style.backgroundColor = 'rgb(255,255,255)';
        if (els.maskColorPreview) els.maskColorPreview.style.backgroundColor = 'rgb(0,255,0)';
    }

    window.addEventListener('DOMContentLoaded', function() {
        initDOMElements();
        applyThemeMode('light');
        detectLocalIP();
        initPreviews();
        buildVideoWall();
        buildCameraTabs();
        initDefaultMaskRegions();
    });
})();
