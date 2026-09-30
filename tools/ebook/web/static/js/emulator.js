/**
 * GDEM0397T81P 墨水屏 LVGL 模拟器（WASM）
 * 480×800 · .ef 字体 · 与 ESP32 BookScreen 同路径渲染
 */

/** Chrome 131+：TextDecoder 不接受 resizable ArrayBuffer（Emscripten 可增长 WASM 堆） */
function patchTextDecoderForResizableBuffer() {
  if (globalThis.__epdTextDecoderPatched) return;
  globalThis.__epdTextDecoderPatched = true;
  const origDecode = TextDecoder.prototype.decode;
  TextDecoder.prototype.decode = function decodePatched(input, options) {
    let fixed = input;
    if (input instanceof ArrayBuffer) {
      if (input.resizable) fixed = input.slice(0);
    } else if (ArrayBuffer.isView(input)) {
      const buf = input.buffer;
      if (buf?.resizable) fixed = new Uint8Array(input);
    }
    return origDecode.call(this, fixed, options);
  };
}
patchTextDecoderForResizableBuffer();

let scriptPromise = null;
let runtimePromise = null;
let tickTimer = null;

function startTick(Module) {
  if (tickTimer) return;
  tickTimer = setInterval(() => {
    try {
      Module.ccall("emulator_tick", null, [], []);
    } catch (_) {
      /* ignore */
    }
  }, 66);
}

function stopTick() {
  if (tickTimer) {
    clearInterval(tickTimer);
    tickTimer = null;
  }
}

function ensureWasmScript() {
  if (scriptPromise) return scriptPromise;

  scriptPromise = new Promise((resolve, reject) => {
    if (window.Module?.calledRun) {
      resolve();
      return;
    }

    const existing = document.querySelector('script[data-ebook-emulator="1"]');
    if (existing) {
      existing.addEventListener("load", () => resolve(), { once: true });
      existing.addEventListener("error", () => reject(new Error("WASM 脚本加载失败")), {
        once: true,
      });
      return;
    }

    window.Module = window.Module || {};
    window.Module.locateFile = (path) => `/wasm/${path.replace(/\?.*$/, "")}`;
    window.Module.printErr = () => {};
    window.Module.print = () => {};
    const cvs = document.getElementById("epd-canvas");
    if (cvs) window.Module.canvas = cvs;

    const s = document.createElement("script");
    s.src = `/wasm/ebook_emulator.js?v=${Date.now()}`;
    s.async = true;
    s.dataset.ebookEmulator = "1";
    s.onload = () => resolve();
    s.onerror = () => reject(new Error("加载失败: /wasm/ebook_emulator.js"));
    document.head.appendChild(s);
  });

  return scriptPromise;
}

function ensureRuntime() {
  if (runtimePromise) return runtimePromise;

  runtimePromise = (async () => {
    await ensureWasmScript();
    const Module = window.Module;
    if (!Module) throw new Error("Module 未就绪");

    if (Module.calledRun) return Module;

    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("WASM 初始化超时")), 30000);
      const prev = Module.onRuntimeInitialized;
      Module.onRuntimeInitialized = () => {
        clearTimeout(timer);
        if (typeof prev === "function") prev();
        resolve();
      };
    });

    return Module;
  })();

  return runtimePromise;
}

