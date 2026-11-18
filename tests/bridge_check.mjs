// SPDX-License-Identifier: MPL-2.0
// The bridge window (web/bridge.html) against a running simulator, without a browser: fetch /bridge, then run its
// script in a vm with a stand-in opener and node's own WebSocket, and relay a HELLO through it.
//   node tests/bridge_check.mjs <port>
import vm from "node:vm";

const port = process.argv[2] || "7720";
const host = `127.0.0.1:${port}`;
let fail = 0;
const ok = (cond, what) => { console.log(`  ${cond ? "ok  " : "FAIL"}  bridge: ${what}`); if (!cond) fail = 1; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const res = await fetch(`http://${host}/bridge?origin=${encodeURIComponent("http://localhost:5173")}`);
const html = await res.text();
ok(res.status === 200, `GET /bridge 200 (${res.status})`);
ok((res.headers.get("content-type") || "").startsWith("text/html"), "text/html");
ok(res.headers.get("cache-control") === "no-store", "Cache-Control: no-store");
ok((await fetch(`http://${host}/bridge.html`)).status === 200, "GET /bridge.html 200");
ok(!/(src|href)=["']?(https?:)?\/\//i.test(html), "no external resources");
const m = html.match(/<script>([\s\S]*?)<\/script>/);
ok(m && m[1].includes('mr: "hello"'), "has the bridge script");
if (!m) process.exit(1);

function run(opts) {
  const posts = [], handlers = [];
  const els = {};
  const el = (id) => (els[id] ??= { id, textContent: "", className: "" });
  const opener = opts.noOpener ? null
    : { closed: false, postMessage: (msg, target, transfer) => posts.push({ msg, target, transfer }) };
  const win = {
    opener, closedCalled: false,
    addEventListener: (t, f) => { if (t === "message") handlers.push(f); },
    close() { win.closedCalled = true; },
  };
  const timers = [];
  const ctx = {
    window: win, document: { getElementById: el },
    location: { host, search: opts.search || "" },
    URL, URLSearchParams, WebSocket, ArrayBuffer, console,
    setInterval: (f, ms) => { const t = setInterval(f, ms); timers.push(t); return t; },
  };
  vm.runInNewContext(m[1], ctx);
  const send = (data, origin, source = opener) => handlers.forEach((f) => f({ data, origin, source }));
  return { posts, send, win, opener, els, stop: () => timers.forEach(clearInterval) };
}

// no opener: a note, nothing else
{
  const b = run({ noOpener: true });
  ok(/RemoteFM1 site/.test(b.els.h?.textContent || ""), "no opener: shows the note");
  b.stop();
}

// hello goes to the fixed origins plus ?origin
{
  const b = run({ search: "?origin=" + encodeURIComponent("http://localhost:5173") });
  const targets = b.posts.filter((p) => p.msg.mr === "hello" && p.msg.v === 1).map((p) => p.target);
  ok(targets.join() === "https://remotefm1.com,https://www.remotefm1.com,http://localhost:5173",
     `hello targets (${targets.join(" ")})`);
  b.stop();
}
{
  const b = run({ search: "?origin=" + encodeURIComponent("http://evil.example") });
  ok(b.posts.length === 2, "?origin that is plain http and not loopback is ignored");
  b.stop();
}

// the relay
{
  const b = run({});
  b.posts.length = 0;
  b.send({ mr: "open" }, "https://evil.example");
  b.send({ mr: "open" }, "https://remotefm1.com", {});
  await sleep(300);
  ok(b.posts.length === 0, "ignores a foreign origin and a foreign source");

  b.send({ mr: "open" }, "https://remotefm1.com");
  for (let i = 0; i < 40 && !b.posts.some((p) => p.msg.mr === "open"); i++) await sleep(50);
  ok(b.posts.some((p) => p.msg.mr === "open" && p.target === "https://remotefm1.com"), "{mr:open} after the ws opens");

  b.send({ mr: "open" }, "https://www.remotefm1.com");   // allowed, but not the peer
  const name = new TextEncoder().encode("bridge-test");
  const hello = new Uint8Array(2 + 1 + 2 + name.length);
  new DataView(hello.buffer).setUint16(0, 1 + 2 + name.length, true);
  hello[2] = 0x81; new DataView(hello.buffer).setUint16(3, 1, true); hello.set(name, 5);
  b.send({ mr: "send", data: hello.buffer }, "https://remotefm1.com");

  let got = new Uint8Array(0);
  for (let i = 0; i < 40; i++) {
    const chunks = b.posts.filter((p) => p.msg.mr === "data").map((p) => new Uint8Array(p.msg.data));
    got = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
    let o = 0; for (const c of chunks) { got.set(c, o); o += c.length; }
    if (got.length >= 7) break;
    await sleep(50);
  }
  const dataPost = b.posts.find((p) => p.msg.mr === "data");
  ok(got.length >= 7 && got[2] === 0x01 && new TextDecoder().decode(got.slice(3, 7)) === "MOVR",
     "HELLO in, device HELLO back as {mr:data}");
  ok(dataPost && dataPost.transfer?.[0] === dataPost.msg.data, "data buffer is in the transfer list");
  ok(b.posts.filter((p) => p.msg.mr === "open").length === 1, "a non-peer origin is ignored after the first");

  b.send({ mr: "close" }, "https://remotefm1.com");
  ok(b.win.closedCalled, "{mr:close} closes the window");
  b.stop();
}

// the opener goes away
{
  const b = run({});
  b.opener.closed = true;
  await sleep(1200);
  ok(b.win.closedCalled, "closes itself when the opener is gone");
  b.stop();
}

process.exit(fail);
