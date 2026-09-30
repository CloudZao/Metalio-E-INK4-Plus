import { createEpdEmulator } from "./emulator.js";

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];

const emulator = createEpdEmulator({ canvasId: "epd-canvas", statusId: "epd-status" });

const state = {
  lastConvertedId: null,
  readingBookId: null,
  readingBookMeta: null,
  uploadId: null,
  pages: 0,
  format: "",
  maxUploadMb: 512,
  efFont: "misans_25_2.ef",
};

  async function fetchJson(url, options = {}, { failOnHttpError = true } = {}) {
    const res = await fetch(url, options);
    const ct = (res.headers.get("content-type") || "").toLowerCase();
    if (!ct.includes("application/json")) {
      if (res.status === 413) {
        throw new Error(
          `文件超过上传上限（${state.maxUploadMb} MB），请拆分源书或使用 convert_ebook.py 命令行转换`
        );
      }
      const hint = (await res.text()).replace(/\s+/g, " ").trim().slice(0, 100);
      throw new Error(`服务器错误（${res.status}）${hint ? `：${hint}` : ""}`);
    }
    const data = await res.json();
    if (failOnHttpError && !res.ok) {
      throw new Error(data?.error || `请求失败（${res.status}）`);
    }
    return data;
  }

  // theme
  const saved = localStorage.getItem("ebook-theme") || "light";
  document.documentElement.setAttribute("data-theme", saved);
  $("#theme-toggle").addEventListener("click", () => {
    const cur = document.documentElement.getAttribute("data-theme") === "dark" ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", cur);
    localStorage.setItem("ebook-theme", cur);
  });

  function scrollToZone(id) {
    document.getElementById(id)?.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }

  function highlightActiveBook(bookId) {
    state.readingBookId = bookId || null;
    $$(".book-card").forEach((c) => c.classList.toggle("active", c.dataset.id === bookId));
  }

  function updateLibraryCount(n) {
    const el = $("#library-count");
    if (el) el.textContent = n != null ? `书库 ${n} 本` : "书库 —";
  }

  async function renderTocPanel(bookMeta) {
    const panel = $("#epd-toc-panel");
    const list = $("#epd-toc-list");
    const meta = $("#epd-toc-meta");
    if (!panel || !list) return;

    const items = emulator.getToc();
    if (!items.length) {
      panel.hidden = true;
      list.innerHTML = "";
      if (meta) meta.textContent = "";
      return;
    }

    panel.hidden = false;
    const cur = emulator.currentTocIndex();
    if (meta) {
      const bookTitle = bookMeta?.title ? String(bookMeta.title) : "";
      meta.textContent = bookTitle ? `${bookTitle} · ${items.length} 章` : `${items.length} 章`;
    }

    list.innerHTML = items
      .map(
        (it) =>
          `<button type="button" class="toc-item${it.index === cur ? " active" : ""}" data-idx="${it.index}">${escapeHtml(it.title)}</button>`
      )
      .join("");

    list.querySelectorAll(".toc-item").forEach((btn) => {
      btn.addEventListener("click", () => {
        const idx = Number(btn.dataset.idx);
        emulator.goToToc(idx);
        renderTocPanel(state.readingBookMeta);
      });
    });

    const active = list.querySelector(".toc-item.active");
    if (active) {
      active.scrollIntoView({ block: "nearest" });
    }
  }

  function hideTocPanel() {
    const panel = $("#epd-toc-panel");
    const list = $("#epd-toc-list");
    const meta = $("#epd-toc-meta");
    if (panel) panel.hidden = true;
    if (list) list.innerHTML = "";
    if (meta) meta.textContent = "";
  }

  async function openInEmulator(bookId) {
    hideTocPanel();
    state.readingBookMeta = null;
    highlightActiveBook(bookId);
    try {
      try {
        state.readingBookMeta = await fetchJson(`/api/book/${encodeURIComponent(bookId)}`);
      } catch (_) {
        /* optional */
      }
      const ok = await emulator.openBookId(bookId);
      if (ok) {
        await renderTocPanel(state.readingBookMeta);
        scrollToZone("zone-reader");
        const title = state.readingBookMeta?.title || bookId;
        $("#epd-status").textContent = `阅读中 · ${title}`;
      }
    } catch (err) {
      $("#epd-status").textContent = String(err.message || err);
    }
  }

  async function loadEfFonts() {
    const sel = $("#ef-font-select");
    if (!sel) return;
    try {
      const data = await fetchJson("/api/fonts/ef");
      sel.innerHTML = "";
      for (const f of data.fonts || []) {
        const o = document.createElement("option");
        o.value = f.id;
        o.textContent = `${f.name} (${Math.round(f.size / 1024)} KB)`;
        sel.appendChild(o);
      }
      if (data.default) {
        sel.value = data.default;
        state.efFont = data.default;
      }
    } catch (_) {
      sel.innerHTML = `<option value="">无 .ef 字体</option>`;
    }
  }

  $("#ef-font-select")?.addEventListener("change", async (e) => {
    state.efFont = e.target.value;
    try {
      await emulator.loadEfFont(state.efFont);
      if (state.readingBookId) {
        await renderTocPanel(state.readingBookMeta);
        const title = state.readingBookMeta?.title || state.readingBookId;
        $("#epd-status").textContent = `阅读中 · ${title} · ${state.efFont}`;
      }
    } catch (err) {
      $("#epd-status").textContent = String(err.message || err);
    }
  });

  $("#epd-prev")?.addEventListener("click", () => {
    emulator.prevPage();
    renderTocPanel(state.readingBookMeta);
  });
  $("#epd-next")?.addEventListener("click", () => {
    emulator.nextPage();
    renderTocPanel(state.readingBookMeta);
  });

  loadEfFonts();
  emulator.init().catch(() => {});

  function syncBinarizeOpts() {
    const keepColor = $("#keep_color")?.checked;
    const method = $("#binarize_method")?.value || "otsu";
    const fixed = $("#binarize-fixed-opts");
    const sau = $("#binarize-sauvola-opts");
    if (fixed) fixed.hidden = keepColor || method !== "fixed";
    if (sau) sau.hidden = keepColor || method !== "sauvola";
    const box = $("#binarize-box");
    if (box && keepColor) {
      $$("select, input, button", box).forEach((el) => {
        if (el.id !== "keep_color") el.disabled = true;
      });
    } else if (box) {
      $$("select, input, button", box).forEach((el) => {
        el.disabled = false;
      });
    }
  }

  function syncKeepColor() {
    syncBinarizeOpts();
    const tip = $("#binarize-tip");
    if ($("#keep_color")?.checked && tip) {
      tip.textContent =
        "已勾选保留彩色：文件内以 JPEG 存储；Web 阅读彩色显示；墨水屏设备阅读时仍按原生 EPUB 同款 Bayer 转黑白。";
    }
  }

  function appendBinarizeFields(fd) {
    if ($("#keep_color")?.checked) {
      fd.set("keep_color", "1");
      return;
    }
    const method = $("#binarize_method")?.value || "otsu";
    fd.set("binarize_method", method);
    fd.set("binarize_contrast", $("#binarize_contrast")?.value || "1");
    fd.set("binarize_threshold", $("#binarize_threshold")?.value || "128");
    fd.set("binarize_window", $("#binarize_window")?.value || "25");
    fd.set("binarize_k", $("#binarize_k")?.value || "0.34");
  }

  function newBookId() {
    if (globalThis.crypto?.randomUUID) {
      return globalThis.crypto.randomUUID().replace(/-/g, "");
    }
    // 极旧环境兜底
    const bytes = new Uint8Array(16);
    if (globalThis.crypto?.getRandomValues) {
      globalThis.crypto.getRandomValues(bytes);
    } else {
      for (let i = 0; i < 16; i++) bytes[i] = (Math.random() * 256) | 0;
    }
    return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
  }

  function fillBookId(force = false) {
    const el = $("#book_id");
    if (!el) return;
    if (!force && el.value.trim()) return;
    el.value = newBookId();
  }

  $("#btn-regen-book-id")?.addEventListener("click", () => fillBookId(true));

  async function stageFile(file) {
    const statusEl = $("#preview-status");
    const box = $("#binarize-box");
    const hint = $("#preview-pages-hint");
    state.uploadId = null;
    state.pages = 0;
    state.format = "";
    $("#upload_id").value = "";
    if (box) box.hidden = true;
    if ($("#preview-grid")) $("#preview-grid").hidden = true;
    if (!file) {
      dropLabel.textContent = "拖入文件或点击选择";
      return;
    }
    // 每次选/拖入新书都换新 book_id（覆盖旧值）
    fillBookId(true);
    dropLabel.textContent = `上传中… ${file.name}`;
    if (statusEl) statusEl.textContent = "上传中…";
    const fd = new FormData();
    fd.append("file", file);
    const data = await fetchJson("/api/stage", { method: "POST", body: fd });
    if (!data.ok) throw new Error(data.error || "上传失败");
    state.uploadId = data.upload_id;
    state.pages = data.pages || 0;
    state.format = data.format || "";
    $("#upload_id").value = state.uploadId;
    dropLabel.textContent = file.name;
    const canPreview = state.format === "pdf" || state.format === "epub";
    if (box) {
      box.hidden = !canPreview;
    }
    const tip = $("#binarize-tip");
    const idxText = $("#preview-index-text");
    if (state.format === "pdf") {
      if (tip) {
        tip.textContent =
          "选好方法与参数后点预览，满意再转换。漫画/扫描件请同时勾选「整页转图片」，预览与成书才一致。";
      }
      if (idxText) idxText.textContent = "预览页（从 1）";
      if (hint) hint.textContent = state.pages ? `共 ${state.pages} 页` : "—";
    } else if (state.format === "epub") {
      if (tip) {
        tip.textContent =
          "按正文出现顺序预览插图二值化效果；参数会用于整书转换。无图 EPUB 不会显示本面板。";
      }
      if (idxText) idxText.textContent = "预览图（从 1）";
      if (hint) {
        hint.textContent = state.pages ? `共 ${state.pages} 张图` : "无插图可预览";
      }
      if (!state.pages && box) box.hidden = true;
    } else if (hint) {
      hint.textContent = "—";
    }
    const pageInput = $("#preview_page");
    if (pageInput && state.pages) {
      pageInput.max = String(state.pages);
      pageInput.value = "1";
    }
    if (statusEl) {
      statusEl.textContent = canPreview && (state.format !== "epub" || state.pages > 0) ? "可预览二值化" : "";
    }
  }

  // dropzone
  const drop = $("#drop");
  const fileInput = $("#file");
  const dropLabel = $("#drop-label");
  const coverFileInput = $("#cover_file");
  const coverFileHint = $("#cover-file-hint");
  const btnClearCoverFile = $("#btn-clear-cover-file");

  function syncCoverFileHint() {
    const f = coverFileInput?.files?.[0];
    if (coverFileHint) {
      coverFileHint.textContent = f
        ? `已选：${f.name}`
        : "未选择；不选则用源书封面（TXT 无封面）";
    }
    if (btnClearCoverFile) btnClearCoverFile.hidden = !f;
  }

  coverFileInput?.addEventListener("change", syncCoverFileHint);
  btnClearCoverFile?.addEventListener("click", () => {
    if (!coverFileInput) return;
    coverFileInput.value = "";
    syncCoverFileHint();
  });
  syncCoverFileHint();
  ["dragenter", "dragover"].forEach((ev) =>
    drop.addEventListener(ev, (e) => {
      e.preventDefault();
      drop.classList.add("drag");
    })
  );
  ["dragleave", "drop"].forEach((ev) =>
    drop.addEventListener(ev, (e) => {
      e.preventDefault();
      drop.classList.remove("drag");
    })
  );
  drop.addEventListener("drop", (e) => {
    const f = e.dataTransfer.files?.[0];
    if (f) {
      const dt = new DataTransfer();
      dt.items.add(f);
      fileInput.files = dt.files;
      stageFile(f).catch((err) => {
        dropLabel.textContent = f.name;
        const s = $("#preview-status");
        if (s) s.textContent = String(err.message || err);
      });
    }
  });
  fileInput.addEventListener("change", () => {
    const f = fileInput.files?.[0];
    stageFile(f).catch((err) => {
      dropLabel.textContent = f?.name || "拖入文件或点击选择";
      const s = $("#preview-status");
      if (s) s.textContent = String(err.message || err);
    });
  });

  $("#pdf_pages_as_images").addEventListener("change", (e) => {
    $("#scan-opts").hidden = !e.target.checked;
  });
  $("#binarize_method")?.addEventListener("change", syncBinarizeOpts);
  $("#keep_color")?.addEventListener("change", syncKeepColor);
  $("#binarize_threshold")?.addEventListener("input", (e) => {
    const el = $("#threshold-val");
    if (el) el.textContent = String(e.target.value);
  });
  syncBinarizeOpts();

  $("#btn-preview")?.addEventListener("click", async () => {
    const statusEl = $("#preview-status");
    const grid = $("#preview-grid");
    if (!state.uploadId) {
      if (statusEl) statusEl.textContent = "请先选择 PDF 或 EPUB";
      return;
    }
    if (statusEl) statusEl.textContent = "预览生成中…";
    try {
      const fd = new FormData();
      fd.set("upload_id", state.uploadId);
      const page = Math.max(1, parseInt($("#preview_page")?.value || "1", 10) || 1);
      fd.set("page", String(page - 1));
      fd.set("pdf_page_scale", $("#pdf_page_scale")?.value || "1.5");
      appendBinarizeFields(fd);
      const data = await fetchJson("/api/preview-binarize", { method: "POST", body: fd });
      if (!data.ok) throw new Error(data.error || "预览失败");
      $("#preview-color").src = `data:image/png;base64,${data.color_png}`;
      $("#preview-gray").src = `data:image/png;base64,${data.gray_png}`;
      $("#preview-binary").src = `data:image/png;base64,${data.binary_png}`;
      if (grid) grid.hidden = false;
      const unit = (data.format || state.format) === "epub" ? "图" : "页";
      if (statusEl) {
        const lab = data.label ? ` · ${data.label}` : "";
        statusEl.textContent = `第 ${data.page + 1}/${data.pages} ${unit}${lab} · ${data.method} · ${data.width}×${data.height}`;
      }
      if (data.pages) {
        state.pages = data.pages;
        const hint = $("#preview-pages-hint");
        if (hint) {
          hint.textContent =
            (data.format || state.format) === "epub"
              ? `共 ${data.pages} 张图`
              : `共 ${data.pages} 页`;
        }
      }
    } catch (err) {
      if (statusEl) statusEl.textContent = String(err.message || err);
      if (grid) grid.hidden = true;
    }
  });

  // convert with progress polling
  $("#convert-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const status = $("#convert-status");
    const btn = $("#btn-convert");
    const bar = $("#progress-bar");
    const pctEl = $("#progress-pct");
    const msgEl = $("#progress-msg");
    const logEl = $("#progress-log");
    const actions = $("#convert-actions");
    status.hidden = false;
    status.className = "status";
    if (actions) actions.hidden = true;
    bar.style.width = "0%";
    pctEl.textContent = "0%";
    msgEl.textContent = "上传中…";
    logEl.textContent = "";
    btn.disabled = true;
    try {
      if (!state.uploadId && !fileInput.files?.[0]) {
        throw new Error("请先选择文件");
      }
      // 若尚未 stage（极少见），先 stage
      if (!state.uploadId && fileInput.files?.[0]) {
        await stageFile(fileInput.files[0]);
      }
      const fd = new FormData(e.target);
      fd.delete("file");
      fd.set("upload_id", state.uploadId || "");
      if (!$("#pdf_pages_as_images").checked) fd.delete("pdf_pages_as_images");
      else fd.set("pdf_pages_as_images", "1");
      ["no_images", "no_cover", "ignore_pdf_toc", "keep_color"].forEach((k) => {
        const el = e.target.elements[k];
        if (el?.checked) fd.set(k, "1");
        else fd.delete(k);
      });
      // FormData(form) 会带上 cover_file；若未选则删掉空字段
      const coverInput = $("#cover_file");
      if (!coverInput?.files?.length) {
        fd.delete("cover_file");
      }
      if (e.target.elements.no_cover?.checked && coverInput?.files?.length) {
        throw new Error("「不要封面」与自定义封面不能同时使用");
      }
      appendBinarizeFields(fd);
      const start = await fetchJson("/api/convert", { method: "POST", body: fd });
      if (!start.ok || !start.job_id) throw new Error(start.error || "启动转换失败");

      const jobId = start.job_id;
      let data = start;
      while (data.status === "queued" || data.status === "running") {
        bar.style.width = `${data.percent || 0}%`;
        pctEl.textContent = `${data.percent || 0}%`;
        msgEl.textContent = data.message || "转换中…";
        await sleep(400);
        data = await fetchJson(`/api/convert/${encodeURIComponent(jobId)}`);
        if (!data.ok && data.status !== "error") throw new Error(data.error || "进度查询失败");
        if (data.status === "error") throw new Error(data.error || "转换失败");
      }

      const result = data.result || data;
      bar.style.width = "100%";
      pctEl.textContent = "100%";
      msgEl.textContent = "完成";
      status.classList.add("ok");
      logEl.textContent =
        `完成：${result.title}\n` +
        `文件：${result.file || result.id + ".ebook"}\n` +
        `book_id：${result.book_id || result.extra?.book_id || "—"}\n` +
        `${result.chapters} 章 · toc=${result.has_toc ? "yes" : "none"} · cover=${result.has_cover ? "yes" : "none"}\n` +
        (result.extra?.keep_color
          ? "图片：保留彩色 JPEG（设备 Bayer 转黑白）\n"
          : result.extra?.binarize
            ? `二值化：${result.extra.binarize}\n`
            : "") +
        (result.warnings?.length ? result.warnings.join("\n") : "");

      const dl = $("#btn-dl-ebook");
      if (dl) {
        dl.href = `/api/book/${encodeURIComponent(result.id)}/download`;
        dl.setAttribute("download", result.file || `${result.id}.ebook`);
      }
      if (actions) actions.hidden = false;
      state.lastConvertedId = result.id;
      refreshLibrary();
    } catch (err) {
      status.classList.add("err");
      msgEl.textContent = "失败";
      logEl.textContent = String(err.message || err);
      if (actions) actions.hidden = true;
    } finally {
      btn.disabled = false;
    }
  });

  $("#btn-goto-emulator")?.addEventListener("click", async () => {
    if (!state.lastConvertedId) return;
    await openInEmulator(state.lastConvertedId);
  });

  function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
  }

  async function refreshLibrary() {
    const grid = $("#book-grid");
    grid.innerHTML = `<div class="empty">加载中…</div>`;
    const data = await fetchJson("/api/library");
    if (!data.books?.length) {
      grid.innerHTML = `<div class="empty">书库为空<br><span class="muted">右侧转换或拖入 .ebook</span></div>`;
      updateLibraryCount(0);
      return;
    }
    updateLibraryCount(data.books.length);
    grid.innerHTML = "";
    for (const b of data.books) {
      const card = document.createElement("div");
      card.className = "book-card";
      card.dataset.id = b.id;
      if (b.id === state.readingBookId) card.classList.add("active");
      const mtime = b.mtime || 0;
      const thumb = b.has_cover
        ? `<div class="thumb"><img src="/api/book/${encodeURIComponent(b.id)}/cover?w=96&h=128&t=${mtime}" alt="" loading="lazy" width="48" height="64" /></div>`
        : `<div class="thumb">—</div>`;
      card.innerHTML = `
        ${thumb}
        <div class="body">
          <h3 title="${escapeAttr(b.title)}">${escapeHtml(b.title)}</h3>
          <div class="meta-line">${escapeHtml(b.author || "未知")} · ${b.chapters}章</div>
          ${
            b.book_id
              ? `<div class="meta-line book-id-line" title="${escapeAttr(b.book_id)}">id ${escapeHtml(b.book_id)}</div>`
              : ""
          }
          <div class="actions">
            <button class="btn primary" data-open="${escapeAttr(b.id)}" type="button">阅读</button>
            <button class="btn ghost" data-cover="${escapeAttr(b.id)}" type="button" title="替换或注入封面">封面</button>
            <a class="btn ghost" href="/api/book/${encodeURIComponent(b.id)}/download" download="${escapeAttr(b.file || b.id + ".ebook")}" title="下载">↓</a>
            <button class="btn ghost danger" data-del="${escapeAttr(b.id)}" type="button" title="删除">×</button>
          </div>
        </div>`;
      grid.appendChild(card);
    }
    grid.querySelectorAll("[data-open]").forEach((btn) =>
      btn.addEventListener("click", () => openInEmulator(btn.getAttribute("data-open")))
    );
    grid.querySelectorAll("[data-cover]").forEach((btn) =>
      btn.addEventListener("click", () => pickAndSetCover(btn.getAttribute("data-cover")))
    );
    grid.querySelectorAll("[data-del]").forEach((btn) =>
      btn.addEventListener("click", async () => {
        if (!confirm("删除这本书？")) return;
        await fetch(`/api/book/${encodeURIComponent(btn.getAttribute("data-del"))}`, { method: "DELETE" });
        if (state.readingBookId === btn.getAttribute("data-del")) {
          state.readingBookId = null;
          hideTocPanel();
        }
        refreshLibrary();
      })
    );
  }

  async function pickAndSetCover(bookId) {
    if (!bookId) return;
    const input = document.createElement("input");
    input.type = "file";
    input.accept = "image/*,.jpg,.jpeg,.png,.webp,.bmp";
    input.addEventListener("change", async () => {
      const file = input.files?.[0];
      if (!file) return;
      try {
        const fd = new FormData();
        fd.append("cover_file", file);
        const methodEl = $("#binarize_method");
        if (methodEl) fd.set("binarize_method", methodEl.value);
        const keep = $("#keep_color");
        if (keep?.checked) fd.set("keep_color", "1");
        const data = await fetchJson(`/api/book/${encodeURIComponent(bookId)}/cover`, {
          method: "POST",
          body: fd,
        });
        if (!data.ok) throw new Error(data.error || "设置封面失败");
        await refreshLibrary();
      } catch (err) {
        alert(String(err.message || err));
      }
    });
    input.click();
  }
  $("#btn-refresh").addEventListener("click", refreshLibrary);
  $("#btn-clear-library")?.addEventListener("click", async () => {
    if (!confirm("确定清空书库？将删除全部已转换的 .ebook，不可恢复。")) return;
    try {
      const data = await fetchJson("/api/library", { method: "DELETE" });
      if (!data.ok) throw new Error(data.error || "清空失败");
      if (state.lastConvertedId) {
        state.lastConvertedId = null;
      }
      await refreshLibrary();
      alert(`已删除 ${data.removed ?? 0} 本`);
    } catch (err) {
      alert(String(err.message || err));
    }
  });

  async function openEbookFile(file) {
    if (!file) return;
    const name = (file.name || "").toLowerCase();
    if (!name.endsWith(".ebook")) {
      throw new Error("请选择 .ebook 文件");
    }
    const hint = $("#open-ebook-hint");
    if (hint) hint.textContent = `正在打开 ${file.name}…`;
    const fd = new FormData();
    fd.append("file", file);
    const data = await fetchJson("/api/library/open", { method: "POST", body: fd });
    if (!data.ok) throw new Error(data.error || "打开失败");
    await refreshLibrary();
    await openInEmulator(data.id);
    if (hint) hint.textContent = `已导入 ${file.name}`;
  }

  $("#open-ebook-file")?.addEventListener("change", async (e) => {
    const file = e.target.files?.[0];
    e.target.value = "";
    if (!file) return;
    try {
      await openEbookFile(file);
    } catch (err) {
      alert(String(err.message || err));
    }
  });

  const bookGrid = $("#book-grid");
  if (bookGrid) {
    bookGrid.addEventListener("dragenter", (e) => {
      e.preventDefault();
      bookGrid.classList.add("drag-over");
    });
    bookGrid.addEventListener("dragover", (e) => {
      e.preventDefault();
      bookGrid.classList.add("drag-over");
    });
    bookGrid.addEventListener("dragleave", (e) => {
      if (!bookGrid.contains(e.relatedTarget)) {
        bookGrid.classList.remove("drag-over");
      }
    });
    bookGrid.addEventListener("drop", async (e) => {
      e.preventDefault();
      bookGrid.classList.remove("drag-over");
      const file = [...(e.dataTransfer?.files || [])].find((f) =>
        (f.name || "").toLowerCase().endsWith(".ebook")
      );
      if (!file) {
        alert("请拖入 .ebook 文件");
        return;
      }
      try {
        await openEbookFile(file);
      } catch (err) {
        alert(String(err.message || err));
      }
    });
  }


  function escapeHtml(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }
  function escapeAttr(s) {
    return escapeHtml(s).replace(/'/g, "&#39;");
  }

  fetchJson("/api/config", {}, { failOnHttpError: false })
    .then((cfg) => {
      if (cfg?.max_upload_mb) state.maxUploadMb = cfg.max_upload_mb;
    })
    .catch(() => {});

  refreshLibrary();