export function createEpdEmulator({ canvasId = "epd-canvas", statusId = "epd-status" } = {}) {
  const canvas = document.getElementById(canvasId);
  const statusEl = document.getElementById(statusId);
  let Module = null;
  let ready = false;
  let initPromise = null;
  let efFont = "misans_25_2.ef";
  let openBookPath = null;

  function setStatus(msg) {
    if (statusEl) statusEl.textContent = msg || "";
  }

  async function fetchBytes(url) {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${url}`);
    const raw = await res.arrayBuffer();
    const bytes = new Uint8Array(raw.byteLength);
    bytes.set(new Uint8Array(raw));
    return bytes;
  }

  async function writeMem(path, bytes) {
    if (!Module?.FS) throw new Error("WASM FS 不可用");
    const dir = path.substring(0, path.lastIndexOf("/"));
    if (dir) {
      try {
        Module.FS.mkdir(dir);
      } catch (_) {
        /* exists */
      }
    }
    try {
      Module.FS.unlink(path);
    } catch (_) {
      /* missing */
    }
    Module.FS.writeFile(path, bytes);
  }

  async function init() {
    if (ready) return true;
    if (initPromise) return initPromise;

    initPromise = (async () => {
      setStatus("加载 WASM…");
      try {
        Module = await ensureRuntime();

        await loadEfFont(efFont);
        if (!Module._epdEmuInited) {
          Module.ccall("emulator_init", "number", [], []);
          Module._epdEmuInited = true;
          startTick(Module);
        } else {
          Module.ccall("emulator_refresh", null, [], []);
        }
        bindPointer(canvas);
        ready = true;
        setStatus("模拟器就绪 · 480×800");
        return true;
      } catch (e) {
        setStatus(String(e.message || e));
        initPromise = null;
        return false;
      }
    })();

    return initPromise;
  }

  async function loadEfFont(name, { reloadBook = true } = {}) {
    efFont = name || efFont;
    if (!Module) {
      Module = await ensureRuntime();
    }
    const bytes = await fetchBytes(`/api/fonts/ef/${encodeURIComponent(efFont)}`);
    const path = `/fonts/${efFont}`;
    await writeMem(path, bytes);
    Module.ccall("emulator_set_epdfont", null, ["string"], [path]);
    Module.ccall("emulator_refresh", null, [], []);
    if (ready) {
      setStatus(reloadBook && openBookPath ? `阅读中 · ${efFont}` : `字体 ${efFont}`);
    }
  }

  function bindPointer(cvs) {
    if (!cvs || !Module) return;
    let down = false;
    const toLocal = (e) => {
      const r = cvs.getBoundingClientRect();
      const sx = 480 / r.width;
      const sy = 800 / r.height;
      return {
        x: Math.round((e.clientX - r.left) * sx),
        y: Math.round((e.clientY - r.top) * sy),
      };
    };
    cvs.addEventListener("pointerdown", (e) => {
      down = true;
      const p = toLocal(e);
      Module._ptrX = p.x;
      Module._ptrY = p.y;
      Module._ptrDown = 1;
    });
    cvs.addEventListener("pointerup", (e) => {
      down = false;
      const p = toLocal(e);
      Module._ptrX = p.x;
      Module._ptrY = p.y;
      Module._ptrDown = 0;
    });
    cvs.addEventListener("pointermove", (e) => {
      if (!down) return;
      const p = toLocal(e);
      Module._ptrX = p.x;
      Module._ptrY = p.y;
    });
  }

  async function openEbookUrl(url) {
    if (!(await init())) return false;
    setStatus("打开书籍…");
    const bytes = await fetchBytes(url);
    const path = "/books/open.ebook";
    await writeMem(path, bytes);
    await new Promise((r) => setTimeout(r, 0));
    const ok = Module.ccall("emulator_open_ebook_path", "number", ["string"], [path]);
    if (!ok) {
      const err = Module.ccall("emulator_last_error", "string", [], []) || "打开失败";
      setStatus(err);
      openBookPath = null;
      return false;
    }
    openBookPath = path;
    setStatus(`阅读中 · ${efFont}`);
    Module.ccall("emulator_refresh", null, [], []);
    return true;
  }

  async function openBookId(id) {
    return openEbookUrl(`/api/book/${encodeURIComponent(id)}/download`);
  }

  function nextPage() {
    Module?.ccall("emulator_next_page", null, [], []);
  }

  function prevPage() {
    Module?.ccall("emulator_prev_page", null, [], []);
  }

  function getToc() {
    if (!Module?._epdEmuInited) return [];
    const n = Module.ccall("emulator_toc_count", "number", [], []);
    const items = [];
    for (let i = 0; i < n; i++) {
      const title =
        Module.ccall("emulator_toc_title", "string", ["number"], [i]) || `章节 ${i + 1}`;
      items.push({ index: i, title });
    }
    return items;
  }

  function currentTocIndex() {
    if (!Module?._epdEmuInited) return -1;
    return Module.ccall("emulator_current_toc_index", "number", [], []);
  }

  function goToToc(index) {
    if (!Module) return;
    Module.ccall("emulator_go_toc", null, ["number"], [index]);
    Module.ccall("emulator_refresh", null, [], []);
  }

  return {
    init,
    loadEfFont,
    openBookId,
    openEbookUrl,
    nextPage,
    prevPage,
    getToc,
    currentTocIndex,
    goToToc,
    isReady: () => ready,
  };
}
