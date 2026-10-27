/* Kube32 Blueprint Mermaid Initializer */
document.addEventListener("DOMContentLoaded", function () {
  if (typeof mermaid === "undefined") {
    return;
  }

  const isDark = document.body.getAttribute("data-md-color-scheme") === "slate";

  mermaid.initialize({
    startOnLoad: true,
    theme: isDark ? "dark" : "default",
    themeVariables: {
      primaryColor: isDark ? "#0c4a6e" : "#e0f2fe",
      primaryTextColor: isDark ? "#38bdf8" : "#0369a1",
      primaryBorderColor: isDark ? "#38bdf8" : "#0284c7",
      lineColor: isDark ? "#38bdf8" : "#0284c7",
      secondaryColor: isDark ? "#164e63" : "#cffafe",
      tertiaryColor: isDark ? "#0f172a" : "#f0f9ff",
      fontFamily: "'Google Sans Flex', -apple-system, sans-serif"
    }
  });
});
