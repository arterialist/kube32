/* ==============================================================================
   Kube32 Material 3 Mermaid Initializer
   Seamlessly integrates Mermaid diagrams with Material for MkDocs
   Supports instant navigation (SPA), light/dark theme switching, and M3 styling
   ============================================================================== */

function initMermaidDiagrams() {
  if (typeof mermaid === "undefined") {
    return;
  }

  const isDark = document.body.getAttribute("data-md-color-scheme") === "slate";

  mermaid.initialize({
    startOnLoad: false,
    securityLevel: "loose",
    theme: isDark ? "dark" : "default",
    themeVariables: {
      primaryColor: isDark ? "#005236" : "#aaf2cc",
      primaryTextColor: isDark ? "#aaf2cc" : "#002517",
      primaryBorderColor: isDark ? "#8ed5b0" : "#226a4c",
      lineColor: isDark ? "#8ed5b0" : "#226a4c",
      secondaryColor: isDark ? "#364b40" : "#cfe7d7",
      tertiaryColor: isDark ? "#244c5a" : "#bee9fb",
      background: isDark ? "#0f1511" : "#f5fbf5",
      nodeBorder: isDark ? "#8ed5b0" : "#226a4c",
      clusterBkg: isDark ? "#171d19" : "#f0f5ef",
      clusterBorder: isDark ? "#353d38" : "#cdd7cf",
      fontFamily: "'Google Sans Flex', system-ui, -apple-system, sans-serif"
    }
  });

  // Convert any <pre class="mermaid"><code> blocks to <div class="mermaid"> while caching source
  const pres = document.querySelectorAll("pre.mermaid");
  pres.forEach(pre => {
    const raw = pre.textContent.trim();
    const div = document.createElement("div");
    div.className = "mermaid";
    div.setAttribute("data-source", raw);
    div.textContent = raw;
    pre.parentNode.replaceChild(div, pre);
  });

  // Run mermaid on all unrendered .mermaid containers
  const containers = document.querySelectorAll(".mermaid:not([data-processed='true'])");
  if (containers.length > 0) {
    mermaid.run({
      nodes: Array.from(containers)
    });
  }
}

function handleThemeChange() {
  document.querySelectorAll(".mermaid").forEach(div => {
    if (div.hasAttribute("data-source")) {
      div.removeAttribute("data-processed");
      div.innerHTML = "";
      div.textContent = div.getAttribute("data-source");
    }
  });
  initMermaidDiagrams();
}

// Hook into Material for MkDocs instant navigation observable
if (typeof document$ !== "undefined") {
  document$.subscribe(initMermaidDiagrams);
} else {
  document.addEventListener("DOMContentLoaded", initMermaidDiagrams);
}

// Observe body for instant light/dark theme switches
const themeObserver = new MutationObserver(mutations => {
  for (const m of mutations) {
    if (m.type === "attributes" && m.attributeName === "data-md-color-scheme") {
      handleThemeChange();
      break;
    }
  }
});
themeObserver.observe(document.body, { attributes: true, attributeFilter: ["data-md-color-scheme"] });
