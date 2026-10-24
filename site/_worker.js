// _worker.js - Cloudflare Pages advanced mode, routed by _routes.json to the films only. Pages answers a byte-range
// request with the whole file, and Safari (every iPhone) will not play a video unless the server answers ranges with
// 206 Partial Content, so this slices the film the asset server returns. Every other file is served by Pages as usual.
export default {
  async fetch(request, env) {
    const range = request.headers.get("Range");
    if (request.method !== "GET" || !range) {
      const response = await env.ASSETS.fetch(request);
      const headers = new Headers(response.headers);
      headers.set("Accept-Ranges", "bytes"); // tells the browser it may ask for parts
      return new Response(response.body, { status: response.status, headers });
    }
    const full = await env.ASSETS.fetch(new Request(request.url, { method: "GET" }));
    if (full.status !== 200) return full;
    const body = await full.arrayBuffer();
    const size = body.byteLength;
    const m = /^bytes=(\d*)-(\d*)$/.exec(range.trim());
    let start = -1, end = -1;
    if (m && m[1] !== "") {
      start = parseInt(m[1], 10);
      end = m[2] === "" ? size - 1 : Math.min(parseInt(m[2], 10), size - 1);
    } else if (m && m[2] !== "") { // the last N bytes
      start = Math.max(0, size - parseInt(m[2], 10));
      end = size - 1;
    }
    if (start < 0 || start >= size || end < start) {
      return new Response(null, { status: 416, headers: { "Content-Range": `bytes */${size}` } });
    }
    const headers = new Headers(full.headers);
    headers.set("Accept-Ranges", "bytes");
    headers.set("Content-Range", `bytes ${start}-${end}/${size}`);
    headers.set("Content-Length", String(end - start + 1));
    return new Response(body.slice(start, end + 1), { status: 206, headers });
  },
};
