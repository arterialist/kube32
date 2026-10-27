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
      primaryColor: isDark ? "#0842a0" : "#d3e3fd",
      primaryTextColor: isDark ? "#d3e3fd" : "#041e49",
      primaryBorderColor: isDark ? "#a8c7fa" : "#0b57d0",
      lineColor: isDark ? "#a8c7fa" : "#0b57d0",
      secondaryColor: isDark ? "#004a75" : "#cce5ff",
      tertiaryColor: isDark ? "#181c20" : "#f2f5f9",
      background: isDark ? "#101418" : "#f8fafd",
      nodeBorder: isDark ? "#a8c7fa" : "#0b57d0",
      clusterBkg: isDark ? "#181c20" : "#f2f5f9",
      clusterBorder: isDark ? "#41474d" : "#c2c7cf",
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
