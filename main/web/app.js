let toastSequence = 0;

function el(id) {
  return document.getElementById(id);
}

function streamUrl() {
  return location.protocol + "//" + location.hostname + ":81/stream";
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function removeToast(toast) {
  if (!toast || toast.dataset.closing === "1") {
    return;
  }

  toast.dataset.closing = "1";
  if (toast._toastTimer) {
    clearTimeout(toast._toastTimer);
    toast._toastTimer = null;
  }
  toast.classList.remove("toast_visible");
  setTimeout(() => {
    if (toast.parentNode) {
      toast.parentNode.removeChild(toast);
    }
  }, 220);
}

function setToastClosable(toast, closable) {
  const close = toast.querySelector(".toast_close");
  if (close) {
    close.style.display = closable ? "" : "none";
  }
}

function scheduleToastRemoval(toast, delayMs) {
  if (toast._toastTimer) {
    clearTimeout(toast._toastTimer);
  }
  toast._toastTimer = setTimeout(() => {
    removeToast(toast);
  }, delayMs);
}

function createToast(message, variant) {
  const toast = document.createElement("div");
  toast.className = "toast toast_" + variant;
  toast.dataset.toastId = String((toastSequence += 1));

  const messageNode = document.createElement("div");
  messageNode.className = "toast_message";
  messageNode.textContent = message;

  const close = document.createElement("button");
  close.type = "button";
  close.className = "toast_close";
  close.textContent = "閉じる";
  close.addEventListener("click", () => {
    removeToast(toast);
  });

  toast.appendChild(messageNode);
  toast.appendChild(close);
  el("toast_stack").prepend(toast);
  requestAnimationFrame(() => {
    toast.classList.add("toast_visible");
  });
  setToastClosable(toast, variant === "error");
  if (variant === "success") {
    scheduleToastRemoval(toast, 5000);
  }
  return toast;
}

function updateToast(toast, message, variant) {
  if (!toast || !toast.isConnected) {
    return createToast(message, variant);
  }

  toast.className = "toast toast_" + variant + " toast_visible";
  toast.dataset.closing = "0";
  const messageNode = toast.querySelector(".toast_message");
  if (messageNode) {
    messageNode.textContent = message;
  }
  if (toast._toastTimer) {
    clearTimeout(toast._toastTimer);
    toast._toastTimer = null;
  }
  setToastClosable(toast, variant === "error");
  if (variant === "success") {
    scheduleToastRemoval(toast, 5000);
  }
  return toast;
}

function triggerDownload(blob, name) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = name;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}

async function downloadBlob(url, name) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(url + " " + response.status);
  }
  const blob = await response.blob();
  triggerDownload(blob, name);
}

async function fetchManifestPage(page, pageSize) {
  const response = await fetch(
    "/api/export/manifest?page=" + page + "&page_size=" + pageSize,
  );
  if (!response.ok) {
    throw new Error("manifest " + response.status);
  }
  return await response.json();
}

async function fetchAllManifestItems() {
  const pageSize = 50;
  let page = 1;
  let items = [];

  while (true) {
    const data = await fetchManifestPage(page, pageSize);
    items = items.concat(data.items || []);
    if (!data.has_next) {
      break;
    }
    page += 1;
  }

  return items;
}

async function refreshStatus() {
  try {
    const response = await fetch("/api/status");
    const json = await response.json();
    el("status").textContent = JSON.stringify(json, null, 2);
  } catch (error) {
    el("status").textContent = "status 取得失敗\n" + String(error);
  }
}

function payload() {
  return {
    subject_id: el("subject_id").value.trim(),
    session_id: el("session_id").value.trim(),
    location_id: el("location_id").value.trim(),
    lighting_id: el("lighting_id").value.trim(),
    camera_position_id: el("camera_position_id").value.trim(),
    annotator_id: el("annotator_id").value.trim(),
    label: Number(el("label").value),
    is_usable_for_training: Number(el("is_usable_for_training").value),
    exclude_reason: el("exclude_reason").value.trim(),
    notes: el("notes").value,
  };
}

async function captureSample() {
  const button = el("capture_button");
  let toast;

  button.disabled = true;
  toast = createToast("撮影リクエスト送信中", "info");
  try {
    const response = await fetch("/api/capture", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload()),
    });
    const text = await response.text();
    if (!response.ok) {
      updateToast(
        toast,
        "撮影失敗\nHTTP " + response.status + "\n" + text,
        "error",
      );
      throw new Error("capture failed");
    }
    updateToast(
      toast,
      "撮影完了\nHTTP " + response.status + "\n" + text,
      "success",
    );
    await refreshStatus();
  } catch (error) {
    if (
      !toast ||
      !toast.isConnected ||
      !toast.classList.contains("toast_error")
    ) {
      updateToast(toast, "撮影失敗\n" + String(error), "error");
    }
  } finally {
    button.disabled = false;
  }
}

async function exportDataset() {
  const button = el("export_button");
  let toast;

  button.disabled = true;
  try {
    toast = createToast("metadata.csv を取得中", "info");
    await downloadBlob("/api/export/metadata", "metadata.csv");
    const items = await fetchAllManifestItems();
    const manifestBlob = new Blob(
      [JSON.stringify({ total_samples: items.length, items: items }, null, 2)],
      { type: "application/json" },
    );
    triggerDownload(manifestBlob, "manifest.json");
    for (let i = 0; i < items.length; i += 1) {
      const item = items[i];
      updateToast(
        toast,
        "画像を取得中 " +
          (i + 1) +
          " / " +
          items.length +
          "\n" +
          item.capture_id,
        "info",
      );
      await downloadBlob(
        "/api/export/image?capture_id=" + encodeURIComponent(item.capture_id),
        item.capture_id + ".jpg",
      );
      await sleep(150);
    }
    updateToast(
      toast,
      "エクスポート完了\nmetadata.csv, manifest.json, 画像 " +
        items.length +
        " 件",
      "success",
    );
  } catch (error) {
    updateToast(toast, "エクスポート失敗\n" + String(error), "error");
  } finally {
    button.disabled = false;
  }
}

async function resetDataset() {
  if (!confirm("dataset を削除します")) {
    return;
  }

  const toast = createToast("SDカードリセットを実行中", "info");
  const response = await fetch("/api/reset", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ confirm: "RESET" }),
  });
  const text = await response.text();
  updateToast(
    toast,
    (response.ok ? "SDカードリセット完了" : "SDカードリセット失敗") +
      "\nHTTP " +
      response.status +
      "\n" +
      text,
    response.ok ? "success" : "error",
  );
  await refreshStatus();
}

function bindEvents() {
  el("capture_button").addEventListener("click", captureSample);
  el("export_button").addEventListener("click", exportDataset);
  el("reset_button").addEventListener("click", resetDataset);
}

function initialize() {
  el("stream_view").src = streamUrl();
  bindEvents();
  refreshStatus();
}

initialize();
