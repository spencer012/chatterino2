// ==UserScript==
// @name         Chatterino: capture Twitch poll / prediction / hype train traffic
// @namespace    chatterino-fork
// @version      1.1
// @description  Records the GQL requests and live (Hermes/PubSub) messages twitch.tv uses for polls, predictions and hype trains, so Chatterino can match them. Click the button in the bottom-left corner to download the capture.
// @match        https://www.twitch.tv/*
// @run-at       document-start
// @grant        none
// ==/UserScript==

// What gets recorded:
// - GQL operations whose name matches INTERESTING (request variables,
//   persisted query hash, the inline query if there is one, and the response).
//   Request headers are never recorded, only whether a Client-Integrity
//   header was sent.
// - Live messages on poll / prediction / hype train / channel points topics.
//
// Tokens are redacted, but the capture still contains your user ID and
// whatever you did (votes, predictions). Look it over before sharing it.
//
// Button: click = download, shift+click = clear, alt+click = toggle capturing
// every GQL operation (not just the interesting ones).

(function () {
    'use strict';

    const INTERESTING = /poll|predict|hype|train|community|points|vote/i;
    const INTERESTING_TOPIC =
        /^(polls|predictions-channel-v1|predictions-user-v1|hype-train-events|community-points-user-v1|community-points-channel-v1)/;
    const STORAGE_KEY = 'chatterino-capture';
    const MAX_ENTRIES = 1500;
    const REDACT_KEYS = new Set([
        'authorization',
        'access_token',
        'accesstoken',
        'token',
        'auth_token',
        'integrity',
        'password',
        'refresh_token',
    ]);

    let captureAll = false;
    let entries = [];
    try {
        entries = JSON.parse(localStorage.getItem(STORAGE_KEY) || '[]');
    } catch (e) {
        entries = [];
    }

    function redact(value) {
        if (Array.isArray(value)) {
            return value.map(redact);
        }
        if (value && typeof value === 'object') {
            const out = {};
            for (const [k, v] of Object.entries(value)) {
                // Emotes are {id, token: "Kappa"}, keep their names.
                const isEmoteName = k === 'token' && 'id' in value;
                out[k] = REDACT_KEYS.has(k.toLowerCase()) && typeof v === 'string' &&
                    !isEmoteName
                    ? '<redacted>'
                    : redact(v);
            }
            return out;
        }
        return value;
    }

    let saveTimer = null;
    function save() {
        if (saveTimer) {
            return;
        }
        saveTimer = setTimeout(() => {
            saveTimer = null;
            try {
                localStorage.setItem(STORAGE_KEY, JSON.stringify(entries));
            } catch (e) {
                // Over the storage quota. Keep the newest half.
                entries = entries.slice(Math.floor(entries.length / 2));
                try {
                    localStorage.setItem(STORAGE_KEY, JSON.stringify(entries));
                } catch (e2) {
                    // Give up on persisting, the in-memory copy still works.
                }
            }
        }, 1000);
    }

    function record(entry) {
        entry.at = new Date().toISOString();
        entry.page = location.pathname;
        entries.push(redact(entry));
        if (entries.length > MAX_ENTRIES) {
            entries.splice(0, entries.length - MAX_ENTRIES);
        }
        save();
        updateButton();
    }

    // ------------------------------------------------------------------
    // GQL (fetch)

    function parseJson(text) {
        try {
            return JSON.parse(text);
        } catch (e) {
            return undefined;
        }
    }

    function hasIntegrityHeader(input, init) {
        const check = (headers) => {
            if (!headers) {
                return false;
            }
            if (headers instanceof Headers) {
                return headers.has('Client-Integrity');
            }
            if (Array.isArray(headers)) {
                return headers.some(([k]) => k.toLowerCase() === 'client-integrity');
            }
            return Object.keys(headers).some((k) => k.toLowerCase() === 'client-integrity');
        };
        return check(init && init.headers) || (input instanceof Request && check(input.headers));
    }

    const originalFetch = window.fetch;
    window.fetch = async function (input, init) {
        const url = input instanceof Request ? input.url : String(input);
        if (!url.includes('gql.twitch.tv')) {
            return originalFetch.apply(this, arguments);
        }

        let bodyText;
        try {
            if (init && typeof init.body === 'string') {
                bodyText = init.body;
            } else if (input instanceof Request) {
                bodyText = await input.clone().text();
            }
        } catch (e) {
            bodyText = undefined;
        }

        const body = bodyText ? parseJson(bodyText) : undefined;
        const ops = Array.isArray(body) ? body : body ? [body] : [];
        const wanted = ops
            .map((op, index) => ({ op, index }))
            .filter(({ op }) => captureAll || INTERESTING.test(op.operationName || '') ||
                INTERESTING.test(op.query || ''));

        const response = await originalFetch.apply(this, arguments);
        if (wanted.length === 0) {
            return response;
        }

        const integrity = hasIntegrityHeader(input, init);
        response
            .clone()
            .text()
            .then((text) => {
                const parsed = parseJson(text);
                for (const { op, index } of wanted) {
                    const result = Array.isArray(parsed) ? parsed[index] : parsed;
                    record({
                        kind: 'gql',
                        operationName: op.operationName,
                        sha256Hash: op.extensions && op.extensions.persistedQuery
                            ? op.extensions.persistedQuery.sha256Hash
                            : undefined,
                        query: op.query,
                        variables: op.variables,
                        status: response.status,
                        sentClientIntegrity: integrity,
                        response: result,
                    });
                }
            })
            .catch(() => {});
        return response;
    };

    // ------------------------------------------------------------------
    // Live updates (Hermes and legacy PubSub websockets)

    const OriginalWebSocket = window.WebSocket;

    function wrapSocket(socket, url) {
        // Hermes subscription ID -> topic.
        const subscriptions = new Map();

        const originalSend = socket.send;
        socket.send = function (data) {
            const msg = typeof data === 'string' ? parseJson(data) : undefined;
            if (msg && msg.type === 'subscribe' && msg.subscribe) {
                const topic = msg.subscribe.pubsub && msg.subscribe.pubsub.topic;
                if (topic) {
                    subscriptions.set(msg.subscribe.id, topic);
                    if (INTERESTING_TOPIC.test(topic)) {
                        record({ kind: 'ws-subscribe', url, topic });
                    }
                }
            }
            return originalSend.apply(this, arguments);
        };

        socket.addEventListener('message', (event) => {
            if (typeof event.data !== 'string') {
                return;
            }
            const msg = parseJson(event.data);
            if (!msg) {
                return;
            }

            let topic;
            let payload;
            if (msg.type === 'notification' && msg.notification) {
                // Hermes
                topic = subscriptions.get(msg.notification.subscription &&
                    msg.notification.subscription.id);
                payload = msg.notification.pubsub;
            } else if (msg.type === 'MESSAGE' && msg.data) {
                // Legacy PubSub
                topic = msg.data.topic;
                payload = msg.data.message;
            } else if (msg.type === 'subscribeResponse' &&
                msg.subscribeResponse && msg.subscribeResponse.result !== 'ok') {
                record({ kind: 'ws-subscribe-failed', url, message: msg });
                return;
            } else {
                return;
            }

            if (!topic || !INTERESTING_TOPIC.test(topic)) {
                return;
            }
            record({
                kind: 'ws-message',
                url,
                topic,
                message: typeof payload === 'string' ? parseJson(payload) ?? payload : payload,
            });
        });
    }

    window.WebSocket = new Proxy(OriginalWebSocket, {
        construct(target, args) {
            const socket = new target(...args);
            const url = String(args[0] || '');
            if (url.includes('hermes.twitch.tv') || url.includes('pubsub-edge.twitch.tv')) {
                wrapSocket(socket, url.split('?')[0]);
            }
            return socket;
        },
    });

    // ------------------------------------------------------------------
    // Button

    let button = null;

    function updateButton() {
        if (!button) {
            return;
        }
        button.textContent = `${captureAll ? 'ALL ' : ''}capture: ${entries.length}`;
    }

    function download() {
        const blob = new Blob([JSON.stringify(entries, null, 2)], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = `twitch-capture-${new Date().toISOString().replace(/[:.]/g, '-')}.json`;
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    }

    function addButton() {
        if (button || !document.body) {
            return;
        }
        button = document.createElement('button');
        button.title = 'Click: download capture\nShift+click: clear\nAlt+click: toggle capturing all GQL operations';
        Object.assign(button.style, {
            position: 'fixed',
            left: '8px',
            bottom: '8px',
            zIndex: 99999,
            padding: '4px 8px',
            font: '12px monospace',
            color: '#fff',
            background: 'rgba(145, 71, 255, 0.85)',
            border: 'none',
            borderRadius: '4px',
            cursor: 'pointer',
        });
        button.addEventListener('click', (event) => {
            if (event.shiftKey) {
                if (confirm('Clear the capture?')) {
                    entries = [];
                    localStorage.removeItem(STORAGE_KEY);
                }
            } else if (event.altKey) {
                captureAll = !captureAll;
            } else {
                download();
            }
            updateButton();
        });
        document.body.appendChild(button);
        updateButton();
    }

    if (document.body) {
        addButton();
    } else {
        document.addEventListener('DOMContentLoaded', addButton);
    }

    window.__chatterinoCapture = {
        entries: () => entries,
        download,
        clear: () => {
            entries = [];
            localStorage.removeItem(STORAGE_KEY);
            updateButton();
        },
    };
})();
